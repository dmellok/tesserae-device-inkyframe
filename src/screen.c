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
// bands of 8 rows so only a 3,200-byte SRAM buffer is needed. Text is laid out
// by text(), which wraps and shrinks it to fit its column.
typedef struct {
    const char *text;
    int len;
    int x, y, scale;
    uint8_t colour;
} item_t;

#define MAX_ITEMS 48
static item_t s_items[MAX_ITEMS];
static int s_nitems;
static char s_pool[2048];   // copies of the page's strings, so callers' buffers may change
static size_t s_pool_n;

#define MARGIN    40
#define CONTENT_W (PANEL_W - 2 * MARGIN)
#define FOOTER_Y  (PANEL_H - 40)
#define BOTTOM    (FOOTER_Y - 12)   // body text stops above the footer

static void reset_page(void) { s_nitems = 0; s_pool_n = 0; }

static void add(const char *text, int len, int x, int y, int scale, uint8_t colour)
{
    if (s_nitems >= MAX_ITEMS || !text || len <= 0) return;
    s_items[s_nitems++] = (item_t){ text, len, x, y, scale, colour };
}

// Next line of `p` at most `cols` characters wide, broken at a space where
// possible. Sets *len to its length and returns where the following line starts.
static const char *wrap_line(const char *p, int cols, int *len)
{
    size_t n = strlen(p);
    if ((int)n <= cols) { *len = (int)n; return p + n; }
    int cut = cols;
    while (cut > 0 && p[cut] != ' ') cut--;
    if (cut == 0) { *len = cols; return p + cols; }   // one long word (a URL): break it
    *len = cut;
    while (p[cut] == ' ') cut++;
    return p + cut;
}

static int count_lines(const char *p, int cols)
{
    int lines = 0, len;
    while (*p == ' ') p++;
    while (*p) { p = wrap_line(p, cols, &len); lines++; }
    return lines;
}

// Lay `t` out in a column `w` pixels wide from (x, y): at the largest scale
// from `scale` down to `min_scale` that fits in `max_lines` lines, wrapping
// further at `min_scale` if it must. Returns the y just below the block.
static int text(const char *t, int x, int y, int w, int scale, int min_scale, int max_lines, uint8_t colour)
{
    if (!t || !*t) return y;
    while (*t == ' ') t++;
    int s = scale;
    while (s > min_scale && count_lines(t, w / (8 * s)) > max_lines) s--;
    int cols = w / (8 * s);
    size_t n = strlen(t);
    if (s_pool_n + n + 1 > sizeof s_pool) { LOGW("screen: text pool full"); return y; }
    char *copy = s_pool + s_pool_n;
    memcpy(copy, t, n + 1);
    s_pool_n += n + 1;
    int line_h = 8 * s + 2 * s + 2;
    const char *p = copy;
    while (*p) {
        int len;
        const char *next = wrap_line(p, cols, &len);
        if (y < FOOTER_Y && y + 8 * s > BOTTOM) { LOGW("screen: text cut at the footer"); break; }
        add(p, len, x, y, s, colour);
        y += line_h;
        p = next;
    }
    return y - (2 * s + 2);
}

// The Tesserae mark (the server's logo, as the setup portal draws it): an ink
// tile with rounded corners, a red square top right and a paper square bottom
// left, each with its outer corner rounded. Drawn from its 256-unit geometry
// at any size, so there is no bitmap to keep in step.
#define LOGO_X    MARGIN
#define LOGO_Y    20
#define LOGO_SIZE 48
static bool s_has_logo;

static bool in_round(float u, float v, float cx, float cy, float r)
{
    float dx = u - cx, dy = v - cy;
    return dx * dx + dy * dy <= r * r;
}

static int logo_colour(int px, int py)
{
    float u = (px + 0.5f) * 256.0f / LOGO_SIZE, v = (py + 0.5f) * 256.0f / LOGO_SIZE;
    // Outer tile, corner radius 72.
    float cx = u < 72 ? 72 : u > 184 ? 184 : u, cy = v < 72 ? 72 : v > 184 ? 184 : v;
    if (!in_round(u, v, cx, cy, 72)) return -1;
    if (u >= 128 && u < 201 && v >= 55 && v < 128 && !(u > 174 && v < 82 && !in_round(u, v, 174, 82, 27)))
        return PAL_RED;
    if (u >= 55 && u < 128 && v >= 128 && v < 201 && !(u < 82 && v > 174 && !in_round(u, v, 82, 174, 27)))
        return PAL_WHITE;
    return PAL_BLACK;
}

static void set_pixel(uint8_t *band, int x, int y, uint8_t c);

static void draw_logo(uint8_t *band, int band_y)
{
    if (!s_has_logo) return;
    for (int row = 0; row < 8; row++) {
        int py = band_y + row - LOGO_Y;
        if (py < 0 || py >= LOGO_SIZE) continue;
        for (int px = 0; px < LOGO_SIZE; px++) {
            int c = logo_colour(px, py);
            if (c >= 0) set_pixel(band, LOGO_X + px, row, (uint8_t)c);
        }
    }
}

