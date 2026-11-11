#include "power.h"

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "hardware/adc.h"
#include "hardware/gpio.h"

#include "board.h"
#include "config.h"
#include "log.h"
#include "rtc.h"

static uint32_t s_vsys_mv;

// Pico W: GP29 doubles as the wireless SPI clock, so VSYS is sampled with the
// CYW43 held idle (the recipe from pico-examples adc/read_vsys). Needs the
// CYW43 initialised.
uint32_t power_sample_vsys_mv(void)
{
    cyw43_thread_enter();
    cyw43_arch_gpio_get(CYW43_WL_GPIO_VBUS_PIN);   // make sure the CYW43 is awake
    adc_init();
    adc_gpio_init(PIN_VSYS_ADC);
    adc_select_input(PIN_VSYS_ADC - 26);
    adc_fifo_setup(true, false, 0, false, false);
    adc_run(true);
    int ignore = 8;
    while (!adc_fifo_is_empty() || ignore-- > 0) (void)adc_fifo_get_blocking();
    uint32_t sum = 0;
    for (int i = 0; i < 8; i++) sum += adc_fifo_get_blocking();
    adc_run(false);
    adc_fifo_drain();
    cyw43_thread_exit();
    uint32_t raw = sum / 8;
    s_vsys_mv = raw * 3u * 3300u / 4096u;
    return s_vsys_mv;
}

uint32_t power_vsys_mv(void) { return s_vsys_mv; }

int32_t power_clamp_sleep_s(int32_t s)
{
    if (s < SLEEP_MIN_S || s > SLEEP_MAX_S) return SLEEP_DEFAULT_S;
    return s;
}

void power_hold_over_reboot(void)
{
    bool flag = pcf_assert_int();
    sleep_ms(2);
    uint8_t sr = board_read_sr();
    if (flag && (sr & SR_RTC_ALARM)) LOGI("power: RTC holds the board on over the restart (sr 0x%02x)", sr);
    else LOGW("power: the RTC is not holding the board on (flag %d, sr 0x%02x); on battery this restart powers off", flag, sr);
}

void power_sleep(uint32_t seconds, bool on_usb, int *button)
{
    if (button) *button = -1;
    if (seconds < 1) seconds = 1;

    bool alarm = pcf_time_valid() && pcf_get_epoch() > 1600000000u;
    if (alarm) {
        pcf_arm_alarm_epoch(pcf_get_epoch() + seconds);
    } else {
        uint32_t minutes = (seconds + 59) / 60;
        if (minutes > 255) minutes = 255;
        pcf_arm_timer_minutes((uint8_t)minutes);
        LOGW("sleep: clock not set, using the RTC countdown timer (%lu min)", (unsigned long)minutes);
    }
    LOGI("sleep: %lu s (%s)", (unsigned long)seconds, on_usb ? "USB: staying resident" : "battery: powering off");
    stdio_flush();

    for (int i = 0; i < LED_COUNT; i++) board_led((led_t)i, 0);
    board_release_power();
    sleep_ms(200);
    // Still running: powered from USB. Emulate the sleep and report what ended it.
    absolute_time_t deadline = make_timeout_time_ms(seconds * 1000u + 2000u);
    uint32_t last_rtc_check = 0;
    uint8_t last_sr = board_read_sr();
    while (!time_reached(deadline)) {
        uint8_t sr = board_read_sr();
        if (sr != last_sr) { LOGI("sr: 0x%02x -> 0x%02x", last_sr, sr); last_sr = sr; }
        int b = -1;
        for (int i = 0; i < 5; i++) if (sr & (1u << i)) { b = i; break; }
        if (b >= 0) {
            // Refresh is returned while still down, so the caller can time a
            // setup / factory-reset hold; other buttons wait for the release.
            absolute_time_t t = make_timeout_time_ms(2000);
            while (b != BUTTON_SETUP_INDEX && board_button_down(b) && !time_reached(t)) sleep_ms(20);
            if (button) *button = b;
            break;
        }
        uint32_t now = to_ms_since_boot(get_absolute_time());
        if (now - last_rtc_check > 1000) {
            last_rtc_check = now;
            if (pcf_alarm_fired()) break;
        }
        sleep_ms(50);
    }
    gpio_put(PIN_HOLD_VSYS_EN, true);
    pcf_disarm();
}
