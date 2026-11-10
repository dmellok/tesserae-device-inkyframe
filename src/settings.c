#include "settings.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "hardware/regs/addressmap.h"

#include "config.h"
#include "log.h"

// Two sectors just below the state record's two. One record per sector: a save
// erases and programs the sector NOT holding the current record, then verifies
// it, so a reset mid-save always leaves one valid copy.
#define SECTOR_OFFSET   FLASH_SETTINGS_OFFSET
#define MAGIC           0x53455454u   // "SETT"
#define VERSION         2u

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t seq;
    char ssid[SETTINGS_SSID_MAX];
    char pass[SETTINGS_PASS_MAX];
    char server_url[SETTINGS_URL_MAX];
    char fallback_ip[SETTINGS_IP_MAX];
    char pairing_code[SETTINGS_CODE_MAX];
    uint8_t verified;
    uint8_t reserved[3];
    // Version 2: the cloud relay (relay.c). The code and private scalar live
    // only until pairing completes; then the token and frame key take over.
    char relay_url[96];
    char relay_code[SETTINGS_CODE_MAX];
    char relay_install[48];
    char relay_device[48];
    char relay_token[128];
    char relay_config_etag[48];
    uint8_t relay_priv[32];
    uint8_t relay_key[32];
    uint8_t relay_has_priv;
    uint8_t relay_has_key;
    uint8_t relay_pad[2];
    uint32_t crc;
} record_t;

// Version 1 is this record cut short where the relay fields begin; its CRC
// sat at the next 4-byte boundary (after 2 bytes of padding), and covered
// everything before it.
#define CRC_OFFSET_V1 ((offsetof(record_t, relay_url) + 3u) & ~3u)
#define RECORD_BYTES  (FLASH_PAGE_SIZE * 4)

_Static_assert(sizeof(record_t) <= RECORD_BYTES, "settings record too large");

static record_t s_rec;
static int s_sector = -1;   // sector holding the current record, -1 if none

static uint32_t crc32(const void *data, size_t len)
{
    const uint8_t *p = data;
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++) crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1)));
    }
    return ~crc;
}

static const record_t *sector_ptr(int i)
{
    return (const record_t *)(FLASH_READ_BASE + SECTOR_OFFSET + (uint32_t)i * FLASH_SECTOR_SIZE);
}

static bool valid(const record_t *r)
{
    return r->magic == MAGIC && r->version == VERSION && r->crc == crc32(r, offsetof(record_t, crc));
}

static bool valid_v1(const record_t *r)
{
    if (r->magic != MAGIC || r->version != 1u) return false;
    uint32_t stored;
    memcpy(&stored, (const uint8_t *)r + CRC_OFFSET_V1, sizeof stored);
    return stored == crc32(r, CRC_OFFSET_V1);
}

static void copy(char *dst, size_t cap, const char *src)
{
    snprintf(dst, cap, "%s", src ? src : "");
}

static bool write_record(void);

static void seed_from_build(void)
{
    memset(&s_rec, 0, sizeof s_rec);
    s_rec.magic = MAGIC;
    s_rec.version = VERSION;
    copy(s_rec.ssid, sizeof s_rec.ssid, WIFI_SSID);
    copy(s_rec.pass, sizeof s_rec.pass, WIFI_PASS);
    copy(s_rec.server_url, sizeof s_rec.server_url, TESSERAE_SERVER_URL);
    copy(s_rec.fallback_ip, sizeof s_rec.fallback_ip, TESSERAE_FALLBACK_IP);
    copy(s_rec.pairing_code, sizeof s_rec.pairing_code, TESSERAE_PAIRING_CODE);
}

