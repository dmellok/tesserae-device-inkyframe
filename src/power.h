// Battery sense and the two sleep regimes.
#pragma once

#include <stdbool.h>
#include <stdint.h>

// Sample VSYS (battery through the frame's diode, or the USB rail). Must run
// before the CYW43 is initialised: GP29 is shared with the wireless SPI.
uint32_t power_sample_vsys_mv(void);
uint32_t power_vsys_mv(void);
int32_t power_clamp_sleep_s(int32_t s);

// Call just before any reset. On battery a reset lets go of the VSYS hold pin
// and the board loses power before the next image can take it again, so the
// RTC's interrupt line (a wake source, latched in the RTC) is asserted to
// carry the board through. The next boot's pcf_init() releases it.
void power_hold_over_reboot(void);

// Arm the RTC and drop VSYS. On battery this never returns. On USB it returns
// once the RTC alarm fires or a button is pressed (index 0..4 in *button, else
// -1), after `seconds` at most.
void power_sleep(uint32_t seconds, bool on_usb, int *button);
