#include "diag.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pico/rand.h"
#include "hardware/watchdog.h"

#include "config.h"
#include "http.h"
#include "json_num.h"
#include "log.h"
#include "psram.h"
#include "rtc.h"
#include "settings.h"
#include "storage.h"

static const char *s_wake = "unknown";
static uint32_t s_boot_epoch;
static bool s_watchdog_reset;

static const char *paint_error(uint8_t code)
{
    switch (code) {
    case DIAG_PAINT_TIMEOUT: return "panel refresh timed out";
    case DIAG_PAINT_INCOMPLETE: return "no complete frame to paint";
    default: return "paint failed";
    }
}

void diag_begin(const char *wake_name)
{
    s_wake = wake_name ? wake_name : "unknown";
    s_watchdog_reset = watchdog_caused_reboot();
    s_boot_epoch = pcf_time_valid() ? pcf_get_epoch() : 0;
}

uint32_t diag_add_status(cJSON *body)
{
    cJSON *logs = cJSON_AddObjectToObject(body, "logs");
    cJSON_AddNumberToObject(logs, "schema", 1);
    cJSON_AddNumberToObject(logs, "ring_bytes", LOG_RING_BYTES);
    uint32_t id, at; uint8_t code;
    storage_get_diag(&id, &at, &code);
    if (!id) return 0;
    cJSON *d = cJSON_AddObjectToObject(body, "diag");
    json_add_u64(d, "id", id);
    cJSON_AddStringToObject(d, "paint_error", paint_error(code));
    if (at) json_add_u64(d, "at", at);
    return id;
}

void diag_acknowledge(uint32_t sent_id)
{
    uint32_t id, at; uint8_t code;
    storage_get_diag(&id, &at, &code);
    if (sent_id && id == sent_id) storage_set_diag(0, 0, DIAG_NONE);
}

void diag_paint_failed(uint8_t code)
{
    uint32_t id = get_rand_32();
    if (!id) id = 1;
    storage_set_diag(id, pcf_time_valid() ? pcf_get_epoch() : 0, code);
    LOGW("diag: paint failure latched (%s)", paint_error(code));
}

// Replace every occurrence of `secret` in buf[0..n) with "[redacted]"-length
// padding, in place (the text keeps its length, so nothing shifts).
static void blank(char *buf, size_t n, const char *secret)
{
    size_t sl = secret ? strlen(secret) : 0;
    if (sl < 4) return;
    for (size_t i = 0; i + sl <= n; i++)
        if (memcmp(buf + i, secret, sl) == 0) { memset(buf + i, '*', sl); i += sl - 1; }
}

// "# tesserae-log v1 ..." plus `extra` (" key=value" pairs, or "").
static int log_header(char *out, size_t cap, const char *extra)
{
    return snprintf(out, cap, "# tesserae-log v1 fw=%s boot=%lu reset=%s wake=%s epoch=%lu%s\n", FW_VERSION,
                    (unsigned long)s_boot_epoch, s_watchdog_reset ? "watchdog" : "power", s_wake,
                    (unsigned long)(pcf_time_valid() ? pcf_get_epoch() : 0), extra);
}

// Blank the secrets and POST body[0..n) to /log, with `extra` header lines.
static int post_log(const char *base, const char *device_id, const char *token, char *body, size_t n,
                    const char *const *extra, int nextra, uint32_t timeout_ms)
{
    body[n] = 0;
    blank(body, n, token);
    blank(body, n, settings_pass());

    char url[256], auth[300];
    snprintf(url, sizeof url, "%s/api/v1/device/%s/log", base, device_id);
    snprintf(auth, sizeof auth, "Authorization: Bearer %s", token);
    const char *hdrs[6] = { auth, "Content-Type: text/plain; charset=utf-8" };
    int nh = 2;
    for (int i = 0; i < nextra && nh < 6; i++) hdrs[nh++] = extra[i];
    http_resp_t r;
    int code = http_request("POST", url, hdrs, nh, body, &r, NULL, NULL, NULL, timeout_ms, 0);
    http_resp_free(&r);
    return code;
}

