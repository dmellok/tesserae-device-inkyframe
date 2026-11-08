// Full-screen text pages for states with no dashboard to show. Rendered into
// the PSRAM frame store with the public-domain font8x8, then painted.
#pragma once

#include <stdbool.h>

// "Register me": shown while waiting for the admin to click Register.
bool screen_pairing(const char *device_id, const char *mac, const char *server_url, const char *kind, const char *note);
// Generic message page: a title plus up to three detail lines ("" to skip).
bool screen_message(const char *title, const char *line1, const char *line2, const char *line3);
// Setup portal instructions plus a QR code that joins the setup network.
// `reason` says why setup is showing ("" for a fresh frame).
bool screen_setup(const char *ap_ssid, const char *ap_pass, const char *portal_url, const char *reason);
