#!/usr/bin/env python3
"""Check a built image's picobin header: version, and the TBYB flag.

    tools/check_image.py <firmware.bin> <x.y.z> [--tbyb | --no-tbyb]

The boot ROM picks the A/B slot with the higher version and only gives a TBYB
image its trial boot, so a release image must carry both correctly.
"""
import struct
import sys

path, version, mode = sys.argv[1], sys.argv[2], (sys.argv[3] if len(sys.argv) > 3 else "")
data = open(path, "rb").read()
i = data.find(struct.pack("<I", 0xFFFFDED3))
if i < 0 or i > 4096:
    sys.exit(f"{path}: no picobin block in the first 4 KB")
off, items = i + 4, {}
while off < i + 256:
    kind, size = data[off], data[off + 1]
    if kind == 0xFF:
        break
    items[kind] = data[off:off + 4 * size]
    off += 4 * size
image_type = struct.unpack_from("<H", items[0x42], 2)[0]
tbyb = bool(image_type & 0x8000)
minor, major = struct.unpack_from("<HH", items[0x48], 4) if 0x48 in items else (None, None)
x, y, z = (int(p) for p in version.split("."))
want = (x, y * 1000 + z)
print(f"{path}: version {major}.{minor} (want {want[0]}.{want[1]}), TBYB {tbyb}")
if (major, minor) != want:
    sys.exit("version in the image header does not match")
if mode == "--tbyb" and not tbyb:
    sys.exit("expected a TBYB image")
if mode == "--no-tbyb" and tbyb:
    sys.exit("a USB image must not be TBYB")
