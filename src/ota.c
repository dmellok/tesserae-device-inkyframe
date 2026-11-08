#include "ota.h"

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"

#include "config.h"
#include "log.h"
#include "storage.h"

// Report vocabulary, in the contract's spelling. Stored as indices.
enum { PH_IDLE, PH_DOWNLOADING, PH_VALIDATING, PH_PENDING_CONFIRM, PH_CONFIRMED, PH_REJECTED, PH_FAILED, PH_ROLLED_BACK };
static const char *const PHASES[] = { "idle", "downloading", "validating", "pending_confirm",
                                      "confirmed", "rejected", "failed", "rolled_back" };
enum {
    R_OK, R_BAD_SIGNATURE, R_MALFORMED_DESCRIPTOR, R_MALFORMED_MANIFEST, R_SCHEMA_VERSION, R_KIND_MISMATCH,
    R_ALREADY_CURRENT, R_BATTERY_LOW, R_BATTERY_UNKNOWN, R_DOWNLOAD_ERROR, R_SIZE_MISMATCH, R_DIGEST_MISMATCH,
    R_IMAGE_INVALID, R_FLASH_ERROR, R_BOOT_FAILED,
};
static const char *const REASONS[] = {
    "ok", "bad_signature", "malformed_descriptor", "malformed_manifest", "schema_version", "kind_mismatch",
    "already_current", "battery_low", "battery_unknown", "download_error", "size_mismatch", "digest_mismatch",
    "image_invalid", "flash_error", "boot_failed",
};
#define N_PHASES  (sizeof PHASES / sizeof PHASES[0])
#define N_REASONS (sizeof REASONS / sizeof REASONS[0])

static void report(uint8_t phase, uint8_t reason, const char *target, const char *attempt, const char *detail)
{
    LOGI("ota: %s (%s) target %s%s%s", PHASES[phase], REASONS[reason], target && target[0] ? target : "-",
         detail && detail[0] ? ": " : "", detail ? detail : "");
    storage_set_ota(phase, reason, target, attempt, detail);
}

void ota_add_capability(cJSON *body)
{
    if (!ota_supported()) return;
    cJSON *o = cJSON_AddObjectToObject(body, "ota");
    cJSON_AddNumberToObject(o, "schema", 1);
    uint8_t phase, reason; const char *target, *attempt, *detail;
    storage_get_ota(&phase, &reason, &target, &attempt, &detail);
    if (phase == PH_IDLE || phase >= N_PHASES) return;
    cJSON_AddStringToObject(o, "phase", PHASES[phase]);
    if (reason < N_REASONS) cJSON_AddStringToObject(o, "reason", REASONS[reason]);
    if (target[0]) cJSON_AddStringToObject(o, "target_fw", target);
    if (attempt[0]) cJSON_AddStringToObject(o, "attempt_id", attempt);
    if (detail[0]) cJSON_AddStringToObject(o, "detail", detail);
}

#if !PICO_RP2350

// The RP2040 boot ROM has no A/B partitions: no OTA, nothing advertised.
void ota_boot(void) {}
bool ota_supported(void) { return false; }
bool ota_on_trial(void) { return false; }
bool ota_confirm(void) { return true; }
void ota_abandon_trial(const char *detail) { (void)detail; }
void ota_offer(const char *payload, const char *signature, const char *kind, bool on_usb, uint32_t vsys_mv)
{
    (void)payload; (void)signature; (void)kind; (void)on_usb; (void)vsys_mv;
}

#else

#include "boot/picobin.h"
#include "boot/picoboot_constants.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "hardware/watchdog.h"
#include "pico/bootrom.h"
#include "pico/time.h"

#include "http.h"
#include "ota_verify.h"
#include "psram.h"
#include "sha256.h"
#include "wifi.h"

static bool s_partitioned;   // booted from a partition table slot
static int s_partition = -1; // the slot we are running from
static bool s_trial;         // an unconfirmed (TBYB) update is running

// The watchdog's longest timeout is about 16.7 s, too short to join Wi-Fi, so
// a timer feeds it until the trial deadline and then stops: a hang, or a trial
// that never confirms, still resets back to the previous image.
static repeating_timer_t s_feed_timer;
static absolute_time_t s_trial_deadline;

static bool feed_watchdog(repeating_timer_t *t)
{
    (void)t;
    if (!time_reached(s_trial_deadline)) watchdog_update();
    return true;
}

