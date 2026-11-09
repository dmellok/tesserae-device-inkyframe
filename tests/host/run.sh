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
MBEDTLS_TAG=v3.6.2                                        # the Pico SDK's copy
MBEDTLS=.deps/mbedtls
if [ ! -f "$MBEDTLS/library/gcm.c" ]; then
    rm -rf "$MBEDTLS"
    git clone -q --depth 1 --branch "$MBEDTLS_TAG" https://github.com/Mbed-TLS/mbedtls.git "$MBEDTLS"
fi
SRC=../../src
cc -std=gnu11 -O1 -Wall -Wextra -DOTA_TRUST_TEST_KEY=1 \
   -I"$SRC" -I"$DEPS/src" \
   test_ota.c "$SRC/ota_verify.c" "$SRC/sha256.c" "$SRC/vendor/cJSON.c" \
   "$DEPS/src/monocypher.c" "$DEPS/src/optional/monocypher-ed25519.c" \
   -o .deps/test_ota
.deps/test_ota

MB_SRC="$MBEDTLS/library"
cc -std=gnu11 -O1 -Wall -Wextra -I"$SRC" -I"$DEPS/src" -I"$MBEDTLS/include" -I. \
   -DMBEDTLS_CONFIG_FILE='"host_mbedtls_config.h"' \
   test_relay.c "$SRC/relay_crypto.c" "$DEPS/src/monocypher.c" \
   "$MB_SRC/gcm.c" "$MB_SRC/aes.c" "$MB_SRC/cipher.c" "$MB_SRC/cipher_wrap.c" "$MB_SRC/hkdf.c" \
   "$MB_SRC/md.c" "$MB_SRC/sha256.c" "$MB_SRC/platform_util.c" "$MB_SRC/constant_time.c" \
   "$MB_SRC/platform.c" "$MB_SRC/aesni.c" "$MB_SRC/aesce.c" \
   -o .deps/test_relay
.deps/test_relay

# Deck and album logic, ported with their tests from tesserae-device-firmware.
cc -std=gnu11 -O1 -Wall -Wextra -I"$SRC" -I"$SRC/vendor" test_deck.c "$SRC/deck.c" "$SRC/vendor/cJSON.c" -o .deps/test_deck
.deps/test_deck
cc -std=gnu11 -O1 -Wall -Wextra -I"$SRC" -I"$SRC/vendor" test_frame_collection.c "$SRC/frame_collection.c" "$SRC/vendor/cJSON.c" -o .deps/test_frame_collection
.deps/test_frame_collection

# The log ring the log uploads and OTA logs copy from (Pico SDK stubbed).
cc -std=gnu11 -O1 -Wall -Wextra -Istubs -I"$SRC" test_log.c "$SRC/log.c" -o .deps/test_log
.deps/test_log > /dev/null   # stdout is the logged lines; results are on stderr
