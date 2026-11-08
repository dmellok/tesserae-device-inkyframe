#include "cache.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pico/rand.h"

#include "config.h"
#include "deck.h"
#include "frame.h"
#include "frame_collection.h"
#include "http.h"
#include "json_num.h"
#include "log.h"
#include "psram.h"
#include "sdcard.h"
#include "sha256.h"
#include "vendor/fatfs/ff.h"

#define DIR_ROOT   "/tesserae"
#define DIR_DECK   "/tesserae/deck"
#define DIR_ALBUM  "/tesserae/album"
#define STATE_FILE "/tesserae/state.json"
#define MANIFEST_MAX (24 * 1024)

static bool s_enabled;

static deck_manifest_t s_deck;
static bool s_has_deck;
static char s_page[DECK_ID_CAP];        // the page on the glass, if a deck page
static bool s_page_report;              // served locally since the last check-in

static fc_manifest_t s_album;
static bool s_has_album;
static fc_play_state_t s_play;
static uint32_t s_album_deadline;       // epoch; 0 = start now
static const char *s_album_state = "idle";

static bool s_dirty;

// ---- files ----

static char *read_file(const char *path, size_t *len)
{
    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK) return NULL;
    FSIZE_t n = f_size(&f);
    if (n > MANIFEST_MAX) { f_close(&f); return NULL; }
    char *buf = malloc((size_t)n + 1);
    UINT got = 0;
    if (buf && f_read(&f, buf, (UINT)n, &got) == FR_OK && got == n) {
        buf[n] = 0;
        if (len) *len = (size_t)n;
    } else {
        free(buf);
        buf = NULL;
    }
    f_close(&f);
    return buf;
}

