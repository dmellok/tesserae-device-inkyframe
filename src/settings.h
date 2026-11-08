// User settings entered in the setup portal: Wi-Fi, the Tesserae server and an
// optional pairing code. Kept in their own two flash sectors, apart from the
// device state record (storage.c), because they change rarely and must survive
// everything except a factory reset.
//
// A build with include/secrets.h seeds them on first boot, so a developer can
// skip the portal; a build without it starts in the portal.
#pragma once

#include <stdbool.h>

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
