#include "storage.h"

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "hardware/regs/addressmap.h"

#include "config.h"
#include "log.h"

// Two sectors, used alternately. A record is always programmed into the
// already-erased spare before the old sector is erased, so there is never an
// instant with no valid copy on flash: a reset or brownout mid-write (the
// commit runs just before VSYS is dropped, when a tired battery is most likely
// to sag) leaves the previous record intact and the device stays paired.
#define SECTOR_COUNT    2u
#define SECTOR_OFFSET   FLASH_STATE_OFFSET
#define RECORD_SIZE     512u
#define SLOTS_PER_SECTOR (FLASH_SECTOR_SIZE / RECORD_SIZE)
#define SLOTS           (SLOTS_PER_SECTOR * SECTOR_COUNT)
#define MAGIC           0x494E4B59u   // "INKY"
#define VERSION         3u

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t seq;
    char token[256];
    char device_id[40];
    char etag[80];
    uint32_t event_id;
    int32_t sleep_s;
    int32_t button_wake_s;
    int32_t awake_poll_s;
    uint8_t always_on;
    uint8_t backoff;
    uint8_t fallback_kind;
    uint8_t reserved;          // bit 0: portal offered this outage; bit 1: OTA log key seeded
    // OTA report (ota.c), carried across reboots and the A/B switch.
    uint8_t ota_phase;
    uint8_t ota_reason;
    // The report the last OTA log went out for (phase, reason; the attempt as
    // a 24-bit hash in what was diag padding). Kept in former padding so the
    // layout, and an older firmware a rollback returns to, are unchanged.
    uint8_t ota_log_phase;
    uint8_t ota_log_reason;
    char ota_target[24];
    char ota_attempt[12];
    char ota_detail[40];
    // Version 3: the last paint failure, re-sent on /status until accepted.
    uint32_t diag_id;          // 0 = none
    uint32_t diag_at;
    uint8_t diag_code;
    uint8_t ota_log_attempt[3];
    uint32_t crc;
} record_t;

// Each version only appended fields before the CRC, so an older record is the
// current one cut short: its CRC sits where the newer fields begin.
#define CRC_OFFSET_V1 offsetof(record_t, ota_phase)
#define CRC_OFFSET_V2 offsetof(record_t, diag_id)

_Static_assert(sizeof(record_t) <= RECORD_SIZE, "record too large");
_Static_assert(offsetof(record_t, ota_target) == offsetof(record_t, ota_phase) + 4, "OTA log key moved the layout");
_Static_assert(offsetof(record_t, crc) == offsetof(record_t, diag_code) + 4, "OTA log key moved the layout");

static record_t s_rec;
static bool s_dirty;
static int s_slot = -1;   // slot holding the current record, -1 if none

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

static const record_t *slot_ptr(int i)
{
    return (const record_t *)(FLASH_READ_BASE + SECTOR_OFFSET + (uint32_t)i * RECORD_SIZE);
}

static bool slot_valid(const record_t *r)
{
    return r->magic == MAGIC && r->version == VERSION && r->crc == crc32(r, offsetof(record_t, crc));
}

// An older record (version 1 or 2) still valid under its own layout; returns
// the length of its fields, or 0.
static size_t slot_old_len(const record_t *r)
{
    if (r->magic != MAGIC) return 0;
    size_t off = r->version == 1u ? CRC_OFFSET_V1 : r->version == 2u ? CRC_OFFSET_V2 : 0;
    if (!off) return 0;
    uint32_t stored;
    memcpy(&stored, (const uint8_t *)r + off, sizeof stored);
    return stored == crc32(r, off) ? off : 0;
}

static void defaults(void)
{
    memset(&s_rec, 0, sizeof s_rec);
    s_rec.magic = MAGIC;
    s_rec.version = VERSION;
    s_rec.sleep_s = SLEEP_DEFAULT_S;
    s_rec.awake_poll_s = 15;
}

void storage_begin(void)
{
    defaults();
    uint32_t best_seq = 0;
    size_t old_len = 0;
    for (int i = 0; i < (int)SLOTS; i++) {
        const record_t *r = slot_ptr(i);
        size_t ol = slot_valid(r) ? 0 : slot_old_len(r);
        if ((slot_valid(r) || ol) && r->seq >= best_seq) { best_seq = r->seq; s_slot = i; old_len = ol; }
    }
    if (s_slot >= 0 && !old_len) {
        memcpy(&s_rec, slot_ptr(s_slot), sizeof s_rec);
        LOGI("storage: loaded record seq %lu from slot %d", (unsigned long)s_rec.seq, s_slot);
        s_dirty = false;
    } else if (s_slot >= 0) {
        // The older fields carry over; the ones added since start empty.
        uint32_t was = slot_ptr(s_slot)->version;
        memcpy(&s_rec, slot_ptr(s_slot), old_len);
        s_rec.version = VERSION;
        LOGI("storage: migrated a version %lu record (seq %lu)", (unsigned long)was, (unsigned long)s_rec.seq);
        s_dirty = true;   // rewritten in the current layout on the next commit
    } else {
        LOGI("storage: no saved state (first boot)");
        s_dirty = false;
    }
}

