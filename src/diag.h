// Device logs and failure reports (Tesserae's device log protocol v1, as the
// ESP32 firmware and the Pi client speak it).
//
// Every /status advertises {"logs": {"schema": 1, "ring_bytes": N}}. When the
// server answers {"logs": {"upload": true}} (the operator is collecting logs,
// or a beat carried a new failure) the RAM log ring is POSTed as text/plain to
// /api/v1/device/<id>/log, under a "# tesserae-log v1" header line.
//
// A failed paint is latched as {"diag": {"id", "paint_error", "at"}} and
// re-sent on every /status until one carrying it is accepted. It is kept in
// flash, because on battery the frame powers off between wakes.
//
// The ring holds this wake's log only: on battery each wake is a fresh boot.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "vendor/cJSON.h"

enum { DIAG_NONE = 0, DIAG_PAINT_TIMEOUT, DIAG_PAINT_INCOMPLETE };

// Call once at boot, with what woke the frame (for the log header).
void diag_begin(const char *wake_name);

// Add "logs" (and "diag", when a failure is latched) to a /status body.
// Returns the id of the diag it included, 0 if none.
uint32_t diag_add_status(cJSON *body);

// A /status carrying diag `sent_id` was accepted: clear it, unless a newer
// failure has replaced it since.
void diag_acknowledge(uint32_t sent_id);

void diag_paint_failed(uint8_t code);

// Upload the log ring (Wi-Fi up). Secrets are blanked first.
void diag_upload_log(const char *base, const char *device_id, const char *token);
