#include "screen.h"

#include <string.h>
#include <stdint.h>

#include "config.h"
#include "epd.h"
#include "log.h"
#include "psram.h"
#include "vendor/font8x8_basic.h"
#include "vendor/qrcodegen.h"

// A page is a small list of text items; the frame is composed into PSRAM in
// bands of 8 rows so only a 3,200-byte SRAM buffer is needed.
typedef struct {
    const char *text;
    int x, y, scale;
    uint8_t colour;
} item_t;

#define MAX_ITEMS 12
static item_t s_items[MAX_ITEMS];
static int s_nitems;

static void add(const char *text, int x, int y, int scale, uint8_t colour)
{
    if (s_nitems >= MAX_ITEMS || !text || !*text) return;
    s_items[s_nitems++] = (item_t){ text, x, y, scale, colour };
}


static void set_pixel(uint8_t *band, int x, int y, uint8_t c)
{
    if (x < 0 || x >= PANEL_W || y < 0 || y >= 8) return;
    uint8_t *b = band + y * ROW_BYTES + x / 2;
    if (x & 1) *b = (uint8_t)((*b & 0xF0) | (c & 0x0F));
    else       *b = (uint8_t)((*b & 0x0F) | (c << 4));
}

// Draw the parts of `it` that fall inside the band starting at row `band_y`.
static void draw_item(uint8_t *band, int band_y, const item_t *it)
{
    int h = 8 * it->scale;
    if (it->y >= band_y + 8 || it->y + h <= band_y) return;
    for (size_t i = 0; it->text[i]; i++) {
        unsigned char ch = (unsigned char)it->text[i];
        if (ch > 127) ch = '?';
        const char *glyph = font8x8_basic[ch];
        int gx = it->x + (int)i * 8 * it->scale;
        for (int row = 0; row < 8; row++) {
            uint8_t bits = (uint8_t)glyph[row];
            if (!bits) continue;
            for (int sy = 0; sy < it->scale; sy++) {
                int py = it->y + row * it->scale + sy;
                if (py < band_y || py >= band_y + 8) continue;
                for (int col = 0; col < 8; col++) {
                    if (!(bits & (1 << col))) continue;
                    for (int sx = 0; sx < it->scale; sx++)
                        set_pixel(band, gx + col * it->scale + sx, py - band_y, it->colour);
                }
            }
        }
    }
}

// One optional QR code per page, drawn as black modules on white.
static uint8_t s_qr[qrcodegen_BUFFER_LEN_FOR_VERSION(10)];
static bool s_has_qr;
static int s_qr_x, s_qr_y, s_qr_scale;

static void set_qr(const char *text, int x, int y, int max_px)
{
    static uint8_t tmp[qrcodegen_BUFFER_LEN_FOR_VERSION(10)];
    s_has_qr = qrcodegen_encodeText(text, tmp, s_qr, qrcodegen_Ecc_MEDIUM, 1, 10, qrcodegen_Mask_AUTO, true);
    if (!s_has_qr) { LOGW("screen: QR encode failed"); return; }
    int modules = qrcodegen_getSize(s_qr) + 8;   // 4-module quiet zone each side
    s_qr_scale = max_px / modules;
    if (s_qr_scale < 2) { s_has_qr = false; return; }
    s_qr_x = x + 4 * s_qr_scale;
    s_qr_y = y + 4 * s_qr_scale;
}

static void draw_qr(uint8_t *band, int band_y)
{
    if (!s_has_qr) return;
    int size = qrcodegen_getSize(s_qr);
    for (int row = 0; row < 8; row++) {
        int py = band_y + row - s_qr_y;
        if (py < 0 || py >= size * s_qr_scale) continue;
        int my = py / s_qr_scale;
        for (int mx = 0; mx < size; mx++) {
            if (!qrcodegen_getModule(s_qr, mx, my)) continue;
            for (int sx = 0; sx < s_qr_scale; sx++) set_pixel(band, s_qr_x + mx * s_qr_scale + sx, row, PAL_BLACK);
        }
    }
}

