#!/usr/bin/env bash
# Transcode the boot splash video into LittleFS JPEG frames.
# Usage: tools/build_splash.sh [fps] [quality]   (defaults: 15 fps, q=4)
set -euo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="$REPO/splash2.mp4"
OUT="$REPO/Arduino/can_receiver_display/data/splash"
FPS="${1:-15}"
Q="${2:-4}"

[ -f "$SRC" ] || { echo "missing $SRC" >&2; exit 1; }
rm -rf "$OUT"; mkdir -p "$OUT"
ffmpeg -v error -i "$SRC" -vf "fps=$FPS" -q:v "$Q" -start_number 0 "$OUT/%03d.jpg"

n=$(ls "$OUT"/*.jpg 2>/dev/null | wc -l)
total=$(du -sk "$OUT" | cut -f1)
echo "wrote $n frames to $OUT (${total}KB, fps=$FPS q=$Q)"
