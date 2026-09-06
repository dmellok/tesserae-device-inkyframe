#!/usr/bin/env python3
"""Generate a labelled palette test image for the Inky Frame 7.3".

Writes palette_test.png, 800x480, containing the seven ACeP primaries as
labelled bars plus a row of secondary patches. Push it to the panel as a
dashboard image: every bar should show the colour its label names. A bar whose
colour does not match its label tells you exactly how the panel's palette order
differs from the one the server packed.

No third-party modules: the PNG is written directly with zlib, and the text is
drawn with the same public-domain 8x8 font the firmware uses
(src/vendor/font8x8_basic.h).
"""
import pathlib
import re
import struct
import sys
import zlib

W, H = 800, 480
HERE = pathlib.Path(__file__).resolve().parent
FONT_H = HERE.parent / "src" / "vendor" / "font8x8_basic.h"

# The seven ACeP (AC073TC1A) primaries, in the panel's own palette-index order.
# The server packs these same indices for the inky_7colour gamut.
BARS = [
    ("BLACK",  (0, 0, 0)),
    ("WHITE",  (255, 255, 255)),
    ("GREEN",  (0, 255, 0)),
    ("BLUE",   (0, 0, 255)),
    ("RED",    (255, 0, 0)),
    ("YELLOW", (255, 255, 0)),
    ("ORANGE", (255, 140, 0)),
]


def load_font():
    """Parse font8x8_basic.h into {codepoint: [8 row bitmaps]}."""
    text = FONT_H.read_text()
    rows = [int(v, 16) for v in re.findall(r"0x([0-9A-Fa-f]{2})", text)]
    return {i: rows[i * 8:(i + 1) * 8] for i in range(len(rows) // 8)}


FONT = load_font()


def draw_text(px, s, x, y, scale, colour):
    """Draw `s` at (x, y). Font rows are LSB = leftmost pixel."""
    for i, ch in enumerate(s):
        glyph = FONT.get(ord(ch))
        if not glyph:
            continue
        for row in range(8):
            bits = glyph[row]
            for col in range(8):
                if not bits & (1 << col):
                    continue
                for sy in range(scale):
                    for sx in range(scale):
                        pxx = x + (i * 8 + col) * scale + sx
                        pyy = y + row * scale + sy
                        if 0 <= pxx < W and 0 <= pyy < H:
                            px[pyy][pxx] = colour


def write_png(path, px):
    raw = b"".join(b"\x00" + bytes(v for p in row for v in p) for row in px)
    def chunk(tag, data):
        c = tag + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c))
    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 2, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(raw, 9))
           + chunk(b"IEND", b""))
    path.write_bytes(png)


def main():
    out = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else HERE) / "palette_test.png"
    px = [[(255, 255, 255)] * W for _ in range(H)]

    bar_w = W // len(BARS)
    bar_top, bar_bottom = 60, 340
    for i, (name, rgb) in enumerate(BARS):
        x0 = i * bar_w
        x1 = W if i == len(BARS) - 1 else x0 + bar_w
        for y in range(bar_top, bar_bottom):
            for x in range(x0, x1):
                px[y][x] = rgb
        # Label above each bar, in black, plus the palette index below it.
        label_x = x0 + max(2, (bar_w - len(name) * 8 * 2) // 2)
        draw_text(px, name, label_x, 30, 2, (0, 0, 0))
        idx = str(i)
        draw_text(px, idx, x0 + (bar_w - 8 * 3) // 2, bar_bottom + 10, 3, (0, 0, 0))

    draw_text(px, "INKY FRAME 7.3 PALETTE TEST", 120, 6, 2, (0, 0, 0))
    draw_text(px, "EACH BAR SHOULD MATCH ITS LABEL", 100, 400, 2, (0, 0, 0))
    draw_text(px, "REPORT ANY BAR THAT DOES NOT", 120, 430, 2, (255, 0, 0))

    write_png(out, px)
    print("wrote", out, out.stat().st_size, "bytes")


if __name__ == "__main__":
    main()
