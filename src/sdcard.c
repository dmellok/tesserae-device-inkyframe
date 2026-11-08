#include "sdcard.h"

#include <string.h>
#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "hardware/gpio.h"

#include "config.h"
#include "log.h"
#include "vendor/fatfs/ff.h"
#include "vendor/fatfs/diskio.h"

#define SD_SPI          spi0
#define SD_INIT_BAUD    400000
#define SD_BAUD         12500000

static bool s_ready;        // card initialised
static bool s_block_addr;   // SDHC/SDXC: addresses are block numbers
static bool s_mounted;
static FATFS s_fs;
static uint32_t s_init_cpsr, s_init_cr0, s_fast_cpsr, s_fast_cr0;
static bool s_fast;

// ---- the shared bus (as epd.c and psram.c do) ----

static uint32_t s_prev_cpsr, s_prev_cr0;

static void claim(void)
{
    s_prev_cpsr = spi_get_hw(SD_SPI)->cpsr;
    s_prev_cr0 = spi_get_hw(SD_SPI)->cr0;
    spi_get_hw(SD_SPI)->cpsr = s_fast ? s_fast_cpsr : s_init_cpsr;
    spi_get_hw(SD_SPI)->cr0 = s_fast ? s_fast_cr0 : s_init_cr0;
}

static void release(void)
{
    spi_get_hw(SD_SPI)->cpsr = s_prev_cpsr;
    spi_get_hw(SD_SPI)->cr0 = s_prev_cr0;
}

static inline void cs(bool low) { gpio_put(PIN_SD_CS, !low); }

static uint8_t xfer(uint8_t b)
{
    uint8_t r;
    spi_write_read_blocking(SD_SPI, &b, &r, 1);
    return r;
}

static bool wait_ready(uint32_t ms)
{
    absolute_time_t t = make_timeout_time_ms(ms);
    while (xfer(0xFF) != 0xFF) if (time_reached(t)) return false;
    return true;
}

static void deselect(void) { cs(false); xfer(0xFF); }

static bool select_card(void)
{
    cs(true);
    xfer(0xFF);
    if (wait_ready(500)) return true;
    deselect();
    return false;
}

// Send a command; returns R1 (0xFF on timeout). ACMDs are prefixed with CMD55.
static uint8_t command(uint8_t cmd, uint32_t arg)
{
    if (cmd & 0x80) {
        cmd &= 0x7F;
        uint8_t r = command(55, 0);
        if (r > 1) return r;
    }
    if (cmd != 12) { deselect(); if (!select_card()) return 0xFF; }
    uint8_t frame[6] = { (uint8_t)(0x40 | cmd), (uint8_t)(arg >> 24), (uint8_t)(arg >> 16), (uint8_t)(arg >> 8), (uint8_t)arg,
                         cmd == 0 ? 0x95 : cmd == 8 ? 0x87 : 0x01 };
    spi_write_blocking(SD_SPI, frame, sizeof frame);
    if (cmd == 12) xfer(0xFF);   // stuff byte after STOP_TRANSMISSION
    uint8_t r;
    int n = 10;
    do { r = xfer(0xFF); } while ((r & 0x80) && --n);
    return r;
}

#define ACMD(n) (0x80 | (n))

