#include "rtc.h"

#include "pico/stdlib.h"
#include "hardware/i2c.h"
#include "hardware/gpio.h"

#include "config.h"
#include "log.h"

#define RTC_I2C      i2c0
#define RTC_ADDR     0x51

enum {
    REG_CONTROL_1 = 0x00,
    REG_CONTROL_2 = 0x01,
    REG_OFFSET = 0x02,
    REG_RAM_BYTE = 0x03,
    REG_SECONDS = 0x04,
    REG_MINUTES = 0x05,
    REG_HOURS = 0x06,
    REG_DAYS = 0x07,
    REG_WEEKDAYS = 0x08,
    REG_MONTHS = 0x09,
    REG_YEARS = 0x0A,
    REG_SECOND_ALARM = 0x0B,
    REG_TIMER_VALUE = 0x10,
    REG_TIMER_MODE = 0x11,
};

// CONTROL_2 bits
#define C2_AIE   0x80
#define C2_AF    0x40
#define C2_MI    0x20
#define C2_HMI   0x10
#define C2_TF    0x08
#define C2_COF_OFF 0x07   // clock output off

static bool s_valid = false;

static uint8_t bcd_enc(uint v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }
static uint8_t bcd_dec(uint8_t v) { return (uint8_t)(((v >> 4) & 0x0F) * 10 + (v & 0x0F)); }

static bool reg_write(uint8_t reg, const uint8_t *data, size_t n)
{
    uint8_t buf[16];
    if (n + 1 > sizeof buf) return false;
    buf[0] = reg;
    for (size_t i = 0; i < n; i++) buf[i + 1] = data[i];
    return i2c_write_timeout_us(RTC_I2C, RTC_ADDR, buf, n + 1, false, 20000) == (int)(n + 1);
}

static bool reg_read(uint8_t reg, uint8_t *data, size_t n)
{
    if (i2c_write_timeout_us(RTC_I2C, RTC_ADDR, &reg, 1, true, 20000) != 1) return false;
    return i2c_read_timeout_us(RTC_I2C, RTC_ADDR, data, n, false, 20000) == (int)n;
}

static uint8_t reg_read8(uint8_t reg)
{
    uint8_t v = 0;
    reg_read(reg, &v, 1);
    return v;
}

static void reg_write8(uint8_t reg, uint8_t v) { reg_write(reg, &v, 1); }

// ---- civil date helpers (proleptic Gregorian, UTC) ----
static int64_t days_from_civil(int y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

static void civil_from_days(int64_t z, int *y, unsigned *m, unsigned *d)
{
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int yy = (int)yoe + (int)era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp + (mp < 10 ? 3 : -9);
    *y = yy + (*m <= 2);
}

void pcf_init(void)
{
    i2c_init(RTC_I2C, 100 * 1000);
    gpio_set_function(PIN_I2C_SDA, GPIO_FUNC_I2C);
    gpio_set_function(PIN_I2C_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(PIN_I2C_SDA);
    gpio_pull_up(PIN_I2C_SCL);

    // Normal mode, 12.5 pF, 24 h, clock running.
    reg_write8(REG_CONTROL_1, 0x00);
    uint8_t sec = 0;
    if (!reg_read(REG_SECONDS, &sec, 1)) {
        LOGE("rtc: no response at 0x51");
        s_valid = false;
        return;
    }
    s_valid = (sec & 0x80) == 0;   // OS flag set = oscillator stopped since last set
    pcf_disarm();
    LOGI("rtc: %s", s_valid ? "clock valid" : "clock not set");
}

bool pcf_time_valid(void) { return s_valid; }

uint32_t pcf_get_epoch(void)
{
    uint8_t r[7] = {0};
    if (!reg_read(REG_SECONDS, r, 7)) return 0;
    unsigned sec = bcd_dec(r[0] & 0x7F), min = bcd_dec(r[1]), hour = bcd_dec(r[2] & 0x3F);
    unsigned day = bcd_dec(r[3] & 0x3F), mon = bcd_dec(r[5] & 0x1F);
    int year = 2000 + bcd_dec(r[6]);
    if (mon < 1 || mon > 12 || day < 1 || day > 31) return 0;
    int64_t days = days_from_civil(year, mon, day);
    return (uint32_t)(days * 86400 + hour * 3600 + min * 60 + sec);
}

void pcf_set_epoch(uint32_t epoch)
{
    int y; unsigned m, d;
    civil_from_days((int64_t)(epoch / 86400), &y, &m, &d);
    unsigned rem = epoch % 86400;
    unsigned wd = (unsigned)((epoch / 86400 + 4) % 7);   // 1970-01-01 was a Thursday
    uint8_t data[7] = {
        bcd_enc(rem % 60), bcd_enc((rem / 60) % 60), bcd_enc(rem / 3600),
        bcd_enc(d), bcd_enc(wd), bcd_enc(m), bcd_enc((unsigned)(y - 2000)),
    };
    if (reg_write(REG_SECONDS, data, 7)) s_valid = true;
}

static void clear_flags(void)
{
    uint8_t c2 = reg_read8(REG_CONTROL_2);
    c2 &= (uint8_t)~(C2_AF | C2_TF);
    c2 |= C2_COF_OFF;   // keep the clock output disabled
    reg_write8(REG_CONTROL_2, c2);
}

void pcf_arm_alarm_epoch(uint32_t epoch)
{
    int y; unsigned m, d;
    civil_from_days((int64_t)(epoch / 86400), &y, &m, &d);
    unsigned rem = epoch % 86400;
    uint8_t alarm[5] = {
        bcd_enc(rem % 60), bcd_enc((rem / 60) % 60), bcd_enc(rem / 3600), bcd_enc(d), 0x80 /* weekday: ignore */
    };
    reg_write(REG_SECOND_ALARM, alarm, 5);
    clear_flags();
    uint8_t c2 = reg_read8(REG_CONTROL_2);
    reg_write8(REG_CONTROL_2, (uint8_t)((c2 | C2_AIE | C2_AF) & ~C2_TF));   // writing AF=1 leaves it unchanged
}

void pcf_arm_timer_minutes(uint8_t minutes)
{
    if (minutes == 0) minutes = 1;
    clear_flags();
    uint8_t timer[2] = { minutes, (uint8_t)((0x03 << 3) | 0x04 | 0x02) };   // 1/60 Hz, TE, TIE
    reg_write(REG_TIMER_VALUE, timer, 2);
}

void pcf_disarm(void)
{
    uint8_t tm = reg_read8(REG_TIMER_MODE);
    reg_write8(REG_TIMER_MODE, (uint8_t)(tm & ~0x07));   // TE=0, TIE=0, TI_TP=0
    uint8_t c2 = reg_read8(REG_CONTROL_2);
    c2 &= (uint8_t)~(C2_AIE | C2_AF | C2_TF | C2_MI | C2_HMI);
    c2 |= C2_COF_OFF;
    reg_write8(REG_CONTROL_2, c2);
    uint8_t off[5] = { 0x80, 0x80, 0x80, 0x80, 0x80 };
    reg_write(REG_SECOND_ALARM, off, 5);
}

bool pcf_alarm_fired(void)
{
    uint8_t c2 = reg_read8(REG_CONTROL_2);
    return (c2 & (C2_AF | C2_TF)) != 0;
}

uint8_t pcf_ram_get(void) { return reg_read8(REG_RAM_BYTE); }
void pcf_ram_set(uint8_t v) { reg_write8(REG_RAM_BYTE, v); }