// Write via a temporary file and a rename, so a power cut never leaves half a file.
static bool write_file(const char *path, const void *data, size_t len)
{
    char tmp[96];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FIL f;
    if (f_open(&f, tmp, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return false;
    UINT put = 0;
    bool ok = f_write(&f, data, (UINT)len, &put) == FR_OK && put == len;
    ok = f_close(&f) == FR_OK && ok;
    if (ok) { f_unlink(path); ok = f_rename(tmp, path) == FR_OK; }
    if (!ok) f_unlink(tmp);
    return ok;
}

static void clear_dir(const char *dir)
{
    DIR d;
    FILINFO fi;
    if (f_opendir(&d, dir) != FR_OK) return;
    char path[300];
    while (f_readdir(&d, &fi) == FR_OK && fi.fname[0]) {
        snprintf(path, sizeof path, "%s/%s", dir, fi.fname);
        f_unlink(path);
    }
    f_closedir(&d);
}

static void frame_path(char *out, size_t cap, const char *dir, const char *digest)
{
    snprintf(out, cap, "%s/%s.bin", dir, digest);
}

// ---- a cached frame onto the glass ----

// Load <dir>/<digest>.bin into the PSRAM frame store, checking its length and
// digest on the way, and paint it.
static bool paint_cached(const char *dir, const char *digest, uint32_t bytes)
{
    if (bytes != FRAME_BYTES) return false;
    char path[96];
    frame_path(path, sizeof path, dir, digest);
    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK) return false;
    if (f_size(&f) != bytes) { f_close(&f); return false; }
    static uint8_t buf[4096];
    sha256_ctx_t sha;
    sha256_init(&sha);
    frame_set_complete(false);
    bool ok = true;
    for (uint32_t off = 0; ok && off < bytes; ) {
        UINT got = 0;
        ok = f_read(&f, buf, sizeof buf, &got) == FR_OK && got > 0;
        if (!ok) break;
        psram_write(off, buf, got);
        sha256_update(&sha, buf, got);
        off += got;
    }
    f_close(&f);
    uint8_t d[32]; char hex[DECK_DIGEST_HEX + 1];
    sha256_final(&sha, d);
    deck_digest_hex16(d, hex);
    if (!ok || strcmp(hex, digest) != 0) {
        LOGW("cache: %s failed its digest check; removing it", path);
        f_unlink(path);
        return false;
    }
    frame_set_complete(true);
    return frame_paint();
}

// ---- download one frame to the card ----

typedef struct { uint32_t want, got; bool length_ok; sha256_ctx_t sha; } dl_t;

static bool dl_length(size_t len, void *arg) { dl_t *d = arg; d->length_ok = len == d->want; return d->length_ok; }

static void dl_sink(const uint8_t *data, size_t len, void *arg)
{
    dl_t *d = arg;
    if (d->got + len > d->want) len = d->want - d->got;
    psram_write(CACHE_PSRAM_OFFSET + d->got, data, len);
    sha256_update(&d->sha, data, len);
    d->got += (uint32_t)len;
}

static bool fetch_to_card(const char *url, const char *token, const char *dir, const char *digest, uint32_t bytes)
{
    if (bytes != FRAME_BYTES) return false;
    char auth[300];
    snprintf(auth, sizeof auth, "Authorization: Bearer %s", token);
    const char *hdrs[] = { auth };
    static dl_t d;
    memset(&d, 0, sizeof d);
    d.want = bytes;
    sha256_init(&d.sha);
    http_resp_t r;
    int code = http_request("GET", url, hdrs, 1, NULL, &r, dl_length, dl_sink, &d, HTTP_TIMEOUT_DOWNLOAD_MS, DOWNLOAD_STALL_TIMEOUT_MS);
    http_resp_free(&r);
    uint8_t sha[32]; char hex[DECK_DIGEST_HEX + 1];
    sha256_final(&d.sha, sha);
    deck_digest_hex16(sha, hex);
    if (code != 200 || d.got != bytes || strcmp(hex, digest) != 0) {
        LOGW("cache: fetch of %s failed (http %d, %lu bytes, digest %s)", digest, code, (unsigned long)d.got, hex);
        return false;
    }
    char path[96], tmp[100];
    frame_path(path, sizeof path, dir, digest);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FIL f;
    if (f_open(&f, tmp, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return false;
    static uint8_t buf[4096];
    bool ok = true;
    for (uint32_t off = 0; ok && off < bytes; off += sizeof buf) {
        UINT n = bytes - off < sizeof buf ? bytes - off : sizeof buf, put = 0;
        psram_read(CACHE_PSRAM_OFFSET + off, buf, n);
        ok = f_write(&f, buf, n, &put) == FR_OK && put == n;
    }
    ok = f_close(&f) == FR_OK && ok;
    if (ok) { f_unlink(path); ok = f_rename(tmp, path) == FR_OK; }
    if (!ok) f_unlink(tmp);
    return ok;
}

static bool have_frame(const char *dir, const char *digest, uint32_t bytes)
{
    char path[96];
    frame_path(path, sizeof path, dir, digest);
    FILINFO fi;
    return f_stat(path, &fi) == FR_OK && fi.fsize == bytes;
}

// Delete <dir>/*.bin that `keep` does not name.
typedef bool (*keep_fn)(const char *digest);

static void prune(const char *dir, keep_fn keep)
{
    DIR d;
    FILINFO fi;
    if (f_opendir(&d, dir) != FR_OK) return;
    char path[300], digest[DECK_DIGEST_HEX + 1];
    while (f_readdir(&d, &fi) == FR_OK && fi.fname[0]) {
        size_t l = strlen(fi.fname);
        if (l != DECK_DIGEST_HEX + 4 || strcmp(fi.fname + DECK_DIGEST_HEX, ".bin") != 0) continue;
        memcpy(digest, fi.fname, DECK_DIGEST_HEX); digest[DECK_DIGEST_HEX] = 0;
        if (keep(digest)) continue;
        snprintf(path, sizeof path, "%s/%s", dir, fi.fname);
        f_unlink(path);
    }
    f_closedir(&d);
}

static bool deck_keeps(const char *digest)
{
    for (int i = 0; i < s_deck.n_pages; i++) if (strcmp(s_deck.pages[i].digest, digest) == 0) return true;
    return false;
}

static bool album_keeps(const char *digest) { return fc_find_digest(&s_album, digest) != NULL; }

// ---- state ----

static void save_state(void)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "page", s_page);
    json_add_u64(o, "album_deadline", s_album_deadline);
    cJSON *p = cJSON_AddObjectToObject(o, "play");
    cJSON_AddStringToObject(p, "version", s_play.version);
    cJSON_AddNumberToObject(p, "index", s_play.current_index);
    json_add_u64(p, "used", s_play.used_mask);
    json_add_u64(p, "rng", s_play.rng_state);
    cJSON_AddBoolToObject(p, "done", s_play.done);
    char *txt = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (txt) { write_file(STATE_FILE, txt, strlen(txt)); free(txt); }
}

static void load_state(void)
{
    char *txt = read_file(STATE_FILE, NULL);
    if (!txt) return;
    cJSON *o = cJSON_Parse(txt);
    free(txt);
    const char *page = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o, "page"));
    if (page) snprintf(s_page, sizeof s_page, "%s", page);
    const cJSON *dl = cJSON_GetObjectItemCaseSensitive(o, "album_deadline");
    if (cJSON_IsNumber(dl)) s_album_deadline = (uint32_t)dl->valuedouble;
    const cJSON *p = cJSON_GetObjectItemCaseSensitive(o, "play");
    if (cJSON_IsObject(p)) {
        const char *v = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(p, "version"));
        if (v) snprintf(s_play.version, sizeof s_play.version, "%s", v);
        const cJSON *x;
        if (cJSON_IsNumber(x = cJSON_GetObjectItemCaseSensitive(p, "index"))) s_play.current_index = (int16_t)x->valuedouble;
        if (cJSON_IsNumber(x = cJSON_GetObjectItemCaseSensitive(p, "used"))) s_play.used_mask = (uint32_t)x->valuedouble;
        if (cJSON_IsNumber(x = cJSON_GetObjectItemCaseSensitive(p, "rng"))) s_play.rng_state = (uint32_t)x->valuedouble;
        s_play.done = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(p, "done"));
    }
    cJSON_Delete(o);
}