static bool slot_erased(int i)
{
    const uint8_t *p = (const uint8_t *)slot_ptr(i);
    // Programming lands a page (256 B) at a time, so sample both pages.
    for (uint32_t k = 0; k < RECORD_SIZE; k += 64) if (p[k] != 0xFF) return false;
    return true;
}

void storage_commit(void)
{
    if (!s_dirty) return;

    int cur_sector = s_slot >= 0 ? s_slot / (int)SLOTS_PER_SECTOR : -1;
    // Prefer a free slot in the sector we are already using; otherwise move to
    // the other sector, which is then erased only AFTER the new record is safely
    // programmed.
    int slot = -1;
    if (cur_sector >= 0)
        for (int i = s_slot + 1; i < (cur_sector + 1) * (int)SLOTS_PER_SECTOR; i++)
            if (slot_erased(i)) { slot = i; break; }
    int erase_sector = -1;
    if (slot < 0) {
        int next_sector = cur_sector < 0 ? 0 : (cur_sector + 1) % (int)SECTOR_COUNT;
        int base = next_sector * (int)SLOTS_PER_SECTOR;
        for (int i = base; i < base + (int)SLOTS_PER_SECTOR; i++)
            if (slot_erased(i)) { slot = i; break; }
        if (slot < 0) {
            // The spare still holds old records: erase it first. The current
            // record lives in the other sector throughout, so a reset here is safe.
            uint32_t ints = save_and_disable_interrupts();
            flash_range_erase(SECTOR_OFFSET + (uint32_t)next_sector * FLASH_SECTOR_SIZE, FLASH_SECTOR_SIZE);
            restore_interrupts(ints);
            slot = base;
        }
        if (cur_sector >= 0) erase_sector = cur_sector;   // retired after the write
    }

    s_rec.seq++;
    s_rec.crc = crc32(&s_rec, offsetof(record_t, crc));
    static uint8_t page[RECORD_SIZE];
    memset(page, 0xFF, sizeof page);
    memcpy(page, &s_rec, sizeof s_rec);

    uint32_t ints = save_and_disable_interrupts();
    flash_range_program(SECTOR_OFFSET + (uint32_t)slot * RECORD_SIZE, page, RECORD_SIZE);
    restore_interrupts(ints);

    bool ok = slot_valid(slot_ptr(slot));
    LOGI("storage: %s seq %lu to slot %d", ok ? "saved" : "FAILED to save", (unsigned long)s_rec.seq, slot);
    if (!ok) return;
    s_slot = slot;
    s_dirty = false;
    // Now that the new record is verified, retire the old sector so the next
    // commit has somewhere erased to go.
    if (erase_sector >= 0) {
        uint32_t i2 = save_and_disable_interrupts();
        flash_range_erase(SECTOR_OFFSET + (uint32_t)erase_sector * FLASH_SECTOR_SIZE, FLASH_SECTOR_SIZE);
        restore_interrupts(i2);
    }
}

static void set_str(char *dst, size_t cap, const char *src)
{
    char tmp[256];
    snprintf(tmp, cap < sizeof tmp ? cap : sizeof tmp, "%s", src ? src : "");
    if (strcmp(dst, tmp) != 0) { snprintf(dst, cap, "%s", tmp); s_dirty = true; }
}

const char *storage_token(void) { return s_rec.token; }
void storage_set_token(const char *t) { set_str(s_rec.token, sizeof s_rec.token, t); }

const char *storage_device_id(void) { return s_rec.device_id[0] ? s_rec.device_id : DEVICE_ID; }
void storage_set_device_id(const char *id) { set_str(s_rec.device_id, sizeof s_rec.device_id, id); }

void storage_clear_pairing(void)
{
    storage_set_token("");
    storage_set_etag("");
}

const char *storage_etag(void) { return s_rec.etag; }
void storage_set_etag(const char *e) { set_str(s_rec.etag, sizeof s_rec.etag, e); }

uint32_t storage_next_event_id(void) { s_rec.event_id++; s_dirty = true; return s_rec.event_id; }

bool storage_fallback_kind(void) { return s_rec.fallback_kind; }
void storage_set_fallback_kind(bool v) { if (s_rec.fallback_kind != (uint8_t)v) { s_rec.fallback_kind = v; s_dirty = true; } }

