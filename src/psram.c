#include "psram.h"

#include <string.h>
#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "hardware/gpio.h"

#include "config.h"
#include "log.h"

#define PSRAM_SPI      spi0
#define PSRAM_BAUD     31250000u
#define CMD_WRITE      0x02
#define CMD_READ       0x03
#define CMD_WRAP       0xC0
#define CMD_RESET_EN   0x66
#define CMD_RESET      0x99

// tCEM caps CS-low time at 8 us. At 31.25 MHz that is ~31 bytes per
// transaction including the 4-byte command, so 24 data bytes per burst.
#define CHUNK          24

static uint32_t s_cpsr, s_cr0;

static inline void cs(bool low) { gpio_put(PIN_PSRAM_CS, !low); }

// The panel driver shares SPI0 at a different clock; restore ours per call.
static void claim(uint32_t *cpsr, uint32_t *cr0)
{
    *cpsr = spi_get_hw(PSRAM_SPI)->cpsr;
    *cr0 = spi_get_hw(PSRAM_SPI)->cr0;
    if (*cpsr != s_cpsr || *cr0 != s_cr0) {
        spi_get_hw(PSRAM_SPI)->cpsr = s_cpsr;
        spi_get_hw(PSRAM_SPI)->cr0 = s_cr0;
    }
}

static void release(uint32_t cpsr, uint32_t cr0)
{
    if (cpsr != s_cpsr || cr0 != s_cr0) {
        spi_get_hw(PSRAM_SPI)->cpsr = cpsr;
        spi_get_hw(PSRAM_SPI)->cr0 = cr0;
    }
}

static void command(uint8_t c)
{
    cs(true);
    spi_write_blocking(PSRAM_SPI, &c, 1);
    cs(false);
}

static void write_burst(uint32_t addr, const uint8_t *data, size_t len)
{
    uint8_t hdr[4] = { CMD_WRITE, (uint8_t)(addr >> 16), (uint8_t)(addr >> 8), (uint8_t)addr };
    cs(true);
    spi_write_blocking(PSRAM_SPI, hdr, 4);
    spi_write_blocking(PSRAM_SPI, data, len);
    cs(false);
}

static void read_burst(uint32_t addr, uint8_t *data, size_t len)
{
    uint8_t hdr[4] = { CMD_READ, (uint8_t)(addr >> 16), (uint8_t)(addr >> 8), (uint8_t)addr };
    cs(true);
    spi_write_blocking(PSRAM_SPI, hdr, 4);
    spi_read_blocking(PSRAM_SPI, 0, data, len);
    cs(false);
}

static bool is_wrapped(void)
{
    // A burst that straddles a 1 KB boundary reads back wrong in wrap mode.
    enum { N = 40 };
    const uint32_t addr = 0x400000 - N / 2;
    uint8_t pattern[N], back[N];
    for (int i = 0; i < N; i++) pattern[i] = (uint8_t)(i * 7 + 1);
    write_burst(addr, pattern, N);
    read_burst(addr, back, N);
    return memcmp(pattern, back, N) != 0;
}

bool psram_init(void)
{
    uint32_t prev_cpsr = spi_get_hw(PSRAM_SPI)->cpsr, prev_cr0 = spi_get_hw(PSRAM_SPI)->cr0;
    uint baud = spi_init(PSRAM_SPI, PSRAM_BAUD);
    s_cpsr = spi_get_hw(PSRAM_SPI)->cpsr;
    s_cr0 = spi_get_hw(PSRAM_SPI)->cr0;

    gpio_init(PIN_PSRAM_CS);
    gpio_set_dir(PIN_PSRAM_CS, GPIO_OUT);
    gpio_put(PIN_PSRAM_CS, 1);
    gpio_set_function(PIN_SPI_SCK, GPIO_FUNC_SPI);
    gpio_set_function(PIN_SPI_MOSI, GPIO_FUNC_SPI);
    gpio_set_function(PIN_SPI_MISO, GPIO_FUNC_SPI);

    command(CMD_RESET_EN);
    sleep_us(1);
    command(CMD_RESET);
    sleep_us(10);

    bool ok = true;
    if (is_wrapped()) {
        command(CMD_WRAP);
        if (is_wrapped()) { LOGE("psram: stuck in wrapped burst mode"); ok = false; }
    }
    if (prev_cpsr != 0) release(prev_cpsr, prev_cr0);
    LOGI("psram: bus at %u Hz, %s", baud, ok ? "ok" : "FAILED");
    return ok;
}

void psram_write(uint32_t addr, const uint8_t *data, size_t len)
{
    uint32_t c, r;
    claim(&c, &r);
    while (len) {
        size_t n = len < CHUNK ? len : CHUNK;
        write_burst(addr, data, n);
        addr += n; data += n; len -= n;
    }
    release(c, r);
}

void psram_fill(uint32_t addr, uint8_t byte, size_t len)
{
    uint8_t buf[CHUNK];
    memset(buf, byte, sizeof buf);
    uint32_t c, r;
    claim(&c, &r);
    while (len) {
        size_t n = len < CHUNK ? len : CHUNK;
        write_burst(addr, buf, n);
        addr += n; len -= n;
    }
    release(c, r);
}

void psram_read(uint32_t addr, uint8_t *data, size_t len)
{
    uint32_t c, r;
    claim(&c, &r);
    while (len) {
        size_t n = len < CHUNK ? len : CHUNK;
        read_burst(addr, data, n);
        addr += n; data += n; len -= n;
    }
    release(c, r);
}

bool psram_test(void)
{
    uint8_t pat[256], back[256];
    for (int i = 0; i < 256; i++) pat[i] = (uint8_t)(i ^ 0xA5);
    // Beyond the frame region, and across a 24-byte chunk boundary.
    psram_write(0x40000 + 10, pat, sizeof pat);
    psram_read(0x40000 + 10, back, sizeof back);
    return memcmp(pat, back, sizeof pat) == 0;
}
