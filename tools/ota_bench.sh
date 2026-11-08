#!/usr/bin/env bash
# Bench OTA: build an update image at VERSION from ENV, sign it with the
# Tesserae TEST key, and write it plus descriptor.json into OUT, for a bench
# build (env bench2_spectra*) to fetch from http://<this host>:8000/.
#   tools/ota_bench.sh <version> <pio env> <out dir> <tesserae repo> <host:port>
set -euo pipefail
VERSION=$1 ENV=$2 OUT=$3 TESSERAE=$4 HOSTPORT=$5
cd "$(dirname "$0")/.."
orig=$(cat include/version.h)
trap 'printf "%s\n" "$orig" > include/version.h' EXIT
sed -i '' "s/#define FW_VERSION \".*\"/#define FW_VERSION \"$VERSION\"/" include/version.h
~/.platformio/penv/bin/pio run -e "$ENV" >/dev/null
mkdir -p "$OUT"
cp ".pio/build/$ENV/firmware.bin" "$OUT/fw-$VERSION.bin"
( cd "$TESSERAE" && uv run python -m app.ota.sign --key-id test-ed25519-1 \
    --device-kind pimoroni_inky_frame_73 --fw-version "$VERSION" \
    --image "$OUT/fw-$VERSION.bin" --image-url "http://$HOSTPORT/fw-$VERSION.bin" \
    --key tests/fixtures/ota/test_signing_key.hex ) > "$OUT/descriptor.json"
echo "built and signed $VERSION ($(wc -c < "$OUT/fw-$VERSION.bin") bytes) -> $OUT"
