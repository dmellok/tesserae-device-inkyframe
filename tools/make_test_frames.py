#!/usr/bin/env python3
"""Generate a synthetic Tesserae-format test frame for the Inky Frame 7.3 self-test.

test.bin  800x480, 4 bpp palette indices (pico_bin layout, high nibble = left pixel):
          eight 100-px-wide bars of palette index 0..7 across the top 120 rows,
          a black border, and a solid black triangle whose apex points at the
          LEFT edge (x = 0) of the native landscape framebuffer.

On a landscape mount the bars should be along the top and the triangle should
point LEFT. Serve with `python3 -m http.server 8000` from this folder.
"""
import sys, pathlib

W, H = 800, 480
BLACK, WHITE = 0, 1
out = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else ".")

def px(x, y):
    if x < 8 or x >= W - 8 or y < 8 or y >= H - 8:
        return BLACK                                   # border
    if y < 120:
        return min(x // 100, 7)                        # palette bars 0..7
    # Triangle: apex at (40, 300), base at x = 420 from y = 170..430.
    if 40 <= x <= 420:
        half = (x - 40) * 130 // 380
        if abs(y - 300) <= half:
            return BLACK
    return WHITE

buf = bytearray(W * H // 2)
i = 0
for y in range(H):
    for x in range(0, W, 2):
        buf[i] = (px(x, y) << 4) | px(x + 1, y)
        i += 1
(out / "test.bin").write_bytes(buf)
print("wrote", out / "test.bin", len(buf), "bytes")