// Every page opens with the mark and the name; its title goes below at HEADER_END.
#define HEADER_END (LOGO_Y + LOGO_SIZE + 24)
static void header(void)
{
    s_has_logo = true;
    text("Tesserae", LOGO_X + LOGO_SIZE + 16, LOGO_Y + (LOGO_SIZE - 24) / 2, 400, 3, 3, 1, PAL_BLACK);
}

static void footer(void)
{
    text("Inky Frame 7.3 / Tesserae client v" FW_VERSION, MARGIN, FOOTER_Y, CONTENT_W, 2, 2, 1, PAL_BLACK);
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
    for (int i = 0; i < it->len; i++) {
        unsigned char ch = (unsigned char)it->text[i];
        if (ch > 127) ch = '?';
        const char *glyph = font8x8_basic[ch];
        int gx = it->x + i * 8 * it->scale;
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
        draw_logo(band, y);
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
    reset_page();
    s_has_qr = false;
    s_has_logo = false;
    return epd_paint(psram_row, NULL);
}

bool screen_pairing(const char *device_id, const char *mac, const char *server_url, const char *kind, const char *note)
{
    static char l1[64], l2[64], l3[200], l4[64];
    snprintf(l1, sizeof l1, "device id: %s", device_id);
    snprintf(l2, sizeof l2, "mac: %s", mac);
    snprintf(l3, sizeof l3, "server: %s", server_url);
    snprintf(l4, sizeof l4, "kind: %s", kind);
    reset_page();
    header();
    int y = text("Register me", MARGIN, HEADER_END, CONTENT_W, 5, 3, 1, PAL_RED) + 28;
    y = text("Tesserae > Settings > Devices > Discovered", MARGIN, y, CONTENT_W, 2, 2, 2, PAL_BLACK) + 28;
    y = text(l1, MARGIN, y, CONTENT_W, 3, 2, 1, PAL_BLACK) + 14;
    y = text(l2, MARGIN, y, CONTENT_W, 3, 2, 1, PAL_BLACK) + 20;
    y = text(l3, MARGIN, y, CONTENT_W, 2, 2, 2, PAL_BLACK) + 10;
    y = text(l4, MARGIN, y, CONTENT_W, 2, 2, 2, PAL_BLACK) + 24;
    text(note, MARGIN, y, CONTENT_W, 2, 2, 3, PAL_BLUE);
    footer();
    LOGI("screen: pairing page");
    return paint();
}

bool screen_message(const char *title, const char *line1, const char *line2, const char *line3)
{
    reset_page();
    header();
    int y = text(title, MARGIN, HEADER_END, CONTENT_W, 4, 3, 1, PAL_RED) + 32;
    y = text(line1, MARGIN, y, CONTENT_W, 3, 2, 2, PAL_BLACK) + 24;
    y = text(line2, MARGIN, y, CONTENT_W, 2, 2, 3, PAL_BLACK) + 12;
    text(line3, MARGIN, y, CONTENT_W, 2, 2, 3, PAL_BLACK);
    footer();
    LOGI("screen: %s", title);
    return paint();
}


bool screen_setup(const char *ap_ssid, const char *ap_pass, const char *portal_url, const char *reason)
{
    static char l_ssid[48], l_pass[48], l_qr[128];
    snprintf(l_ssid, sizeof l_ssid, "%s", ap_ssid);
    snprintf(l_pass, sizeof l_pass, "password: %s", ap_pass);
    snprintf(l_qr, sizeof l_qr, "WIFI:T:WPA;S:%s;P:%s;;", ap_ssid, ap_pass);
    (void)portal_url;
    // Text keeps to the left of the QR code (x 540..780).
    const int col_w = 520 - MARGIN, ind = MARGIN + 32;
    reset_page();
    header();
    int y = text("Set up this frame", MARGIN, HEADER_END, CONTENT_W, 4, 3, 1, PAL_RED) + 20;
    y = text(reason, MARGIN, y, col_w, 2, 2, 5, PAL_BLUE) + 24;
    if (y < HEADER_END + 72) y = HEADER_END + 72;
    y = text("1. Join the Wi-Fi network", MARGIN, y, col_w, 2, 2, 2, PAL_BLACK) + 12;
    y = text(l_ssid, ind, y, col_w - 32, 2, 2, 2, PAL_BLACK) + 8;
    y = text(l_pass, ind, y, col_w - 32, 2, 2, 2, PAL_BLACK) + 24;
    y = text("2. Open " SETUP_PORTAL_URL, MARGIN, y, col_w, 2, 2, 2, PAL_BLACK) + 24;
    text("3. Enter your Wi-Fi + server", MARGIN, y, col_w, 2, 2, 2, PAL_BLACK);
    footer();
    set_qr(l_qr, 540, HEADER_END + 40, 240);
    LOGI("screen: setup page (%s)", ap_ssid);
    return paint();
}
