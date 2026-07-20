#!/usr/bin/env bash
# Compile the RP2040 CAN display sketch to an OTA-ready .bin.
#
# Usage:
#   tools/build_ota.sh              # build with the current FW_VERSION
#   tools/build_ota.sh 2026.07.19-x # stamp FW_VERSION first, then build
#
# Output: build/can_receiver_display.ino.bin  (upload this via the XIAO page)
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SKETCH_DIR="$REPO/Arduino/can_receiver_display"
INO="$SKETCH_DIR/can_receiver_display.ino"
# Board is the Adafruit Feather RP2040 CAN (integrated MCP2515 on spi1 / pins
# 14/15/8) — the _can variant is required; the generic adafruit_feather maps SPI
# to the wrong pins and CAN init fails. flash= must match the board (2MB FS).
# freq=250 (overclock) is compiled in for boot-splash JPEG decode headroom; keep
# it identical here and on the USB flash or an OTA update reverts the clock.
FQBN="rp2040:rp2040:adafruit_feather_can:flash=8388608_2097152,freq=250"
OUT="$REPO/build"
BIN="$OUT/can_receiver_display.ino.bin"

# Optional: stamp a new FW_VERSION into the sketch before building.
if [ "${1:-}" != "" ]; then
  sed -i -E "s/#define FW_VERSION \"[^\"]*\"/#define FW_VERSION \"$1\"/" "$INO"
  echo "FW_VERSION -> $1"
fi

ver="$(grep -m1 '#define FW_VERSION' "$INO" | sed -E 's/.*"([^"]*)".*/\1/')"
echo "Building can_receiver_display  (FW_VERSION=$ver)"
echo "FQBN: $FQBN"

mkdir -p "$OUT"
arduino-cli compile --fqbn "$FQBN" --output-dir "$OUT" "$SKETCH_DIR"

if [ ! -f "$BIN" ]; then
  echo "ERROR: expected image not found at $BIN" >&2
  exit 1
fi

size="$(stat -c%s "$BIN")"
echo
echo "OTA image ready:"
echo "  $BIN"
echo "  $((size / 1024)) KB ($size bytes), version $ver"