void settings_begin(void)
{
    uint32_t best = 0;
    bool old = false;
    for (int i = 0; i < 2; i++) {
        const record_t *r = sector_ptr(i);
        bool cur = valid(r), v1 = !cur && valid_v1(r);
        if ((cur || v1) && (s_sector < 0 || r->seq >= best)) { best = r->seq; s_sector = i; old = v1; }
    }
    if (s_sector >= 0) {
        memset(&s_rec, 0, sizeof s_rec);
        memcpy(&s_rec, sector_ptr(s_sector), old ? offsetof(record_t, relay_url) : sizeof s_rec);
        if (old) {
            // Rewrite it now (boot, radio down), so the old layout is gone.
            s_rec.version = VERSION;
            LOGI("settings: migrated a version 1 record");
            write_record();
        }
        LOGI("settings: loaded (ssid '%s', server '%s', %s)", s_rec.ssid, s_rec.server_url,
             s_rec.verified ? "verified" : "not yet verified");
    } else {
        seed_from_build();
        LOGI("settings: none saved; %s", settings_configured() ? "using the build's defaults" : "setup needed");
    }
}

static bool write_record(void)
{
    int target = s_sector < 0 ? 0 : 1 - s_sector;
    s_rec.seq++;
    s_rec.crc = crc32(&s_rec, offsetof(record_t, crc));
    static uint8_t buf[RECORD_BYTES];
    memset(buf, 0xFF, sizeof buf);
    memcpy(buf, &s_rec, sizeof s_rec);

    uint32_t off = SECTOR_OFFSET + (uint32_t)target * FLASH_SECTOR_SIZE;
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(off, FLASH_SECTOR_SIZE);
    flash_range_program(off, buf, sizeof buf);
    restore_interrupts(ints);

    bool ok = valid(sector_ptr(target));
    LOGI("settings: %s seq %lu to sector %d", ok ? "saved" : "FAILED to save", (unsigned long)s_rec.seq, target);
    if (ok) s_sector = target;
    return ok;
}

bool settings_configured(void) { return s_rec.ssid[0] && (s_rec.server_url[0] || settings_relay_mode()); }
const char *settings_ssid(void) { return s_rec.ssid; }
const char *settings_pass(void) { return s_rec.pass; }
const char *settings_server_url(void) { return s_rec.server_url; }
const char *settings_fallback_ip(void) { return s_rec.fallback_ip; }
const char *settings_pairing_code(void) { return s_rec.pairing_code; }
bool settings_verified(void) { return s_rec.verified; }

bool settings_save(const char *ssid, const char *pass, const char *server_url, const char *pairing_code)
{
    bool changed_net = false;
    if (ssid && strcmp(ssid, s_rec.ssid) != 0) { copy(s_rec.ssid, sizeof s_rec.ssid, ssid); changed_net = true; }
    if (pass && strcmp(pass, s_rec.pass) != 0) { copy(s_rec.pass, sizeof s_rec.pass, pass); changed_net = true; }
    if (server_url) {
        char url[SETTINGS_URL_MAX];
        copy(url, sizeof url, server_url);
        size_t l = strlen(url);
        while (l && url[l - 1] == '/') url[--l] = 0;   // no trailing slash
        if (strcmp(url, s_rec.server_url) != 0) {
            copy(s_rec.server_url, sizeof s_rec.server_url, url);
            s_rec.fallback_ip[0] = 0;   // belonged to the old server
            changed_net = true;
        }
    }
    if (pairing_code) copy(s_rec.pairing_code, sizeof s_rec.pairing_code, pairing_code);
    if (changed_net) s_rec.verified = 0;
    return write_record();
}

void settings_set_verified(void)
{
    if (s_rec.verified) return;
    s_rec.verified = 1;
    write_record();
}

void settings_clear_pairing_code(void)
{
    if (!s_rec.pairing_code[0]) return;
    s_rec.pairing_code[0] = 0;
    write_record();
}

void settings_erase(void)
{
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(SECTOR_OFFSET, 2 * FLASH_SECTOR_SIZE);
    restore_interrupts(ints);
    s_sector = -1;
    seed_from_build();
    LOGW("settings: erased");
}

// ---- cloud relay ----