static void compose(void)
{
    static uint8_t band[ROW_BYTES * 8];
    for (int y = 0; y < PANEL_H; y += 8) {
        memset(band, (PAL_WHITE << 4) | PAL_WHITE, sizeof band);
        for (int i = 0; i < s_nitems; i++) draw_item(band, y, &s_items[i]);
        draw_qr(band, y);
        psram_write((uint32_t)y * ROW_BYTES, band, sizeof band);
    }
}

static void psram_row(int y, uint8_t *out, void *arg)
{
    (void)arg;
    psram_read((uint32_t)y * ROW_BYTES, out, ROW_BYTES);
}

static bool paint(void)
{
    compose();
    s_nitems = 0;
    s_has_qr = false;
    return epd_paint(psram_row, NULL);
}

bool screen_pairing(const char *device_id, const char *mac, const char *server_url, const char *kind, const char *note)
{
    static char l1[64], l2[64], l3[96], l4[64];
    snprintf(l1, sizeof l1, "device id: %s", device_id);
    snprintf(l2, sizeof l2, "mac: %s", mac);
    snprintf(l3, sizeof l3, "server: %s", server_url);
    snprintf(l4, sizeof l4, "kind: %s", kind);
    s_nitems = 0;
    add("Register me", 40, 48, 5, PAL_RED);
    add("Tesserae > Settings > Devices > Discovered", 40, 120, 2, PAL_BLACK);
    add(l1, 40, 176, 3, PAL_BLACK);
    add(l2, 40, 216, 3, PAL_BLACK);
    add(l3, 40, 256, 2, PAL_BLACK);
    add(l4, 40, 288, 2, PAL_BLACK);
    add(note, 40, 336, 2, PAL_BLUE);
    add("Inky Frame 7.3 / Tesserae client v" FW_VERSION, 40, 440, 2, PAL_BLACK);
    LOGI("screen: pairing page");
    return paint();
}

bool screen_message(const char *title, const char *line1, const char *line2, const char *line3)
{
    s_nitems = 0;
    add(title, 40, 48, 4, PAL_RED);
    add(line1, 40, 140, 3, PAL_BLACK);
    add(line2, 40, 184, 2, PAL_BLACK);
    add(line3, 40, 216, 2, PAL_BLACK);
    add("Inky Frame 7.3 / Tesserae client v" FW_VERSION, 40, 440, 2, PAL_BLACK);
    LOGI("screen: %s", title);
    return paint();
}


bool screen_setup(const char *ap_ssid, const char *ap_pass, const char *portal_url, const char *reason)
{
    static char l_ssid[48], l_pass[48], l_qr[128];
    snprintf(l_ssid, sizeof l_ssid, "%s", ap_ssid);
    snprintf(l_pass, sizeof l_pass, "password: %s", ap_pass);
    snprintf(l_qr, sizeof l_qr, "WIFI:T:WPA;S:%s;P:%s;;", ap_ssid, ap_pass);
    s_nitems = 0;
    add("Set up this frame", 40, 40, 4, PAL_RED);
    add(reason, 40, 92, 2, PAL_BLUE);
    add("1. Join the Wi-Fi network", 40, 140, 2, PAL_BLACK);
    add(l_ssid, 72, 172, 2, PAL_BLACK);
    add(l_pass, 72, 200, 2, PAL_BLACK);
    add("2. Open " SETUP_PORTAL_URL, 40, 248, 2, PAL_BLACK);
    add("3. Enter your Wi-Fi + server", 40, 296, 2, PAL_BLACK);
    add("Inky Frame 7.3 / Tesserae client v" FW_VERSION, 40, 440, 2, PAL_BLACK);
    set_qr(l_qr, 540, 120, 240);
    LOGI("screen: setup page (%s)", ap_ssid);
    return paint();
}