static bool card_init(void)
{
    gpio_init(PIN_SD_CS);
    gpio_set_dir(PIN_SD_CS, GPIO_OUT);
    cs(false);
    gpio_pull_up(PIN_SPI_MISO);
    gpio_set_function(PIN_SPI_MISO, GPIO_FUNC_SPI);

    // The bus settings for each speed, captured once.
    uint32_t pc = spi_get_hw(SD_SPI)->cpsr, pr = spi_get_hw(SD_SPI)->cr0;
    spi_set_baudrate(SD_SPI, SD_INIT_BAUD);
    spi_set_format(SD_SPI, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    s_init_cpsr = spi_get_hw(SD_SPI)->cpsr; s_init_cr0 = spi_get_hw(SD_SPI)->cr0;
    spi_set_baudrate(SD_SPI, SD_BAUD);
    s_fast_cpsr = spi_get_hw(SD_SPI)->cpsr; s_fast_cr0 = spi_get_hw(SD_SPI)->cr0;
    spi_get_hw(SD_SPI)->cpsr = pc; spi_get_hw(SD_SPI)->cr0 = pr;

    s_fast = false;
    claim();
    for (int i = 0; i < 10; i++) xfer(0xFF);   // 80 clocks with CS high
    bool ok = false;
    if (command(0, 0) == 1) {
        absolute_time_t t = make_timeout_time_ms(1000);
        if (command(8, 0x1AA) == 1) {
            uint8_t r7[4];
            for (int i = 0; i < 4; i++) r7[i] = xfer(0xFF);
            if (r7[2] == 0x01 && r7[3] == 0xAA) {   // SD v2
                while (!time_reached(t) && command(ACMD(41), 1u << 30)) sleep_ms(1);
                if (!time_reached(t) && command(58, 0) == 0) {
                    uint8_t ocr[4];
                    for (int i = 0; i < 4; i++) ocr[i] = xfer(0xFF);
                    s_block_addr = (ocr[0] & 0x40) != 0;
                    ok = true;
                }
            }
        } else {   // SD v1
            while (!time_reached(t) && command(ACMD(41), 0)) sleep_ms(1);
            ok = !time_reached(t) && command(16, 512) == 0;
            s_block_addr = false;
        }
    }
    deselect();
    release();
    s_fast = ok;
    LOGI("sd: %s%s", ok ? "card ready" : "no card", ok ? (s_block_addr ? " (SDHC/SDXC)" : " (SDSC)") : "");
    return ok;
}

static bool read_block_data(uint8_t *buf)
{
    absolute_time_t t = make_timeout_time_ms(200);
    uint8_t tok;
    do { tok = xfer(0xFF); } while (tok == 0xFF && !time_reached(t));
    if (tok != 0xFE) return false;
    static uint8_t ff[512];
    memset(ff, 0xFF, sizeof ff);
    spi_write_read_blocking(SD_SPI, ff, buf, 512);
    xfer(0xFF); xfer(0xFF);   // CRC
    return true;
}

static bool write_block_data(const uint8_t *buf, uint8_t token)
{
    if (!wait_ready(500)) return false;
    xfer(token);
    if (token == 0xFD) return true;   // stop-tran token
    spi_write_blocking(SD_SPI, buf, 512);
    xfer(0xFF); xfer(0xFF);           // dummy CRC
    return (xfer(0xFF) & 0x1F) == 0x05;
}

// ---- FatFs disk I/O (one physical drive) ----

DSTATUS disk_status(BYTE pdrv) { return (pdrv || !s_ready) ? STA_NOINIT : 0; }

DSTATUS disk_initialize(BYTE pdrv)
{
    if (pdrv) return STA_NOINIT;
    if (!s_ready) s_ready = card_init();
    return s_ready ? 0 : STA_NOINIT;
}

DRESULT disk_read(BYTE pdrv, BYTE *buf, LBA_t sector, UINT count)
{
    if (pdrv || !s_ready) return RES_NOTRDY;
    uint32_t addr = s_block_addr ? (uint32_t)sector : (uint32_t)sector * 512u;
    claim();
    bool ok;
    if (count == 1) {
        ok = command(17, addr) == 0 && read_block_data(buf);
    } else {
        ok = command(18, addr) == 0;
        for (UINT i = 0; ok && i < count; i++) ok = read_block_data(buf + i * 512);
        command(12, 0);
    }
    deselect();
    release();
    return ok ? RES_OK : RES_ERROR;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buf, LBA_t sector, UINT count)
{
    if (pdrv || !s_ready) return RES_NOTRDY;
    uint32_t addr = s_block_addr ? (uint32_t)sector : (uint32_t)sector * 512u;
    claim();
    bool ok;
    if (count == 1) {
        ok = command(24, addr) == 0 && write_block_data(buf, 0xFE);
    } else {
        ok = command(25, addr) == 0;
        for (UINT i = 0; ok && i < count; i++) ok = write_block_data(buf + i * 512, 0xFC);
        if (!write_block_data(NULL, 0xFD)) ok = false;
    }
    if (!wait_ready(500)) ok = false;
    deselect();
    release();
    return ok ? RES_OK : RES_ERROR;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
    if (pdrv || !s_ready) return RES_NOTRDY;
    if (cmd == CTRL_SYNC) {
        claim();
        bool ok = select_card();
        deselect();
        release();
        return ok ? RES_OK : RES_ERROR;
    }
    if (cmd == GET_BLOCK_SIZE) { *(DWORD *)buff = 1; return RES_OK; }
    return RES_PARERR;
}

// ---- mount ----

bool sdcard_mount(void)
{
    if (s_mounted) return true;
    FRESULT r = f_mount(&s_fs, "", 1);
    s_mounted = r == FR_OK;
    if (!s_mounted && s_ready) LOGW("sd: card found but not mountable (FatFs %d); is it FAT32 or exFAT?", r);
    if (s_mounted) LOGI("sd: mounted, %lu MB free", (unsigned long)(sdcard_free_bytes() >> 20));
    return s_mounted;
}

bool sdcard_mounted(void) { return s_mounted; }

uint64_t sdcard_free_bytes(void)
{
    if (!s_mounted) return 0;
    DWORD clusters;
    FATFS *fs;
    if (f_getfree("", &clusters, &fs) != FR_OK) return 0;
    return (uint64_t)clusters * fs->csize * 512u;
}
