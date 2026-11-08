// Cloud relay transport (Tesserae docs/relay/contract.md): for a frame that
// cannot reach the Tesserae server. Both ends talk outbound to a relay
// mailbox over HTTPS; frames and config arrive sealed with a key derived on
// each side (X25519 + HKDF) that never travels, so the relay sees ciphertext.
//
// Pairing: the panel POSTs its public key with a pairing code from Tesserae's
// Settings > Cloud relay, then polls until home completes the exchange. The
// POST happens once, latched on the stored private key (only that route
// enforces the code's expiry).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tesserae.h"

typedef enum {
    RELAY_PAIRED,       // the frame key exists
    RELAY_WAITING,      // submitted; home has not completed it yet
    RELAY_CODE_DEAD,    // unknown or expired code: a new one is needed
    RELAY_REJECTED,     // 403: the relay refused it
    RELAY_UNREACHABLE,
} relay_pair_result_t;

// Advance pairing (Wi-Fi up). Polls for up to `budget_ms` while waiting.
relay_pair_result_t relay_pair(uint32_t budget_ms);

// GET the frame mailbox with If-None-Match. On 200 the sealed frame is
// downloaded, authenticated and opened into the PSRAM frame store
// (`*opened`), with its ETag in etag_out. Returns the HTTP status; 401 means
// the pairing was revoked.
int relay_fetch_frame(const char *etag, bool *opened, char *etag_out, size_t etag_cap);

typedef struct {
    int code;                 // HTTP status of the status POST
    int32_t sleep_interval_s; // -1 when no config was applied
    int32_t button_wake_s;
    int32_t awake_poll_s;
    int always_on;            // -1 absent
    uint32_t diag_sent;
} relay_status_t;

// POST the status body (same JSON as a REST /status) and, when the relay's
// config_etag moved, pull and open the config mailbox.
void relay_post_status(const tess_telemetry_t *t, relay_status_t *out);
