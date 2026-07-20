#!/usr/bin/env bash
# Push the splash frames to the in-car display over WiFi, via the XIAO OTA bridge.
# No USB needed. Connect THIS machine to the XIAO SoftAP (MAZDASPEED) first.
#
# Usage: tools/push_splash.sh [host]     (default host: 192.168.4.1)
#
# Uploads every data/splash/NNN.jpg into /splash on the RP2040's LittleFS, then
# deletes any stale higher-numbered frames (so a shorter clip doesn't leave old
# frames that would play after the new ones).
set -euo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DIR="$REPO/Arduino/can_receiver_display/data/splash"
HOST="${1:-192.168.4.1}"
BASE="http://$HOST"

frames=("$DIR"/*.jpg)
[ -e "${frames[0]}" ] || { echo "no frames in $DIR — run tools/build_splash.sh first" >&2; exit 1; }
n=${#frames[@]}

echo "pushing $n frames to $BASE (/splash) ..."
for f in "${frames[@]}"; do
  name=$(basename "$f")
  resp=$(curl -sS -m 30 -F "f=@$f" "$BASE/api/upload?path=/splash") || { echo "curl failed on $name — is this machine on the MAZDASPEED AP?" >&2; exit 1; }
  echo "  $name -> $resp"
  case "$resp" in OK*) ;; *) echo "FAILED on $name" >&2; exit 1;; esac
done

# Best-effort cleanup: delete leftover frames at indices >= n (ignore not-found).
echo "clearing stale frames >= $n ..."
for ((i=n; i<n+50; i++)); do
  p=$(printf "/splash/%03d.jpg" "$i")
  curl -sS -m 10 -X POST "$BASE/api/delete?path=$p" >/dev/null 2>&1 || true
done

echo "done. Reboot the display (cycle ignition) to see the new splash."
