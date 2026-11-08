// User settings entered in the setup portal: Wi-Fi, the Tesserae server and an
// optional pairing code. Kept in their own two flash sectors, apart from the
// device state record (storage.c), because they change rarely and must survive
// everything except a factory reset.
//
// A build with include/secrets.h seeds them on first boot, so a developer can
// skip the portal; a build without it starts in the portal.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define SETTINGS_SSID_MAX     33
#define SETTINGS_PASS_MAX     65
#define SETTINGS_URL_MAX      160
#define SETTINGS_IP_MAX       16
#define SETTINGS_CODE_MAX     16

void settings_begin(void);

// Wi-Fi SSID and server URL are both set: the device can try to onboard.
bool settings_configured(void);

const char *settings_ssid(void);
const char *settings_pass(void);
const char *settings_server_url(void);
const char *settings_fallback_ip(void);
const char *settings_pairing_code(void);

// True once these settings have joined Wi-Fi and reached the server. Until
// then a failure reopens the setup portal; after it, an outage only retries.
bool settings_verified(void);

// NULL leaves a field unchanged. Saving new Wi-Fi or server values clears the
// verified flag. Writes flash immediately (call with the radio down).
bool settings_save(const char *ssid, const char *pass, const char *server_url, const char *pairing_code);
void settings_set_verified(void);
void settings_clear_pairing_code(void);

// Factory reset: erase both sectors. The next boot starts in the portal (or
// from secrets.h on a development build).
void settings_erase(void);

// ---- cloud relay (relay.c) ----
// Relay mode: the frame talks only to a relay mailbox, never to the server.
bool settings_relay_mode(void);
bool settings_relay_paired(void);
const char *settings_relay_url(void);
const char *settings_relay_code(void);
const char *settings_relay_install(void);
const char *settings_relay_device(void);
const char *settings_relay_token(void);
const char *settings_relay_config_etag(void);
const uint8_t *settings_relay_key(void);    // NULL until paired
const uint8_t *settings_relay_priv(void);   // NULL unless a pairing is in flight

// From the portal: relay URL ("" = the hosted relay) and a pairing code.
// Clears any previous relay pairing. Writes flash.
bool settings_save_relay(const char *url, const char *code);
// These change RAM only; settings_commit() writes them (with the radio down).
void settings_relay_set_priv(const uint8_t priv[32]);
void settings_relay_set_paired(const char *install, const char *device, const char *token, const uint8_t key[32]);
void settings_relay_set_config_etag(const char *etag);
// A revoked pairing (401) or an expired code: forget the pairing.
void settings_relay_drop(bool keep_code);
void settings_commit(void);
