#!/usr/bin/env bash
# Build and run the host tests for the OTA verifier and SHA-256. Fetches the
# same pinned Monocypher the firmware and the ESP32 firmware use.
set -euo pipefail
cd "$(dirname "$0")"
MONO_COMMIT=ab2b16dd619ad5f6979a4fbe69cfa324a6fcc35f   # Monocypher 4.0.3
DEPS=.deps/Monocypher
if [ ! -f "$DEPS/src/monocypher.c" ]; then
    rm -rf "$DEPS"
    git clone -q https://github.com/LoupVaillant/Monocypher.git "$DEPS"
    git -C "$DEPS" checkout -q "$MONO_COMMIT"
fi
SRC=../../src
cc -std=gnu11 -O1 -Wall -Wextra -DOTA_TRUST_TEST_KEY=1 \
   -I"$SRC" -I"$DEPS/src" \
   test_ota.c "$SRC/ota_verify.c" "$SRC/sha256.c" "$SRC/vendor/cJSON.c" \
   "$DEPS/src/monocypher.c" "$DEPS/src/optional/monocypher-ed25519.c" \
   -o .deps/test_ota
.deps/test_ota
