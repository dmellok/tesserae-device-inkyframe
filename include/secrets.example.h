// Copy this file to include/secrets.h (git-ignored) and fill in your values.
#pragma once

#define WIFI_SSID            "your-ssid"
#define WIFI_PASS            "your-password"

// Base URL of the Tesserae server, no trailing slash.
#define TESSERAE_SERVER_URL  "http://tesserae.local:8765"

// Optional: if the server URL uses a .local (mDNS) hostname, the firmware
// resolves it with an mDNS query on every wake. Set a fallback IP for when
// that fails. "" = none.
#define TESSERAE_FALLBACK_IP ""

// Optional: a 6-digit pairing code from Settings -> Devices -> Pair new device.
// Leave empty to use zero-touch discovery (click Register in the UI instead).
#define TESSERAE_PAIRING_CODE ""