bool ota_supported(void) { return s_partitioned; }
bool ota_on_trial(void) { return s_trial; }

void ota_boot(void)
{
    boot_info_t bi;
    if (!rom_get_boot_info(&bi)) { LOGW("ota: no boot info"); return; }
    s_partition = bi.partition;
    s_partitioned = bi.partition >= 0;
    s_trial = (bi.tbyb_and_update_info & BOOT_TBYB_AND_UPDATE_FLAG_BUY_PENDING) != 0;
    LOGI("ota: running from %s%s, boot type %u", s_partitioned ? (s_partition == 0 ? "partition A" : "partition B") : "unpartitioned flash",
         s_trial ? " (update on trial)" : "", bi.boot_type);

    uint8_t phase, reason; const char *target, *attempt, *detail;
    storage_get_ota(&phase, &reason, &target, &attempt, &detail);
    if (s_trial) {
        // A hang before the buy must still revert: the boot ROM's own
        // watchdog is replaced by one long enough to join Wi-Fi.
        s_trial_deadline = make_timeout_time_ms(OTA_TRIAL_DEADLINE_MS);
        watchdog_enable(OTA_WATCHDOG_MS, true);
        add_repeating_timer_ms(1000, feed_watchdog, NULL, &s_feed_timer);
        if (phase != PH_PENDING_CONFIRM) report(PH_PENDING_CONFIRM, R_OK, FW_VERSION, attempt, "");
    } else if (phase == PH_PENDING_CONFIRM || phase == PH_DOWNLOADING || phase == PH_VALIDATING) {
        if (strcmp(target, FW_VERSION) == 0) {
            report(PH_CONFIRMED, R_OK, target, attempt, "");
        } else {
            // We wrote and booted the update, and are back on the old image.
            report(PH_ROLLED_BACK, R_BOOT_FAILED, target, attempt, detail[0] ? detail : "the update did not confirm");
        }
        storage_commit();
    }
}

bool ota_confirm(void)
{
    if (!s_trial) return true;
#ifdef OTA_BENCH_NEVER_CONFIRM
    // Bench rollback test: behave like an update that never proves itself.
    LOGW("ota: bench build never confirms; waiting for the trial watchdog");
    while (true) sleep_ms(1000);
#endif
    // explicit_buy rewrites the image's first sector: no flash access may run
    // meanwhile, so the radio (whose IRQ path runs from flash) goes down first.
    wifi_off();
    static uint8_t workarea[4096];
    uint32_t ints = save_and_disable_interrupts();
    int rc = rom_explicit_buy(workarea, sizeof workarea);
    restore_interrupts(ints);
    if (rc != 0) {
        LOGE("ota: explicit_buy failed (%d)", rc);
        ota_abandon_trial("explicit_buy failed");
    }
    s_trial = false;
    cancel_repeating_timer(&s_feed_timer);
    watchdog_disable();
    uint8_t phase, reason; const char *target, *attempt, *detail;
    storage_get_ota(&phase, &reason, &target, &attempt, &detail);
    report(PH_CONFIRMED, R_OK, FW_VERSION, attempt, "");
    storage_commit();
    return true;
}

void ota_abandon_trial(const char *why)
{
    uint8_t phase, reason; const char *target, *attempt, *detail;
    storage_get_ota(&phase, &reason, &target, &attempt, &detail);
    report(PH_ROLLED_BACK, R_BOOT_FAILED, FW_VERSION, attempt, why);
    wifi_off();
    storage_commit();
    LOGW("ota: abandoning the update (%s); rebooting to the previous image", why);
    stdio_flush();
    watchdog_reboot(0, 0, 0);
    while (true) tight_loop_contents();
}

// ---- the inactive partition ----

typedef struct { uint32_t start, size; int index; } slot_t;