void diag_upload_log(const char *base, const char *device_id, const char *token)
{
    if (!token || !token[0]) return;
    // header + ring, under the server's 64 KB cap
    size_t cap = LOG_RING_BYTES + 256;
    char *body = malloc(cap + 1);
    if (!body) { LOGW("diag: no memory for the log upload"); return; }
    int h = log_header(body, cap, "");
    size_t n = (size_t)h + log_ring_copy(body + h, cap - (size_t)h);
    int code = post_log(base, device_id, token, body, n, NULL, 0, HTTP_TIMEOUT_STATUS_MS);
    free(body);
    LOGI("diag: uploaded %u bytes of log (http %d)", (unsigned)n, code);
}

// ---- OTA logs ----

// The carried tail in PSRAM: this header, then `len` bytes of log text.
#define CARRY_MAGIC 0x474C544Fu   // "OTLG"
#define CARRY_DATA  (OTA_LOG_CARRY_PSRAM_OFFSET + 16u)
typedef struct { uint32_t magic, len, crc, check; } carry_hdr_t;

_Static_assert(LOG_RING_BYTES + 128 <= OTA_LOG_CARRY_BYTES, "a whole ring must fit the carry");
_Static_assert(OTA_LOG_CARRY_PSRAM_OFFSET + 16 + OTA_LOG_CARRY_BYTES <= 8u * 1024u * 1024u, "carry past the PSRAM");
_Static_assert(CACHE_PSRAM_OFFSET + FRAME_BYTES <= OTA_LOG_CARRY_PSRAM_OFFSET, "carry overlaps the card staging");

static uint32_t s_carry_len;    // bytes carried in from before the reboot
static bool s_carry_saved;      // diag_carry_save ran this boot
static uint32_t s_ota_mark;     // ring position up to which an OTA log went out

static uint32_t crc32_update(uint32_t crc, const uint8_t *p, size_t n)
{
    crc = ~crc;
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++) crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1)));
    }
    return ~crc;
}

static uint32_t carry_crc(uint32_t len)
{
    uint8_t buf[256];
    uint32_t crc = 0;
    for (uint32_t off = 0; off < len; off += sizeof buf) {
        uint32_t k = len - off < sizeof buf ? len - off : sizeof buf;
        psram_read(CARRY_DATA + off, buf, k);
        crc = crc32_update(crc, buf, k);
    }
    return crc;
}

static void carry_write_header(uint32_t len)
{
    carry_hdr_t h = { len ? CARRY_MAGIC : 0, len, len ? carry_crc(len) : 0, 0 };
    h.check = ~(h.magic ^ h.len ^ h.crc);
    psram_write(OTA_LOG_CARRY_PSRAM_OFFSET, (const uint8_t *)&h, sizeof h);
}

void diag_carry_begin(void)
{
    carry_hdr_t h;
    psram_read(OTA_LOG_CARRY_PSRAM_OFFSET, (uint8_t *)&h, sizeof h);
    s_carry_len = 0;
    if (h.magic != CARRY_MAGIC || h.check != ~(h.magic ^ h.len ^ h.crc) || !h.len || h.len > OTA_LOG_CARRY_BYTES) return;
    if (carry_crc(h.len) != h.crc) { LOGW("diag: the carried log failed its check; dropped"); carry_write_header(0); return; }
    s_carry_len = h.len;
    LOGI("diag: %lu bytes of log carried over the reboot", (unsigned long)h.len);
}

