#!/usr/bin/env bash
# Build the sketch and merge bootloader + partition table + boot_app0 + app into
# one image that can be written to offset 0x0 on a 16 MB T-Display S3.
#
# Usage: ./tools/make_release.sh
#        CLI=/path/to/arduino-cli ESPTOOL=/path/to/esptool ./tools/make_release.sh
set -euo pipefail
cd "$(dirname "$0")/.."

CLI="${CLI:-../lilygo-pokedex/tools/arduino-cli}"
FQBN="${FQBN:-esp32:esp32:esp32s3:CDCOnBoot=cdc,FlashSize=16M,PSRAM=disabled,PartitionScheme=rainmaker_4MB}"
BUILD="${BUILD:-build/release}"
OUT="${OUT:-releases/pokedex-gen5-full.bin}"

if [ ! -x "$CLI" ] && ! command -v "$CLI" >/dev/null 2>&1; then
  echo "arduino-cli not found: $CLI" >&2
  exit 1
fi

ESPTOOL="${ESPTOOL:-$(command -v esptool.py || command -v esptool || true)}"
if [ -z "$ESPTOOL" ]; then
  ESPTOOL="$(ls -1 "$HOME"/.arduino15/packages/esp32/tools/esptool_py/*/esptool 2>/dev/null | tail -1)"
fi
if [ -z "$ESPTOOL" ] || [ ! -x "$ESPTOOL" ]; then
  echo "esptool not found; set ESPTOOL=/path/to/esptool" >&2
  exit 1
fi

BOOTAPP="$(ls -1 "$HOME"/.arduino15/packages/esp32/hardware/esp32/*/tools/partitions/boot_app0.bin 2>/dev/null | tail -1)"
if [ -z "$BOOTAPP" ]; then
  echo "boot_app0.bin not found in the ESP32 Arduino package" >&2
  exit 1
fi

rm -rf "$BUILD"
mkdir -p "$BUILD" "$(dirname "$OUT")"
"$CLI" compile --fqbn "$FQBN" --output-dir "$BUILD" pokedex

# esptool 5 calls it merge-bin and warns about merge_bin; older versions only
# know merge_bin. Pick the one this install has.
MERGE=merge-bin
if ! "$ESPTOOL" "$MERGE" --help >/dev/null 2>&1; then MERGE=merge_bin; fi

"$ESPTOOL" --chip esp32s3 "$MERGE" -o "$OUT" \
  --flash-mode dio --flash-freq 80m --flash-size 16MB \
  0x0 "$BUILD/pokedex.ino.bootloader.bin" \
  0x8000 "$BUILD/pokedex.ino.partitions.bin" \
  0xe000 "$BOOTAPP" \
  0x10000 "$BUILD/pokedex.ino.bin"

ls -l "$OUT"
echo "sha256: $(sha256sum "$OUT" | cut -d' ' -f1)"
