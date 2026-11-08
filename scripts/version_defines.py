# PRE build script: stamp FW_VERSION (include/version.h) into the RP2350 image
# header, as PICO_CRT0_VERSION_MAJOR / _MINOR. The boot ROM boots the A/B
# partition with the higher version, so an installed update wins over the
# image it replaced. "x.y.z" -> major x, minor y*1000+z (both 16 bits).
Import("env")
import re
from os.path import join

text = open(join(env["PROJECT_INCLUDE_DIR"], "version.h")).read()
m = re.search(r'FW_VERSION\s+"(\d+)\.(\d+)\.(\d+)"', text)
if not m:
    raise SystemExit("version.h: FW_VERSION must be \"x.y.z\"")
major, minor, patch = (int(g) for g in m.groups())
if major > 0xFFFF or minor > 64 or patch > 999:
    raise SystemExit("FW_VERSION out of range for the image header (minor <= 64, patch <= 999)")
env.Append(CPPDEFINES=[("PICO_CRT0_VERSION_MAJOR", major), ("PICO_CRT0_VERSION_MINOR", minor * 1000 + patch)])
