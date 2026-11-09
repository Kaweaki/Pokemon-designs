#!/usr/bin/env bash
# Build and flash the pokedex sketch. The RainMaker build advertises PROV_POKEDEX
# for BLE provisioning; see README.md for the app-side controls.
#
# Usage: ./build.sh [compile|upload|monitor]     PORT=/dev/ttyACM1 to switch board
set -euo pipefail
cd "$(dirname "$0")"

CLI="${CLI:-../lilygo-pokedex/tools/arduino-cli}"
# PSRAM disabled on purpose: the two boards here have different PSRAM types and
# PSRAM=opi hangs the quad one before the app runs. Nothing here needs it.
FQBN="esp32:esp32:esp32s3:CDCOnBoot=cdc,FlashSize=16M,PSRAM=disabled,PartitionScheme=rainmaker_4MB"
PORT="${PORT:-/dev/ttyACM0}"
action="${1:-compile}"

case "$action" in
  compile) "$CLI" compile --fqbn "$FQBN" --warnings default pokedex ;;
  upload)  "$CLI" compile --fqbn "$FQBN" pokedex && "$CLI" upload -p "$PORT" --fqbn "$FQBN" pokedex ;;
  monitor) "$CLI" monitor -p "$PORT" --config baudrate=115200 ;;
  *) echo "usage: $0 [compile|upload|monitor]" >&2; exit 2 ;;
esac