void cache_commit(void)
{
    if (!s_enabled || !s_dirty) return;
    s_dirty = false;
    save_state();
}

// ---- boot ----

void cache_begin(void)
{
    if (!sdcard_mount()) return;
    f_mkdir(DIR_ROOT);
    f_mkdir(DIR_DECK);
    f_mkdir(DIR_ALBUM);
    s_enabled = true;
    fc_play_reset(&s_play, "", get_rand_32());
    size_t n;
    char *txt = read_file(DIR_DECK "/manifest.json", &n);
    if (txt) { s_has_deck = deck_manifest_parse(txt, n, &s_deck); free(txt); }
    txt = read_file(DIR_ALBUM "/manifest.json", &n);
    if (txt) { s_has_album = fc_manifest_parse(txt, n, &s_album); free(txt); }
    load_state();
    if (s_has_album) s_album_state = "playing";
    LOGI("cache: card ready%s%s", s_has_deck ? ", deck cached" : "", s_has_album ? ", album cached" : "");
}

bool cache_enabled(void) { return s_enabled; }

// ---- status ----


void cache_add_status(cJSON *body)
{
    if (!s_enabled) return;
    uint64_t free_b = sdcard_free_bytes();
    if (!free_b) return;
    // capacity_bytes is written as raw digits: cJSON prints numbers past
    // INT_MAX with "%g", which this firmware's printf cannot format, and a
    // card's free space is usually past 2^31. (Malformed output there made
    // the whole /status body invalid JSON.)
    cJSON *d = cJSON_AddObjectToObject(body, "deck_cache");
    cJSON_AddNumberToObject(d, "schema", 1);
    json_add_u64(d, "capacity_bytes", free_b);
    cJSON *f = cJSON_AddObjectToObject(body, "frame_cache");
    cJSON_AddNumberToObject(f, "schema", 1);
    json_add_u64(f, "capacity_bytes", free_b);
    if (s_page_report && s_has_deck) {
        cJSON_AddStringToObject(body, "deck_page_id", s_page);
        cJSON_AddStringToObject(body, "deck_version", s_deck.version);
    }
    if (s_has_album) {
        int cached = 0;
        for (int i = 0; i < s_album.n_frames; i++)
            if (have_frame(DIR_ALBUM, s_album.frames[i].digest, s_album.frames[i].bytes)) cached++;
        cJSON *c = cJSON_AddObjectToObject(body, "collection");
        cJSON_AddStringToObject(c, "id", s_album.collection_id);
        cJSON_AddStringToObject(c, "version", s_album.version);
        cJSON_AddNumberToObject(c, "cached", cached);
        cJSON_AddNumberToObject(c, "total", s_album.total_frames);
        cJSON_AddStringToObject(c, "state", s_album_state);
    }
}

