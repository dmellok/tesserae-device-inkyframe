// PCF85063A real-time clock on I2C0. Keeps wall-clock time through power-off
// and wakes the board with its alarm or countdown timer. Register layout from
// the NXP datasheet; sequences follow Pimoroni's pcf85063a driver.
#pragma once

#include <stdbool.h>
#include <stdint.h>

void pcf_init(void);
// True if the oscillator has run continuously since the time was last set.
bool pcf_time_valid(void);
uint32_t pcf_get_epoch(void);
void pcf_set_epoch(uint32_t epoch);
// Wake at an absolute time (UTC epoch). The alarm has no month field, so the
// target must be less than ~27 days away. Clears any pending flags first.
void pcf_arm_alarm_epoch(uint32_t epoch);
// Wake in N minutes using the countdown timer (1..255) when the clock is unset.
void pcf_arm_timer_minutes(uint8_t minutes);
// Pull the INT line low now and leave it low (a countdown-timer flag raised
// within ~50 ms), so the board stays powered with no help from the MCU.
// pcf_init() of the next boot releases it. False if the flag never came up.
bool pcf_assert_int(void);
// Disable and clear alarm + timer (releases the INT line at boot).
void pcf_disarm(void);
bool pcf_alarm_fired(void);
// One byte of battery-backed RAM.
uint8_t pcf_ram_get(void);
void pcf_ram_set(uint8_t v);
