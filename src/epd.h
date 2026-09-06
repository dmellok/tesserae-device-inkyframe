// Inky Frame 7.3" panel driver (AC073TC1A 7-colour, or E673 Spectra 6 with
// PANEL_VARIANT=PANEL_SPECTRA). Command sequences follow Pimoroni's inky73
// (pimoroni-pico, MIT) and inky_e673.py (pimoroni/inky, MIT) drivers.
//
// The panel takes 4 bpp palette indices, row-major from the top-left of the
// landscape buffer, high nibble = left pixel: exactly the Tesserae frame bytes.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Supplies row `y` (ROW_BYTES bytes) into `out`.
typedef void (*epd_row_fn)(int y, uint8_t *out, void *arg);

void epd_init(void);
// Full sequence: init, stream all rows, refresh, power off. Blocks ~30 s.
// Returns false if the panel never reported ready.
bool epd_paint(epd_row_fn row, void *arg);
// Eight vertical bars of palette index 0..7 (panel-revision detector).
bool epd_paint_test_pattern(void);