static bool target_slot(slot_t *out)
{
    static uint8_t workarea[4096];
    if (rom_load_partition_table(workarea, sizeof workarea, false) != 0) { LOGE("ota: no partition table"); return false; }
    int other = s_partition == 0 ? rom_get_b_partition(0) : 0;
    if (other < 0) { LOGE("ota: partition %d has no A/B partner (%d)", s_partition, other); return false; }
    uint32_t info[3];
    int n = rom_get_partition_table_info(info, 3, PT_INFO_PARTITION_LOCATION_AND_FLAGS | PT_INFO_SINGLE_PARTITION | ((uint32_t)other << 24));
    if (n != 3) { LOGE("ota: partition info failed (%d)", n); return false; }
    uint32_t loc = info[1];
    uint32_t first = (loc & PICOBIN_PARTITION_LOCATION_FIRST_SECTOR_BITS) >> PICOBIN_PARTITION_LOCATION_FIRST_SECTOR_LSB;
    uint32_t last = (loc & PICOBIN_PARTITION_LOCATION_LAST_SECTOR_BITS) >> PICOBIN_PARTITION_LOCATION_LAST_SECTOR_LSB;
    out->start = first * FLASH_SECTOR_SIZE;
    out->size = (last - first + 1) * FLASH_SECTOR_SIZE;
    out->index = other;
    return true;
}

// ---- download into PSRAM, hashing as it streams ----

typedef struct {
    uint32_t want, got;
    bool length_ok;
    sha256_ctx_t sha;
} dl_t;

static bool on_length(size_t len, void *arg)
{
    dl_t *d = arg;
    d->length_ok = len == d->want;
    if (!d->length_ok) LOGE("ota: image is %u bytes, the manifest says %lu", (unsigned)len, (unsigned long)d->want);
    return d->length_ok;
}

static void sink(const uint8_t *data, size_t len, void *arg)
{
    dl_t *d = arg;
    if (d->got + len > d->want) len = d->want - d->got;
    psram_write(OTA_PSRAM_OFFSET + d->got, data, len);
    sha256_update(&d->sha, data, len);
    d->got += (uint32_t)len;
}

// The image must start with a valid picobin block (the boot ROM checks it
// again, but a bad image is better refused before the partition is erased).
static bool image_looks_valid(uint32_t size)
{
    static uint8_t head[4096];
    uint32_t n = size < sizeof head ? size : sizeof head;
    psram_read(OTA_PSRAM_OFFSET, head, n);
    for (uint32_t i = 0; i + 4 <= n; i += 4) {
        uint32_t w = (uint32_t)head[i] | (uint32_t)head[i + 1] << 8 | (uint32_t)head[i + 2] << 16 | (uint32_t)head[i + 3] << 24;
        if (w == PICOBIN_BLOCK_MARKER_START) return true;
    }
    return false;
}

static bool write_partition(const slot_t *slot, uint32_t size)
{
    static uint8_t buf[FLASH_SECTOR_SIZE];
    for (uint32_t off = 0; off < size; off += FLASH_SECTOR_SIZE) {
        uint32_t n = size - off < FLASH_SECTOR_SIZE ? size - off : FLASH_SECTOR_SIZE;
        memset(buf, 0xFF, sizeof buf);
        psram_read(OTA_PSRAM_OFFSET + off, buf, n);
        uint32_t ints = save_and_disable_interrupts();
        flash_range_erase(slot->start + off, FLASH_SECTOR_SIZE);
        flash_range_program(slot->start + off, buf, FLASH_SECTOR_SIZE);
        restore_interrupts(ints);
        if (memcmp((const void *)(XIP_NOCACHE_NOALLOC_NOTRANSLATE_BASE + slot->start + off), buf, FLASH_SECTOR_SIZE) != 0) {
            LOGE("ota: flash verify failed at +0x%lx", (unsigned long)off);
            return false;
        }
        if ((off / FLASH_SECTOR_SIZE) % 32 == 0) LOGI("ota: wrote %lu / %lu", (unsigned long)off, (unsigned long)size);
    }
    return true;
}