const char *cache_frame_query(void)
{
    static char q[128];
    q[0] = 0;
    if (s_page_report && s_has_deck)
        snprintf(q, sizeof q, "deck_page_id=%s&deck_version=%s", s_page, s_deck.version);
    s_page_report = false;   // one report per check-in
    return q;
}

// ---- sync ----

static void sync_deck(const char *base, const char *device_id, const char *token)
{
    char url[256], auth[300];
    snprintf(url, sizeof url, "%s/api/v1/device/%s/deck", base, device_id);
    snprintf(auth, sizeof auth, "Authorization: Bearer %s", token);
    const char *hdrs[] = { auth };
    http_resp_t r;
    int code = http_request("GET", url, hdrs, 1, NULL, &r, NULL, NULL, NULL, HTTP_TIMEOUT_STATUS_MS, 0);
    if (code == 204) {
        http_resp_free(&r);
        if (s_has_deck) { LOGI("cache: deck unbound; clearing it"); clear_dir(DIR_DECK); s_has_deck = false; s_page[0] = 0; s_dirty = true; }
        return;
    }
    static deck_manifest_t m;
    if (code != 200 || !r.body || r.truncated || !deck_manifest_parse(r.body, r.body_len, &m)) {
        LOGW("cache: deck manifest unusable (http %d)", code);
        http_resp_free(&r);
        return;
    }
    bool new_deck = !s_has_deck || strcmp(m.deck_id, s_deck.deck_id) != 0;
    if (new_deck) clear_dir(DIR_DECK);
    int fetched = 0;
    for (int i = 0; i < m.n_pages; i++) {
        const deck_page_t *p = &m.pages[i];
        if (have_frame(DIR_DECK, p->digest, p->bytes)) continue;
        char furl[320];
        snprintf(furl, sizeof furl, "%s/api/v1/device/%s/deck/frame/%s", base, device_id, p->digest);
        if (fetch_to_card(furl, token, DIR_DECK, p->digest, p->bytes)) fetched++;
    }
    write_file(DIR_DECK "/manifest.json", r.body, r.body_len);
    http_resp_free(&r);
    s_deck = m;
    s_has_deck = true;
    prune(DIR_DECK, deck_keeps);
    if (new_deck || !deck_find_page(&s_deck, s_page)) snprintf(s_page, sizeof s_page, "%s", s_deck.entry_page_id);
    s_dirty = true;
    LOGI("cache: deck %s v%s synced (%d pages, %d fetched)", s_deck.deck_id, s_deck.version, s_deck.n_pages, fetched);
}

