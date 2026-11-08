#!/usr/bin/env bash
# One-time: give a Pico 2 W Inky Frame the A/B partition table that
# over-the-air updates need, then load the firmware into slot A.
#
#   tools/install_partitions.sh [firmware.uf2]
#
# Run with the frame on USB (in the running firmware, or already in BOOTSEL).
# Wi-Fi, server and pairing live in the last 16 KB of flash, outside both
# partitions, and survive. Anything else on the Pico (MicroPython and its
# files) does not. Re-running it is harmless.
set -euo pipefail
cd "$(dirname "$0")/.."
PICOTOOL=${PICOTOOL:-$(ls -d ~/.platformio/packages/tool-picotool-rp2040-earlephilhower*/picotool | head -1)}
FW=${1:-.pio/build/inkyframe2_spectra/firmware.uf2}
[ -f "$FW" ] || { echo "no $FW: build it first (pio run -e inkyframe2_spectra)"; exit 1; }

"$PICOTOOL" partition create partitions/pt.json partitions/pt.uf2
echo "==> into BOOTSEL"
"$PICOTOOL" reboot -f -u >/dev/null 2>&1 || true
sleep 4
echo "==> writing the partition table"
"$PICOTOOL" load partitions/pt.uf2
"$PICOTOOL" reboot -u        # the boot ROM reads the table at reset
sleep 4
"$PICOTOOL" partition info
echo "==> loading $FW into partition A"
"$PICOTOOL" load "$FW" -p 0
"$PICOTOOL" reboot
echo "done: the frame boots from partition A and can now take updates"