void ota_offer(const char *payload, const char *signature, const char *kind, bool on_usb, uint32_t vsys_mv)
{
    if (!s_partitioned || s_trial) return;
    ota_manifest_t m;
    ota_verify_result_t v = ota_verify_descriptor(payload, signature, kind, FW_VERSION, &m);
    char attempt[12] = "";
    if (m.sha256[0]) snprintf(attempt, sizeof attempt, "%.8s", m.sha256);

    uint8_t phase, reason; const char *target, *prev_attempt, *detail;
    storage_get_ota(&phase, &reason, &target, &prev_attempt, &detail);
    if (v != OTA_VERIFY_OK) {
        // Do not overwrite the "confirmed" of the update now running.
        if (!(v == OTA_VERIFY_ALREADY_CURRENT && phase == PH_CONFIRMED))
            report(PH_REJECTED, (uint8_t)v, m.fw_version, attempt, "");   // verify codes share the reason order
        return;
    }
    // An image that already failed to boot is not retried; a new release is.
    if (phase == PH_ROLLED_BACK && strcmp(prev_attempt, attempt) == 0) {
        LOGI("ota: %s already rolled back; not retrying", m.fw_version);
        return;
    }
    if (!on_usb) {
        if (vsys_mv == 0) { report(PH_REJECTED, R_BATTERY_UNKNOWN, m.fw_version, attempt, ""); return; }
        if (vsys_mv < OTA_MIN_VSYS_MV) {
            char d[40]; snprintf(d, sizeof d, "%lu mV", (unsigned long)vsys_mv);
            report(PH_REJECTED, R_BATTERY_LOW, m.fw_version, attempt, d);
            return;
        }
    }
    slot_t slot;
    if (!target_slot(&slot)) { report(PH_FAILED, R_FLASH_ERROR, m.fw_version, attempt, "no partition to install into"); return; }
    if (m.size_bytes > slot.size) {
        char d[40]; snprintf(d, sizeof d, "%lu bytes > slot %lu", (unsigned long)m.size_bytes, (unsigned long)slot.size);
        report(PH_FAILED, R_IMAGE_INVALID, m.fw_version, attempt, d);
        return;
    }

    LOGI("ota: installing %s (%lu bytes) into partition %c", m.fw_version, (unsigned long)m.size_bytes, slot.index == 0 ? 'A' : 'B');
    report(PH_DOWNLOADING, R_OK, m.fw_version, attempt, "");
    // This firmware has no TLS, so an https:// image is fetched over plain
    // HTTP (the signer's host serves both). That costs no integrity: the image
    // must match the signed size and SHA-256 before a byte reaches flash.
    static char url[340], plain[340];
    if (strncmp(m.image_url, "https://", 8) == 0) snprintf(plain, sizeof plain, "http://%s", m.image_url + 8);
    else snprintf(plain, sizeof plain, "%s", m.image_url);
    wifi_rewrite_host(plain, url, sizeof url);
    static dl_t dl;
    memset(&dl, 0, sizeof dl);
    dl.want = m.size_bytes;
    sha256_init(&dl.sha);
    http_resp_t r;
    int code = http_request("GET", url, NULL, 0, NULL, &r, on_length, sink, &dl, OTA_DOWNLOAD_TIMEOUT_MS, DOWNLOAD_STALL_TIMEOUT_MS);
    http_resp_free(&r);
    if (code != 200 || dl.got != dl.want) {
        char d[48]; snprintf(d, sizeof d, "http %d, %lu/%lu bytes", code, (unsigned long)dl.got, (unsigned long)dl.want);
        report(PH_FAILED, (!dl.length_ok && code == HTTP_ERR_ABORTED) ? R_SIZE_MISMATCH : R_DOWNLOAD_ERROR, m.fw_version, attempt, d);
        return;
    }

    report(PH_VALIDATING, R_OK, m.fw_version, attempt, "");
    uint8_t digest[32]; char hex[65];
    sha256_final(&dl.sha, digest);
    sha256_hex(digest, hex);
    if (strcmp(hex, m.sha256) != 0) { report(PH_FAILED, R_DIGEST_MISMATCH, m.fw_version, attempt, ""); return; }
    if (!image_looks_valid(m.size_bytes)) { report(PH_FAILED, R_IMAGE_INVALID, m.fw_version, attempt, "no picobin block"); return; }

    // Flash with the radio down; the frame reboots straight after.
    wifi_off();
    if (!write_partition(&slot, m.size_bytes)) {
        report(PH_FAILED, R_FLASH_ERROR, m.fw_version, attempt, "");
        storage_commit();
        return;
    }
    report(PH_PENDING_CONFIRM, R_OK, m.fw_version, attempt, "");
    storage_commit();
    LOGI("ota: rebooting into %s for its trial boot", m.fw_version);
    stdio_flush();
    rom_reboot(REBOOT2_FLAG_REBOOT_TYPE_FLASH_UPDATE | REBOOT2_FLAG_NO_RETURN_ON_SUCCESS, 100, XIP_BASE + slot.start, 0);
    while (true) tight_loop_contents();
}

#endif
