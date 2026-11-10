#include "board.h"

#include <math.h>
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/pwm.h"

#include "config.h"

static wake_event_t s_wake = WAKE_UNKNOWN;
static uint8_t s_boot_sr;
static const uint8_t LED_PINS[LED_COUNT] = {
    PIN_LED_ACTIVITY, PIN_LED_CONNECTION, PIN_LED_A, PIN_LED_B, PIN_LED_C, PIN_LED_D, PIN_LED_E
};

static void gpio_out(uint pin, bool value)
{
    gpio_init(pin);
    gpio_set_dir(pin, GPIO_OUT);
    gpio_put(pin, value);
}

static void led_pwm_init(uint pin)
{
    pwm_config cfg = pwm_get_default_config();
    uint slice = pwm_gpio_to_slice_num(pin);
    pwm_set_wrap(slice, 65535);
    pwm_init(slice, &cfg, true);
    gpio_set_function(pin, GPIO_FUNC_PWM);
    pwm_set_gpio_level(pin, 0);
}

uint8_t board_read_sr(void)
{
    gpio_put(PIN_SR_LATCH, 0); sleep_us(1);
    gpio_put(PIN_SR_LATCH, 1); sleep_us(1);
    uint8_t result = 0;
    for (int i = 0; i < 8; i++) {
        result <<= 1;
        result |= gpio_get(PIN_SR_OUT) ? 1 : 0;
        gpio_put(PIN_SR_CLOCK, 0); sleep_us(1);
        gpio_put(PIN_SR_CLOCK, 1); sleep_us(1);
    }
    return result;
}

void board_init(void)
{
    // Keep the board powered: on battery the hardware latch only holds VSYS for
    // a moment after a wake event.
    gpio_out(PIN_HOLD_VSYS_EN, true);

    gpio_out(PIN_SR_CLOCK, true);
    gpio_out(PIN_SR_LATCH, true);
    gpio_init(PIN_SR_OUT);
    gpio_set_dir(PIN_SR_OUT, GPIO_IN);

    uint8_t sr = board_read_sr();
    s_boot_sr = sr;
    if (sr & SR_BUTTON_A) s_wake = WAKE_BUTTON_A;
    if (sr & SR_BUTTON_B) s_wake = WAKE_BUTTON_B;
    if (sr & SR_BUTTON_C) s_wake = WAKE_BUTTON_C;
    if (sr & SR_BUTTON_D) s_wake = WAKE_BUTTON_D;
    if (sr & SR_BUTTON_E) s_wake = WAKE_BUTTON_E;
    if (sr & SR_RTC_ALARM) s_wake = WAKE_RTC_ALARM;
    if (sr & SR_EXT_TRIGGER) s_wake = WAKE_EXT_TRIGGER;

    for (int i = 0; i < LED_COUNT; i++) led_pwm_init(LED_PINS[i]);
}

wake_event_t board_wake_event(void) { return s_wake; }
uint8_t board_boot_sr(void) { return s_boot_sr; }

bool board_button_down(int index)
{
    if (index < 0 || index > 4) return false;
    return (board_read_sr() & (1u << index)) != 0;
}

int board_any_button_down(void)
{
    uint8_t sr = board_read_sr();
    for (int i = 0; i < 5; i++)
        if (sr & (1u << i)) return i;
    return -1;
}

bool board_epd_busy(void)
{
    return (board_read_sr() & SR_EINK_BUSY_N) == 0;
}

void board_led(led_t led, uint8_t percent)
{
    if (led >= LED_COUNT) return;
    if (percent > 100) percent = 100;
    uint16_t value = (uint16_t)(powf(percent / 100.0f, 2.8f) * 65535.0f + 0.5f);
    pwm_set_gpio_level(LED_PINS[led], value);
}

void board_led_button(int index, uint8_t percent)
{
    if (index < 0 || index > 4) return;
    board_led((led_t)(LED_A + index), percent);
}

void board_release_power(void)
{
    gpio_put(PIN_HOLD_VSYS_EN, false);
}

const char *board_wake_name(wake_event_t e)
{
    switch (e) {
    case WAKE_BUTTON_A: return "button A";
    case WAKE_BUTTON_B: return "button B";
    case WAKE_BUTTON_C: return "button C";
    case WAKE_BUTTON_D: return "button D";
    case WAKE_BUTTON_E: return "button E";
    case WAKE_RTC_ALARM: return "RTC alarm";
    case WAKE_EXT_TRIGGER: return "external trigger";
    default: return "power-on / reset";
    }
}