void diag_carry_save(void)
{
    if (s_carry_saved) return;
    s_carry_saved = true;
    char *buf = malloc(LOG_RING_BYTES + 128);
    if (!buf) { LOGW("diag: no memory to carry the log over the reboot"); return; }
    int h = snprintf(buf, 128, "# --- fw %s, %s, log to the reboot ---\n", FW_VERSION, s_wake);
    size_t n = (size_t)h + log_ring_copy_since(buf + h, LOG_RING_BYTES, s_ota_mark);
    // Keep the newest of (what was carried in + this boot) within the cap.
    uint32_t keep = s_carry_len;
    if (keep + n > OTA_LOG_CARRY_BYTES) {
        uint32_t drop = keep + (uint32_t)n - OTA_LOG_CARRY_BYTES;
        uint8_t chunk[256];
        for (uint32_t off = 0; off + drop < keep; off += sizeof chunk) {
            uint32_t k = keep - drop - off < sizeof chunk ? keep - drop - off : sizeof chunk;
            psram_read(CARRY_DATA + drop + off, chunk, k);
            psram_write(CARRY_DATA + off, chunk, k);
        }
        keep -= drop;
    }
    psram_write(CARRY_DATA + keep, (const uint8_t *)buf, n);
    free(buf);
    carry_write_header(keep + (uint32_t)n);
    LOGI("diag: %lu bytes of log saved for the next boot", (unsigned long)(keep + n));
}

bool diag_upload_ota_log(const char *base, const char *device_id, const char *token,
                         const char *phase, const char *target, const char *attempt)
{
    if (!base || !base[0] || !token || !token[0]) return false;
    // header + carried tail + this boot's ring; the tail gives way first.
    const size_t head_room = 512;
    uint32_t carry = s_carry_len;
    if (carry > OTA_LOG_BODY_MAX - head_room - LOG_RING_BYTES) carry = OTA_LOG_BODY_MAX - head_room - LOG_RING_BYTES;
    size_t cap = head_room + carry + LOG_RING_BYTES;
    char *body = malloc(cap + 1);
    if (!body && carry) { carry = 0; cap = head_room + LOG_RING_BYTES; body = malloc(cap + 1); }
    if (!body) { LOGW("diag: no memory for the OTA log"); return false; }

    char extra[96];
    snprintf(extra, sizeof extra, " kind=ota phase=%s target=%s attempt=%s", phase && phase[0] ? phase : "-",
             target && target[0] ? target : "-", attempt && attempt[0] ? attempt : "-");
    size_t n = (size_t)log_header(body, head_room, extra);
    if (carry) {
        n += (size_t)snprintf(body + n, head_room - n, "# --- carried over the reboot (%lu bytes) ---\n", (unsigned long)carry);
        // The newest `carry` bytes, from a whole line on.
        char *t = body + n;
        psram_read(CARRY_DATA + (s_carry_len - carry), (uint8_t *)t, carry);
        size_t got = carry;
        char *nl = carry < s_carry_len ? memchr(t, '\n', got) : NULL;
        if (nl) { size_t skip = (size_t)(nl - t) + 1; memmove(t, nl + 1, got - skip); got -= skip; }
        n += got;
        n += (size_t)snprintf(body + n, cap - n, "# --- this boot ---\n");
    }
    uint32_t mark = log_ring_mark();
    n += log_ring_copy(body + n, cap - n);
    for (size_t i = 0; i < n; i++) if (!body[i]) body[i] = '?';   // sent as a C string

    char h_phase[48], h_target[64], h_attempt[48];
    const char *hdrs[4] = { "X-Tesserae-Log-Kind: ota" };
    int nh = 1;
    if (phase && phase[0]) { snprintf(h_phase, sizeof h_phase, "X-Tesserae-Ota-Phase: %s", phase); hdrs[nh++] = h_phase; }
    if (target && target[0]) { snprintf(h_target, sizeof h_target, "X-Tesserae-Ota-Target: %s", target); hdrs[nh++] = h_target; }
    if (attempt && attempt[0]) { snprintf(h_attempt, sizeof h_attempt, "X-Tesserae-Ota-Attempt: %s", attempt); hdrs[nh++] = h_attempt; }
    int code = post_log(base, device_id, token, body, n, hdrs, nh, OTA_LOG_TIMEOUT_MS);
    free(body);
    bool ok = code >= 200 && code < 300;
    LOGI("diag: OTA log (%s) %u bytes, %lu carried: http %d", phase ? phase : "-", (unsigned)n, (unsigned long)carry, code);
    if (ok) {
        s_ota_mark = mark;   // a later carry starts after what went out
        if (s_carry_len) { s_carry_len = 0; carry_write_header(0); }
    }
    return ok;
}
