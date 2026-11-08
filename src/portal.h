// Setup portal: an access point plus a captive web form for Wi-Fi, the
// Tesserae server and an optional pairing code. Paints the setup page (with a
// QR code that joins the network) before the radio comes up.
#pragma once

#include <stdbool.h>
#include <stddef.h>

// "Tesserae-Setup-XXXX" for this board.
void portal_ap_name(char *out, size_t cap);

// Run the portal. Saving the form writes the settings and reboots, so that
// path never returns. Returns false when the portal closed unused (on battery,
// after SETUP_IDLE_TIMEOUT_MS) or could not start; on USB it waits for ever.
// `reason` is shown on the page and the form ("" for a fresh frame).
bool portal_run(const char *reason);
