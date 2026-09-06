// Build-time configuration for the Inky Frame 7.3" Tesserae client.
// Network credentials and the server URL live in secrets.h (git-ignored).
#pragma once

#include "secrets.h"
#include "version.h"

// ---- panel variant ----------------------------------------------------------
// The 7.3" Inky Frame shipped with two different panels that share every pin:
//   PANEL_ACEP     original 7-colour ACeP (AC073TC1A): black, white, green, blue,
//                  red, yellow, orange. Units sold before September 2025.
//   PANEL_SPECTRA  Spectra 6 (E673): black, white, yellow, red, blue, green.
// The server packs palette indices in the panel's own order for the declared
// gamut, so the choice decides the kind/gamut announced and the init sequence.
#define PANEL_ACEP     1
#define PANEL_SPECTRA  2
#ifndef PANEL_VARIANT
#define PANEL_VARIANT  PANEL_ACEP
#endif

// ---- identity ---------------------------------------------------------------
// device_id must match ^[a-z][a-z0-9_-]{1,31}$ . The server may hand back a
// different id on MAC match (a re-flashed board); the firmware adopts it.
#define DEVICE_ID            "inky_frame_73"
#define DEVICE_NAME          "Inky Frame 7.3"

#if PANEL_VARIANT == PANEL_ACEP
#define DEVICE_KIND          "pimoroni_inky_frame_73_acep"
#define DEVICE_GAMUT         "inky_7colour"
#define PAL_BLACK  0
#define PAL_WHITE  1
#define PAL_GREEN  2
#define PAL_BLUE   3
#define PAL_RED    4
#define PAL_YELLOW 5
#define PAL_ORANGE 6
#define PAL_COUNT  7
#else
#define DEVICE_KIND          "pimoroni_inky_frame_73"
#define DEVICE_GAMUT         "spectra_6"
#define PAL_BLACK  0
#define PAL_WHITE  1
#define PAL_YELLOW 2
#define PAL_RED    3
#define PAL_BLUE   5
#define PAL_GREEN  6
#define PAL_COUNT  7
#endif
// Stock kind used if the catalog entry above is not installed on the server.
// Its default panel is 1600x1200, so frames are refused until the admin sets
// the panel size; the firmware logs this loudly.
#define DEVICE_KIND_FALLBACK "pico_bin_client"

// ---- panel geometry ---------------------------------------------------------
// Landscape-native 800x480 buffer, 4 bpp, high nibble = left pixel.
// PANEL_ROTATION is the turn the server applies from this buffer to the
// dashboard (0/90/180/270). Verified on the bench with tools/make_test_frames.py:
// standing on its legs, the buffer origin is the physical top-left and the
// test triangle points left as generated, so no turn is needed. Read once at
// registration; afterwards change it from Settings -> Devices -> Rotation.
#define PANEL_W              800
#define PANEL_H              480
#define PANEL_ROTATION       0
#define FRAME_BYTES          (PANEL_W * PANEL_H / 2)   // 192000
#define ROW_BYTES            (PANEL_W / 2)             // 400

// ---- pins (Inky Frame, both revisions) --------------------------------------
#define PIN_HOLD_VSYS_EN     2
#define PIN_PSRAM_CS         3
#define PIN_I2C_SDA          4
#define PIN_I2C_SCL          5
#define PIN_LED_ACTIVITY     6
#define PIN_LED_CONNECTION   7
#define PIN_SR_CLOCK         8
#define PIN_SR_LATCH         9
#define PIN_SR_OUT           10
#define PIN_LED_A            11
#define PIN_LED_B            12
#define PIN_LED_C            13
#define PIN_LED_D            14
#define PIN_LED_E            15
#define PIN_SPI_MISO         16
#define PIN_EINK_CS          17
#define PIN_SPI_SCK          18
#define PIN_SPI_MOSI         19
#define PIN_EINK_RESET       27
#define PIN_EINK_DC          28
#define PIN_VSYS_ADC         29     // Pico W: VSYS/3, ADC channel 3

// ---- buttons ----------------------------------------------------------------
// Tesserae button names sent for each front button ("" = not mapped). The
// server's default button map knows left / right / refresh; other names need
// a custom map on the device card.
#define BUTTON_A_NAME        "left"
#define BUTTON_B_NAME        ""
#define BUTTON_C_NAME        "refresh"
#define BUTTON_D_NAME        ""
#define BUTTON_E_NAME        "right"

// ---- sleep and retry --------------------------------------------------------
// Bounds applied to server sleep values; the RTC alarm has no month field, so
// the longest single sleep is 27 days.
#define SLEEP_MIN_S          30
#define SLEEP_MAX_S          (27 * 86400)
#define SLEEP_DEFAULT_S      900
// Escalating backoff when Wi-Fi or the server is unreachable (seconds).
#define BACKOFF_STEPS_S      { 60, 120, 300, 900 }
// While waiting for the admin to click Register, stay awake this long polling
// /discover before falling back to deep-sleep retries.
#define PAIRING_AWAKE_BUDGET_S 300

// ---- network timeouts (ms) --------------------------------------------------
#define WIFI_CONNECT_TIMEOUT_MS   15000
#define WIFI_JOIN_ATTEMPTS        3
#define MDNS_QUERY_TIMEOUT_MS     1500
#define MDNS_QUERY_ATTEMPTS       3
#define HTTP_TIMEOUT_DISCOVER_MS  10000
#define HTTP_TIMEOUT_FRAME_MS     8000
#define HTTP_TIMEOUT_STATUS_MS    10000
#define HTTP_TIMEOUT_DOWNLOAD_MS  30000
#define DOWNLOAD_STALL_TIMEOUT_MS 10000
#define HTTP_BODY_MAX             (24 * 1024)   // JSON responses; larger bodies are truncated

// ---- misc -------------------------------------------------------------------
#define EPD_REFRESH_TIMEOUT_MS    50000
#define WIFI_HOSTNAME             "inky-frame-73"