static void sync_album(const char *base, const char *device_id, const char *token)
{
    char url[256], auth[300];
    snprintf(url, sizeof url, "%s/api/v1/device/%s/collection", base, device_id);
    snprintf(auth, sizeof auth, "Authorization: Bearer %s", token);
    const char *hdrs[] = { auth };
    http_resp_t r;
    int code = http_request("GET", url, hdrs, 1, NULL, &r, NULL, NULL, NULL, HTTP_TIMEOUT_STATUS_MS, 0);
    if (code == 204) {
        http_resp_free(&r);
        if (s_has_album) { LOGI("cache: album unbound; clearing it"); clear_dir(DIR_ALBUM); s_has_album = false; s_album_state = "idle"; s_dirty = true; }
        return;
    }
    static fc_manifest_t m;
    if (code != 200 || !r.body || r.truncated || !fc_manifest_parse(r.body, r.body_len, &m)) {
        LOGW("cache: album manifest unusable (http %d)", code);
        http_resp_free(&r);
        return;
    }
    bool new_album = !s_has_album || strcmp(m.collection_id, s_album.collection_id) != 0;
    if (new_album) clear_dir(DIR_ALBUM);
    int fetched = 0;
    for (int i = 0; i < m.n_frames; i++) {
        const fc_frame_t *f = &m.frames[i];
        if (have_frame(DIR_ALBUM, f->digest, f->bytes) || !f->url[0]) continue;
        char furl[320];
        if (f->url[0] == '/') snprintf(furl, sizeof furl, "%s%s", base, f->url);
        else snprintf(furl, sizeof furl, "%s", f->url);
        if (fetch_to_card(furl, token, DIR_ALBUM, f->digest, f->bytes)) fetched++;
    }
    write_file(DIR_ALBUM "/manifest.json", r.body, r.body_len);
    http_resp_free(&r);
    if (new_album || strcmp(s_play.version, m.version) != 0) fc_play_reset(&s_play, m.version, get_rand_32());
    s_album = m;
    s_has_album = true;
    prune(DIR_ALBUM, album_keeps);
    s_album_state = "playing";
    s_dirty = true;
    LOGI("cache: album %s v%s synced (%d frames, %d fetched)", s_album.collection_id, s_album.version, s_album.n_frames, fetched);
}

void cache_note_status(const char *deck_version, bool has_deck,
                       const char *album_version, bool has_album,
                       const char *base, const char *device_id, const char *token)
{
    if (!s_enabled) return;
    if (has_deck && (!s_has_deck || strcmp(deck_version, s_deck.version) != 0)) sync_deck(base, device_id, token);
    else if (!has_deck && s_has_deck) { clear_dir(DIR_DECK); s_has_deck = false; s_page[0] = 0; s_dirty = true; LOGI("cache: deck unbound"); }
    if (has_album && (!s_has_album || strcmp(album_version, s_album.version) != 0)) sync_album(base, device_id, token);
    else if (!has_album && s_has_album) { clear_dir(DIR_ALBUM); s_has_album = false; s_album_state = "idle"; s_dirty = true; LOGI("cache: album unbound"); }
}

// ---- local serving ----

bool cache_local_nav(const char *button)
{
    if (!s_enabled || !s_has_deck || !button) return false;
    const char *cur = s_page[0] ? s_page : s_deck.entry_page_id;
    const char *target = deck_nav_button(&s_deck, cur, button);
    if (!target) return false;
    const deck_page_t *p = deck_find_page(&s_deck, target);
    if (!p || !paint_cached(DIR_DECK, p->digest, p->bytes)) return false;
    snprintf(s_page, sizeof s_page, "%s", target);
    s_page_report = true;
    s_dirty = true;
    LOGI("cache: deck page %s painted from the card (no server round trip)", target);
    return true;
}

static int32_t album_interval(void)
{
    return fc_interval_clamp(s_album.album.interval_s, ALBUM_MIN_INTERVAL_S, ALBUM_MAX_INTERVAL_S, ALBUM_DEFAULT_INTERVAL_S);
}

void cache_note_network_frame(uint32_t now)
{
    if (!s_has_album) return;
    s_album_deadline = (uint32_t)fc_interrupt_resume_at(s_album_deadline, now, album_interval());
    s_album_state = "interrupted";
    s_dirty = true;
}

bool cache_album_due(uint32_t now)
{
    if (!s_enabled || !s_has_album || now < 1600000000u || now < s_album_deadline) return false;
    int idx;
    if (!fc_play_next(&s_album, &s_play, &idx)) { s_album_state = "held"; s_dirty = true; return false; }
    const fc_frame_t *f = &s_album.frames[idx];
    s_album_deadline = now + (uint32_t)album_interval();
    s_dirty = true;
    if (!paint_cached(DIR_ALBUM, f->digest, f->bytes)) return false;
    s_album_state = "playing";
    LOGI("cache: album frame %s painted from the card; next in %ld s", f->frame_id, (long)album_interval());
    return true;
}

int32_t cache_album_wait_s(uint32_t now)
{
    if (!s_enabled || !s_has_album || s_play.done) return -1;
    if (s_album_deadline <= now) return 0;
    return (int32_t)(s_album_deadline - now);
}