bool settings_relay_mode(void) { return s_rec.relay_code[0] || s_rec.relay_has_key; }
bool settings_relay_paired(void) { return s_rec.relay_has_key && s_rec.relay_token[0]; }
const char *settings_relay_url(void) { return s_rec.relay_url[0] ? s_rec.relay_url : RELAY_DEFAULT_URL; }
const char *settings_relay_code(void) { return s_rec.relay_code; }
const char *settings_relay_install(void) { return s_rec.relay_install; }
const char *settings_relay_device(void) { return s_rec.relay_device; }
const char *settings_relay_token(void) { return s_rec.relay_token; }
const char *settings_relay_config_etag(void) { return s_rec.relay_config_etag; }
const uint8_t *settings_relay_key(void) { return s_rec.relay_has_key ? s_rec.relay_key : NULL; }
const uint8_t *settings_relay_priv(void) { return s_rec.relay_has_priv ? s_rec.relay_priv : NULL; }

static bool s_relay_dirty;

void settings_learn_ip(const char *ip)
{
    if (!ip || !ip[0] || strlen(ip) >= SETTINGS_IP_MAX || strcmp(ip, s_rec.fallback_ip) == 0) return;
    copy(s_rec.fallback_ip, sizeof s_rec.fallback_ip, ip);
    LOGI("settings: remembering %s as the server's address", ip);
    s_relay_dirty = true;
}

static void wipe_relay_pairing(void)
{
    memset(s_rec.relay_install, 0, sizeof s_rec.relay_install);
    memset(s_rec.relay_device, 0, sizeof s_rec.relay_device);
    memset(s_rec.relay_token, 0, sizeof s_rec.relay_token);
    memset(s_rec.relay_config_etag, 0, sizeof s_rec.relay_config_etag);
    memset(s_rec.relay_priv, 0, sizeof s_rec.relay_priv);
    memset(s_rec.relay_key, 0, sizeof s_rec.relay_key);
    s_rec.relay_has_priv = s_rec.relay_has_key = 0;
}

bool settings_save_relay(const char *url, const char *code)
{
    copy(s_rec.relay_url, sizeof s_rec.relay_url, url && url[0] ? url : RELAY_DEFAULT_URL);
    size_t l = strlen(s_rec.relay_url);
    while (l && s_rec.relay_url[l - 1] == '/') s_rec.relay_url[--l] = 0;
    copy(s_rec.relay_code, sizeof s_rec.relay_code, code);
    wipe_relay_pairing();   // a new code means a new pairing
    s_rec.verified = 0;
    return write_record();
}

void settings_relay_set_priv(const uint8_t priv[32])
{
    memcpy(s_rec.relay_priv, priv, 32);
    s_rec.relay_has_priv = 1;
    s_relay_dirty = true;
}

void settings_relay_set_paired(const char *install, const char *device, const char *token, const uint8_t key[32])
{
    copy(s_rec.relay_install, sizeof s_rec.relay_install, install);
    copy(s_rec.relay_device, sizeof s_rec.relay_device, device);
    copy(s_rec.relay_token, sizeof s_rec.relay_token, token);
    memcpy(s_rec.relay_key, key, 32);
    s_rec.relay_has_key = 1;
    // The scalar and the code have done their only job.
    memset(s_rec.relay_priv, 0, sizeof s_rec.relay_priv);
    s_rec.relay_has_priv = 0;
    s_rec.relay_code[0] = 0;
    s_relay_dirty = true;
}

void settings_relay_set_config_etag(const char *etag)
{
    if (strcmp(etag, s_rec.relay_config_etag) == 0) return;
    copy(s_rec.relay_config_etag, sizeof s_rec.relay_config_etag, etag);
    s_relay_dirty = true;
}

void settings_relay_drop(bool keep_code)
{
    wipe_relay_pairing();
    if (!keep_code) s_rec.relay_code[0] = 0;
    s_relay_dirty = true;
}

// Written by settings_relay_* and settings_learn_ip.
void settings_commit(void)
{
    if (!s_relay_dirty) return;
    s_relay_dirty = false;
    write_record();
}
