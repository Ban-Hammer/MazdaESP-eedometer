#!/usr/bin/env bash
# Validate generated splash frames: contiguous 0-indexed 240x240 baseline JPEGs,
# under the FS budget. Non-zero exit on any failure.
set -euo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DIR="$REPO/Arduino/can_receiver_display/data/splash"
MAX_KB=400
fail=0

n=$(ls "$DIR"/*.jpg 2>/dev/null | wc -l)
[ "$n" -ge 1 ] || { echo "FAIL: no frames in $DIR"; exit 1; }

# contiguous 0-indexed 000..(n-1)
for ((i=0; i<n; i++)); do
  f=$(printf "%s/%03d.jpg" "$DIR" "$i")
  [ -f "$f" ] || { echo "FAIL: missing frame $f"; fail=1; }
done

# each 240x240 baseline mjpeg (parse by key — ffprobe csv field order is not fixed)
for f in "$DIR"/*.jpg; do
  info=$(ffprobe -v error -select_streams v:0 \
    -show_entries stream=width,height,codec_name -of default=noprint_wrappers=1 "$f")
  w=$(printf '%s\n' "$info" | sed -n 's/^width=//p')
  h=$(printf '%s\n' "$info" | sed -n 's/^height=//p')
  codec=$(printf '%s\n' "$info" | sed -n 's/^codec_name=//p')
  { [ "$w" = "240" ] && [ "$h" = "240" ]; } || { echo "FAIL: $f is ${w}x${h}"; fail=1; }
  [ "$codec" = "mjpeg" ] || { echo "FAIL: $f codec=$codec (want baseline mjpeg)"; fail=1; }
done

total=$(du -sk "$DIR" | cut -f1)
[ "$total" -le "$MAX_KB" ] || { echo "FAIL: total ${total}KB > ${MAX_KB}KB"; fail=1; }

[ "$fail" = 0 ] && echo "OK: $n frames, ${total}KB, all 240x240 baseline JPEG"
exit "$fail"
