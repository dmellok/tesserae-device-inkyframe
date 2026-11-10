// Inky Frame board support: VSYS hold, wake-reason shift register, buttons,
// LEDs. The shift register is read MSB-first as in Pimoroni's C++ driver:
// bit 0..4 = buttons A..E, 5 = RTC alarm, 6 = external trigger, 7 = EINK busy
// (0 = busy).
#pragma once

#include <stdbool.h>
#include <stdint.h>

enum {
    SR_BUTTON_A = 1 << 0,
    SR_BUTTON_B = 1 << 1,
    SR_BUTTON_C = 1 << 2,
    SR_BUTTON_D = 1 << 3,
    SR_BUTTON_E = 1 << 4,
    SR_RTC_ALARM = 1 << 5,
    SR_EXT_TRIGGER = 1 << 6,
    SR_EINK_BUSY_N = 1 << 7,   // set = panel idle
};

typedef enum {
    WAKE_UNKNOWN = 0,   // power applied, reset button, USB plug
    WAKE_BUTTON_A, WAKE_BUTTON_B, WAKE_BUTTON_C, WAKE_BUTTON_D, WAKE_BUTTON_E,
    WAKE_RTC_ALARM,
    WAKE_EXT_TRIGGER,
} wake_event_t;

typedef enum {
    LED_ACTIVITY, LED_CONNECTION, LED_A, LED_B, LED_C, LED_D, LED_E, LED_COUNT
} led_t;

// Call first thing in main(): holds VSYS, sets up the shift register and LEDs,
// and captures the wake reason from the shift register.
void board_init(void);

wake_event_t board_wake_event(void);
// The shift register as board_init() first read it (buttons, RTC alarm, busy).
uint8_t board_boot_sr(void);
uint8_t board_read_sr(void);
// Index 0..4 = A..E, live state.
bool board_button_down(int index);
// First pressed button 0..4, or -1.
int board_any_button_down(void);
bool board_epd_busy(void);
void board_led(led_t led, uint8_t percent);
void board_led_button(int index, uint8_t percent);
// Drops VSYS hold. On battery this cuts power to the Pico; on USB nothing
// happens and the caller carries on.
void board_release_power(void);
const char *board_wake_name(wake_event_t e);
