// Signed over-the-air updates (Tesserae docs/ota/contract.md) on the RP2350's
// A/B partitions.
//
// The image is installed into the partition the frame is not running from and
// booted with a flash-update reboot. It is built "try before you buy" (TBYB):
// the boot ROM runs it once, and unless it calls explicit_buy the next reset
// goes back to the previous image. The firmware buys only after Wi-Fi joins,
// so an update that cannot reach the network reverts by itself.
//
// Only an RP2350 running from a partition table advertises OTA; an RP2040
// build, or a frame flashed without the table, reports nothing and is never
// offered an update.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "vendor/cJSON.h"

// Read the boot info and the saved report: notices a trial boot (and arms a
// watchdog so a hang reverts) or a rollback. Call after storage_begin().
void ota_boot(void);

// The frame can take an update (RP2350, booted from a partition).
bool ota_supported(void);

// This boot is an unconfirmed update.
bool ota_on_trial(void);

// Wi-Fi joined on a trial boot: buy the image (with the radio briefly down,
// as explicit_buy writes flash) and record "confirmed". Returns false if the
// buy failed, in which case the frame reboots back to the previous image.
bool ota_confirm(void);

// A trial boot that could not prove itself (no Wi-Fi): record the rollback and
// reset, so the boot ROM returns to the previous image. Does not return.
void ota_abandon_trial(const char *detail);

// Add the "ota" capability and report to a /register or /status body.
void ota_add_capability(cJSON *body);

// OTA logs (contract "OTA logs"): send this boot's log, tagged as an OTA
// log, if the saved report is one that goes with one (failed, confirmed,
// rolled_back, or rejected by the battery gate) and none has gone out for its
// (attempt_id, phase, reason) yet. Best-effort and bounded; a failure leaves it
// due for a later wake. Call with Wi-Fi up, after the /status that carried the
// report. The install also sends one itself just before its reboot.
void ota_log_flush(const char *base);

// A descriptor arrived on /status (Wi-Fi up). Verifies, checks the battery,
// downloads, installs and reboots into the new image (never returns then);
// otherwise records why not and returns.
void ota_offer(const char *payload, const char *signature, const char *kind, bool on_usb, uint32_t vsys_mv);
