// Tesserae v1 REST device API: discover / register / frame / status / log.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "http.h"

typedef struct {
    const char *device_id;
    const char *kind;
    const char *gamut;
    const char *mac;
} tess_identity_t;

typedef struct {
    int code;                 // HTTP status or negative transport error
    bool registered;
    char token[256];
    char device_id[40];       // server-assigned id (MAC match); may differ
    int retry_after_s;
    int32_t sleep_interval_s; // -1 if absent
    uint32_t server_time;     // 0 if absent
    char error[96];
} tess_pair_t;

typedef enum { TESS_KIND_UNKNOWN, TESS_KIND_EXISTS, TESS_KIND_MISSING } tess_kind_probe_t;

typedef struct {
    int code;
    char url[256];
    char format[16];
    char etag[80];
    int panel_w, panel_h;
    int32_t button_wake_s;    // -1 if absent
} tess_frame_t;

typedef struct {
    uint32_t battery_mv;      // 0 = omit
    int rssi;
    const char *ip;
    const char *mac;
    const char *button;       // NULL = none pending
    const char *button_key;   // physical button "a".."e", or NULL
    uint32_t button_event_id;
    bool can_stay_awake;
} tess_telemetry_t;

typedef struct {
    int code;
    int32_t next_poll_s;      // -1 if absent
    uint32_t wake_at;
    uint32_t server_time;
    int32_t sleep_interval_s; // -1 if absent
    int32_t button_wake_s;    // -1 if absent
    int always_on;            // -1 absent, 0/1
    // A signed OTA descriptor, when the server offered one (else NULL). Points
    // into storage owned by tesserae.c, valid until the next status call.
    const char *ota_payload;
    const char *ota_signature;
    uint32_t diag_sent;       // the diag id this beat carried, 0 if none
    bool logs_upload;         // the server asked for the log ring
    // Deck / album version announcements (cache.c); has_* false = unbound.
    bool has_deck, has_album;
    char deck_version[32], album_version[32];
    int32_t awake_poll_s;     // -1 if absent
} tess_status_t;

// Register with a dummy pairing code: a 400 "unknown device kind" means the
// kind is not installed; a 403 means it exists.
tess_kind_probe_t tess_probe_kind(const char *base, const tess_identity_t *id);
void tess_discover(const char *base, const tess_identity_t *id, tess_pair_t *out);
void tess_register(const char *base, const tess_identity_t *id, const char *code, tess_pair_t *out);
void tess_get_frame(const char *base, const char *device_id, const char *token, const char *etag,
                    const char *button, const char *button_key, uint32_t event_id, tess_frame_t *out);
// The /status JSON body for this telemetry (malloc'd; free it). The relay
// posts the same body. *diag_sent gets the diag id it carried, 0 if none.
char *tess_status_body(const tess_telemetry_t *t, uint32_t *diag_sent);

void tess_post_status(const char *base, const char *device_id, const char *token,
                      const tess_telemetry_t *t, tess_status_t *out);
void tess_post_log(const char *base, const char *device_id, const char *token, const char *level, const char *msg);
