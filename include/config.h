// Build-time configuration for the Inky Frame 7.3" Tesserae client.
//
// Wi-Fi and the server are entered in the setup portal and kept in flash
// (settings.c). include/secrets.h (git-ignored, optional) seeds them on first
// boot so a development build can skip the portal.
#pragma once

#if __has_include("secrets.h")
#include "secrets.h"
#endif
#include "version.h"

#ifndef WIFI_SSID
#define WIFI_SSID            ""
#endif
#ifndef WIFI_PASS
#define WIFI_PASS            ""
#endif
#ifndef TESSERAE_SERVER_URL
#define TESSERAE_SERVER_URL  ""
#endif
#ifndef TESSERAE_FALLBACK_IP
#define TESSERAE_FALLBACK_IP ""
#endif
#ifndef TESSERAE_PAIRING_CODE
#define TESSERAE_PAIRING_CODE ""
#endif

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
// Tesserae button action for each front button. Every press also carries its
// letter as button_key ("a".."e"), which the server's button map checks first,
// so each button can be bound on its own; a button with no action here ("")
// goes by its letter alone.
#define BUTTON_A_NAME        "left"
#define BUTTON_B_NAME        ""
#define BUTTON_C_NAME        "refresh"
#define BUTTON_D_NAME        ""
#define BUTTON_E_NAME        "right"
// Hold Refresh (button C) as the frame wakes: this long opens the setup
// portal, and this long wipes Wi-Fi, server and pairing (factory reset).
#define BUTTON_SETUP_INDEX   2
#define HOLD_SETUP_MS        5000
#define HOLD_RESET_MS        20000

// ---- setup portal -----------------------------------------------------------
// The access point a fresh (or reset) frame opens: "<prefix>-XXXX", WPA2 with
// this password, matching the ESP32 firmware's setup network.
#define SETUP_AP_PREFIX      "Tesserae-Setup"
#define SETUP_AP_PASSWORD    "tesserae"
#define SETUP_PORTAL_URL     "http://192.168.4.1/"
// On battery, close an unused portal and power off after this long.
#define SETUP_IDLE_TIMEOUT_MS (15u * 60u * 1000u)

// ---- flash layout -----------------------------------------------------------
// The last four 4 KB sectors: two for the device state record (storage.c),
// two below them for the portal settings (settings.c).
#define FLASH_STATE_OFFSET    (PICO_FLASH_SIZE_BYTES - 2u * 4096u)
#define FLASH_SETTINGS_OFFSET (PICO_FLASH_SIZE_BYTES - 4u * 4096u)
// Where the firmware reads those sectors. On the RP2350 the image runs from an
// A/B partition through address translation, so XIP_BASE is a window onto the
// partition, not the whole flash; the untranslated alias reaches the sectors
// wherever the image lives. (The SDK's flash erase/program take physical
// offsets either way.)
#if PICO_RP2350
#define FLASH_READ_BASE       XIP_NOCACHE_NOALLOC_NOTRANSLATE_BASE
#else
#define FLASH_READ_BASE       XIP_BASE
#endif

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

// ---- over-the-air updates (RP2350 only; see src/ota.h) ----------------------
// Downloaded images are staged in PSRAM above the frame store.
#define OTA_PSRAM_OFFSET          (1024u * 1024u)
#define OTA_DOWNLOAD_TIMEOUT_MS   120000
// A trial boot must join Wi-Fi and confirm within this, or it reverts.
#define OTA_TRIAL_DEADLINE_MS     90000
#define OTA_WATCHDOG_MS           8000
// On battery, refuse to install below this VSYS. The Frame reads the battery
// through a diode, so this is about the ESP32 firmware's 3.6 V at the cell.
#define OTA_MIN_VSYS_MV           3300

// ---- cloud relay (src/relay.c) ----------------------------------------------
#define RELAY_DEFAULT_URL         "https://relay.tesserae.ink"
// Sealed frames are staged in PSRAM here before they are opened into the
// frame store at 0.
#define RELAY_PSRAM_OFFSET        (4u * 1024u * 1024u)