int32_t storage_sleep_s(void) { return s_rec.sleep_s; }
void storage_set_sleep_s(int32_t s) { if (s_rec.sleep_s != s) { s_rec.sleep_s = s; s_dirty = true; } }
int32_t storage_button_wake_s(void) { return s_rec.button_wake_s; }
void storage_set_button_wake_s(int32_t s) { if (s_rec.button_wake_s != s) { s_rec.button_wake_s = s; s_dirty = true; } }
bool storage_always_on(void) { return s_rec.always_on; }
void storage_set_always_on(bool v) { if (s_rec.always_on != (uint8_t)v) { s_rec.always_on = v; s_dirty = true; } }
int32_t storage_awake_poll_s(void) { return s_rec.awake_poll_s; }
void storage_set_awake_poll_s(int32_t s) { if (s_rec.awake_poll_s != s) { s_rec.awake_poll_s = s; s_dirty = true; } }

void storage_erase(void)
{
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(SECTOR_OFFSET, SECTOR_COUNT * FLASH_SECTOR_SIZE);
    restore_interrupts(ints);
    s_slot = -1;
    defaults();
    s_dirty = false;
    LOGW("storage: erased");
}

void storage_get_ota(uint8_t *phase, uint8_t *reason, const char **target, const char **attempt, const char **detail)
{
    *phase = s_rec.ota_phase; *reason = s_rec.ota_reason;
    *target = s_rec.ota_target; *attempt = s_rec.ota_attempt; *detail = s_rec.ota_detail;
}

void storage_set_ota(uint8_t phase, uint8_t reason, const char *target, const char *attempt, const char *detail)
{
    if (s_rec.ota_phase != phase || s_rec.ota_reason != reason) {
        s_rec.ota_phase = phase; s_rec.ota_reason = reason; s_dirty = true;
    }
    set_str(s_rec.ota_target, sizeof s_rec.ota_target, target);
    set_str(s_rec.ota_attempt, sizeof s_rec.ota_attempt, attempt);
    set_str(s_rec.ota_detail, sizeof s_rec.ota_detail, detail);
}

void storage_get_diag(uint32_t *id, uint32_t *at, uint8_t *code)
{
    *id = s_rec.diag_id; *at = s_rec.diag_at; *code = s_rec.diag_code;
}

void storage_set_diag(uint32_t id, uint32_t at, uint8_t code)
{
    if (s_rec.diag_id != id || s_rec.diag_at != at || s_rec.diag_code != code) {
        s_rec.diag_id = id; s_rec.diag_at = at; s_rec.diag_code = code; s_dirty = true;
    }
}

void storage_get_ota_log_sent(uint8_t *phase, uint8_t *reason, uint32_t *attempt_hash, bool *ota_log_key_set)
{
    *phase = s_rec.ota_log_phase; *reason = s_rec.ota_log_reason;
    *attempt_hash = (uint32_t)s_rec.ota_log_attempt[0] | (uint32_t)s_rec.ota_log_attempt[1] << 8 |
                    (uint32_t)s_rec.ota_log_attempt[2] << 16;
    *ota_log_key_set = (s_rec.reserved & 2) != 0;
}

void storage_set_ota_log_sent(uint8_t phase, uint8_t reason, uint32_t attempt_hash)
{
    uint8_t a0 = (uint8_t)attempt_hash, a1 = (uint8_t)(attempt_hash >> 8), a2 = (uint8_t)(attempt_hash >> 16);
    if (s_rec.ota_log_phase != phase || s_rec.ota_log_reason != reason || !(s_rec.reserved & 2) ||
        s_rec.ota_log_attempt[0] != a0 || s_rec.ota_log_attempt[1] != a1 || s_rec.ota_log_attempt[2] != a2) {
        s_rec.ota_log_phase = phase; s_rec.ota_log_reason = reason;
        s_rec.ota_log_attempt[0] = a0; s_rec.ota_log_attempt[1] = a1; s_rec.ota_log_attempt[2] = a2;
        s_rec.reserved |= 2;
        s_dirty = true;
    }
}

bool storage_portal_offered(void) { return s_rec.reserved & 1; }
void storage_set_portal_offered(bool v)
{
    uint8_t r = v ? (s_rec.reserved | 1) : (s_rec.reserved & ~1);
    if (r != s_rec.reserved) { s_rec.reserved = r; s_dirty = true; }
}

uint8_t storage_backoff(void) { return s_rec.backoff; }
void storage_set_backoff(uint8_t s) { if (s_rec.backoff != s) { s_rec.backoff = s; s_dirty = true; } }
