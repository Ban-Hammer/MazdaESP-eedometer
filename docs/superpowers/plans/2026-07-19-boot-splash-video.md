# Boot Splash Video Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Play a ~2 s MJPEG splash (`mazdaspeed_splash.mp4`) from LittleFS at boot on the RP2040 CAN display, aborting early if the car is already moving, then hand off to the normal speed/gear display.

**Architecture:** An ffmpeg script transcodes the mp4 into 0-indexed baseline JPEG frames under `data/splash/`, uploaded to LittleFS with the fonts. A decoupled `splash::play()` module decodes each frame with TJpg_Decoder straight to the panel (no framebuffer), paced to ~15 fps, calling a per-frame `InterFrame` callback that polls `ota::service()` and aborts on CAN speed > 0. One call in `setup()` (after TFT+CAN+font init) wires it in; `ota::begin()` still runs first so update-mode-first holds.

**Tech Stack:** RP2040 (arduino-pico), TFT_eSPI (ST7789 240×240), TJpg_Decoder 1.1.0, LittleFS, ffmpeg (host), arduino-cli.

## Global Constraints

- Spec: `docs/superpowers/specs/2026-07-19-boot-splash-video-design.md`.
- **FQBN:** `rp2040:rp2040:adafruit_feather_can:flash=8388608_2097152` (the `_can` variant is mandatory — see CLAUDE.md).
- Panel is **240×240**; source clip is already 240×240 (no scale/crop). Rotation is `SCREEN_ROTATION` (currently 2); splash plays **after** `tft.setRotation()`.
- Frames live in **LittleFS** at `/splash/000.jpg …` (0-indexed, contiguous), uploaded via the `arduino-littlefs-upload` IDE step (same as fonts) — **not** delivered by OTA.
- **Bounded playback:** the frame loop stops at the first missing file and is hard-capped at `MAX_FRAMES = 300`; no unbounded waits. `splash::play()` must never hang `setup()` (preserves update-mode-first).
- **Graceful skip:** if `/splash/000.jpg` is absent, `play()` returns immediately.
- TJpg_Decoder needs no config edit (RP2040 auto-defines `TJPGD_LOAD_FFS`); use `setSwapBytes(true)` for correct colors with `tft.pushImage`.
- Host tooling present: `ffmpeg`, `ffprobe`, `arduino-cli` (sketchbook = `/home/honk/mazda/Arduino`).

---

## File Structure

```
mazdaspeed_splash.mp4                                  # source (exists, 240x240, 2s)
tools/build_splash.sh                                  # NEW: transcode mp4 -> data/splash/NNN.jpg
tools/check_splash.sh                                  # NEW: Tier-1 asset validation (host, runnable)
Arduino/can_receiver_display/data/splash/000.jpg …     # NEW generated assets (committed)
Arduino/can_receiver_display/splash.h                  # NEW: module interface
Arduino/can_receiver_display/splash.cpp                # NEW: module impl (TJpg_Decoder)
Arduino/can_receiver_display/can_receiver_display.ino  # MODIFY: include + callback + setup() call
Arduino/libraries/TJpg_Decoder/                        # NEW dependency (installed via arduino-cli)
```

---

## Task 1: Asset pipeline (transcode + validate)

**Files:**
- Create: `tools/build_splash.sh`, `tools/check_splash.sh`
- Generate: `Arduino/can_receiver_display/data/splash/000.jpg …`

**Interfaces:**
- Consumes: `mazdaspeed_splash.mp4`.
- Produces: 0-indexed baseline JPEG frames in `data/splash/` that `splash.cpp` (Task 2) loads as `/splash/NNN.jpg`.

- [ ] **Step 1: Write the validation script first (the test)**

`tools/check_splash.sh`:
```bash
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

# each 240x240 baseline mjpeg
for f in "$DIR"/*.jpg; do
  read -r w h codec < <(ffprobe -v error -select_streams v:0 \
    -show_entries stream=width,height,codec_name -of csv=p=0 "$f" | tr ',' ' ')
  { [ "$w" = "240" ] && [ "$h" = "240" ]; } || { echo "FAIL: $f is ${w}x${h}"; fail=1; }
  [ "$codec" = "mjpeg" ] || { echo "FAIL: $f codec=$codec (want baseline mjpeg)"; fail=1; }
done

total=$(du -sk "$DIR" | cut -f1)
[ "$total" -le "$MAX_KB" ] || { echo "FAIL: total ${total}KB > ${MAX_KB}KB"; fail=1; }

[ "$fail" = 0 ] && echo "OK: $n frames, ${total}KB, all 240x240 baseline JPEG"
exit "$fail"
```

- [ ] **Step 2: Run it to confirm it fails (no frames yet)**

Run: `chmod +x tools/check_splash.sh && tools/check_splash.sh`
Expected: `FAIL: no frames in …/data/splash` (exit 1).

- [ ] **Step 3: Write the transcode script**

`tools/build_splash.sh`:
```bash
#!/usr/bin/env bash
# Transcode the boot splash video into LittleFS JPEG frames.
# Usage: tools/build_splash.sh [fps] [quality]   (defaults: 15 fps, q=4)
set -euo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="$REPO/mazdaspeed_splash.mp4"
OUT="$REPO/Arduino/can_receiver_display/data/splash"
FPS="${1:-15}"
Q="${2:-4}"

[ -f "$SRC" ] || { echo "missing $SRC" >&2; exit 1; }
rm -rf "$OUT"; mkdir -p "$OUT"
ffmpeg -v error -i "$SRC" -vf "fps=$FPS" -q:v "$Q" -start_number 0 "$OUT/%03d.jpg"

n=$(ls "$OUT"/*.jpg 2>/dev/null | wc -l)
total=$(du -sk "$OUT" | cut -f1)
echo "wrote $n frames to $OUT (${total}KB, fps=$FPS q=$Q)"
```

- [ ] **Step 4: Generate the frames**

Run: `chmod +x tools/build_splash.sh && tools/build_splash.sh`
Expected: `wrote 30 frames to …/data/splash (≈188KB, fps=15 q=4)` (frame count ~24–30).

- [ ] **Step 5: Run the validation — now it passes**

Run: `tools/check_splash.sh`
Expected: `OK: 30 frames, ≈188KB, all 240x240 baseline JPEG` (exit 0).

- [ ] **Step 6: Commit**

```bash
git add tools/build_splash.sh tools/check_splash.sh Arduino/can_receiver_display/data/splash
git commit -m "feat(splash): asset pipeline — transcode mp4 to LittleFS JPEG frames"
```

---

## Task 2: Splash playback module

**Files:**
- Create: `Arduino/can_receiver_display/splash.h`, `Arduino/can_receiver_display/splash.cpp`
- Dependency: `Arduino/libraries/TJpg_Decoder` (install if missing)

**Interfaces:**
- Consumes: `TFT_eSPI`, `TJpg_Decoder`, `LittleFS`, the `/splash/NNN.jpg` frames (Task 1).
- Produces:
  - `namespace splash { typedef bool (*InterFrame)(); void play(TFT_eSPI& tft, uint8_t fps, InterFrame between); }`

- [ ] **Step 1: Ensure the TJpg_Decoder library is installed**

Run: `arduino-cli lib list 2>/dev/null | grep -i tjpg || arduino-cli lib install "TJpg_Decoder"`
Expected: `TJpg_Decoder 1.1.0` present under `Arduino/libraries/TJpg_Decoder`.

- [ ] **Step 2: Write the module interface**

`Arduino/can_receiver_display/splash.h`:
```cpp
#pragma once
#include <stdint.h>

class TFT_eSPI;   // forward declaration (used by reference only)

namespace splash {
  // Return true to abort playback early (checked once per frame).
  typedef bool (*InterFrame)();

  // Play /splash/000.jpg, 001.jpg, … from LittleFS at ~fps, calling `between` once per
  // frame. No-op if /splash/000.jpg is absent. Bounded (stops at first gap, capped at
  // MAX_FRAMES). Leaves the screen black on return.
  void play(TFT_eSPI& tft, uint8_t fps, InterFrame between);
}
```

- [ ] **Step 3: Write the module implementation**

`Arduino/can_receiver_display/splash.cpp`:
```cpp
#include "splash.h"
#include <Arduino.h>
#include <TFT_eSPI.h>
#include <TJpg_Decoder.h>
#include <FS.h>
#include <LittleFS.h>

namespace {
  TFT_eSPI* g_tft = nullptr;
  const int MAX_FRAMES = 300;

  // TJpg_Decoder output callback: push one decoded MCU block to the panel.
  bool tftOutput(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bitmap) {
    if (!g_tft) return false;
    if (y >= g_tft->height()) return false;   // block off-screen: stop this frame
    g_tft->pushImage(x, y, w, h, bitmap);
    return true;                              // keep decoding
  }
}

namespace splash {

void play(TFT_eSPI& tft, uint8_t fps, InterFrame between) {
  if (!LittleFS.exists("/splash/000.jpg")) return;   // nothing to play

  g_tft = &tft;
  TJpgDec.setJpgScale(1);
  TJpgDec.setSwapBytes(true);          // correct colour order for TFT_eSPI pushImage
  TJpgDec.setCallback(tftOutput);

  const uint32_t frameInterval = (fps > 0) ? (1000u / fps) : 0;
  char path[24];
  for (int i = 0; i < MAX_FRAMES; ++i) {
    snprintf(path, sizeof(path), "/splash/%03d.jpg", i);
    if (!LittleFS.exists(path)) break;               // past the last frame

    uint32_t t0 = millis();
    TJpgDec.drawFsJpg(0, 0, path, LittleFS);         // a bad frame just draws nothing

    if (between && between()) break;                  // abort (moving, or OTA took over)

    uint32_t elapsed = millis() - t0;                 // pace to target fps
    if (elapsed < frameInterval) delay(frameInterval - elapsed);
  }

  tft.fillScreen(TFT_BLACK);   // clean slate for the normal display
  g_tft = nullptr;
}

} // namespace splash
```

- [ ] **Step 4: Compile the sketch (module compiles even before it's called)**

Run: `arduino-cli compile --fqbn rp2040:rp2040:adafruit_feather_can:flash=8388608_2097152 Arduino/can_receiver_display`
Expected: `Sketch uses … bytes`. If it fails on `#include <SD.h>` (pulled in by `TJPGD_LOAD_SD_LIBRARY`), comment out `#define TJPGD_LOAD_SD_LIBRARY` in `Arduino/libraries/TJpg_Decoder/src/User_Config.h` and recompile.

- [ ] **Step 5: Commit**

```bash
git add Arduino/can_receiver_display/splash.h Arduino/can_receiver_display/splash.cpp Arduino/libraries/TJpg_Decoder
git commit -m "feat(splash): TJpg_Decoder playback module (splash::play)"
```

---

## Task 3: Wire the splash into boot

**Files:**
- Modify: `Arduino/can_receiver_display/can_receiver_display.ino`

**Interfaces:**
- Consumes: `splash::play`, `splash::InterFrame` (Task 2); existing `tft`, `CAN0`, `rxId/len/rxBuf`, `CAN0_INT`, `ota::service`, `otaUi`, `BL_PIN`.
- Produces: the boot-time splash with OTA-polling + speed-abort.

- [ ] **Step 1: Include the module**

At the top of `can_receiver_display.ino`, after `#include "ota_update.h"`, add:
```cpp
#include "splash.h"
```

- [ ] **Step 2: Add the interframe callback**

Immediately after the `otaUi(...)` function definition, add:
```cpp
// Called once per splash frame: keep the board updatable, and abort the splash the
// moment real CAN data shows the car is moving.
static bool splashInterFrame() {
  ota::service(otaUi);                     // an OTA mid-splash takes over + reboots
  if (!digitalRead(CAN0_INT)) {            // non-blocking CAN peek
    CAN0.readMsgBuf(&rxId, &len, rxBuf);
    if (rxId == 0x202) {
      unsigned int raw = (rxBuf[2] << 8) | rxBuf[3];
      if ((raw + 50) / 100 > 0) return true;   // moving -> abort
    }
  }
  return false;
}
```

- [ ] **Step 3: Play the splash at the end of `setup()`**

At the very end of `setup()`, after the `dm.setTextColor(...)` line and before the closing `}`, add:
```cpp
  analogWrite(BL_PIN, 255);                // backlight on so the splash is visible
  splash::play(tft, 15, splashInterFrame); // ~2s MJPEG boot splash (aborts if moving)
```
(The backlight PWM is otherwise first driven in `loop()`'s `updateBacklight()`, which
runs only after the splash — so it must be turned on here or the splash renders dark.)

- [ ] **Step 4: Compile**

Run: `arduino-cli compile --fqbn rp2040:rp2040:adafruit_feather_can:flash=8388608_2097152 Arduino/can_receiver_display`
Expected: compiles clean; sketch size grows modestly over the pre-splash build.

- [ ] **Step 5: Commit**

```bash
git add Arduino/can_receiver_display/can_receiver_display.ino
git commit -m "feat(splash): play boot splash in setup() with OTA-poll + speed-abort"
```

---

## Task 4: On-hardware bench verification

**Files:** none (verification; small fixes only if a check fails).

**Interfaces:**
- Consumes: the full assembly + CAN replay bench.
- Produces: evidence the splash plays, aborts, skips, and doesn't regress CAN/OTA.

- [ ] **Step 1: Flash sketch + upload LittleFS (fonts + splash)**

```bash
arduino-cli upload -p /dev/ttyACM0 \
  --fqbn rp2040:rp2040:adafruit_feather_can:flash=8388608_2097152 Arduino/can_receiver_display
```
Then upload LittleFS via the Arduino IDE (`Ctrl+Shift+P` → "Upload LittleFS to Pico/RP2040…") so `data/splash/` reaches the device. Serial should print `All fonts are there!`.

- [ ] **Step 2: Full playback (CAN idle / speed 0)**

Power on with CAN replay stopped or replaying speed 0.
Expected: the splash plays start-to-finish (~2 s) at a smooth rate, then the screen clears and the normal display appears.

- [ ] **Step 3: Abort-on-motion**

Power on (or reset) with CAN replaying a speed > 0 frame stream.
Expected: the splash cuts out within a frame or two and the speedo appears immediately.

- [ ] **Step 4: Graceful skip (missing frames)**

Temporarily rename `/splash` on the device (re-upload LittleFS without the `data/splash` dir, or test before Step 1's LittleFS upload).
Expected: board boots straight to the normal display, no hang, OTA still reachable.

- [ ] **Step 5: OTA-during-splash**

While the splash is playing (Step 2 conditions), push `build/can_receiver_display.ino.bin` from the XIAO page.
Expected: the update runs to completion and the board reboots into the new version — proving `ota::service()` is polled during the splash.

- [ ] **Step 6: Decode-rate + tearing check**

Observe playback smoothness. Optionally add a temporary `Serial.printf` of per-frame `millis()` delta in `splash.cpp` to measure the real rate.
Expected: ~15 fps. If visibly choppy, re-run `tools/build_splash.sh 12` (12 fps) and re-upload LittleFS. If frame tearing is objectionable, note it for the sprite-buffer fallback (decode into a 240×240 `TFT_eSprite`, push whole frames).

- [ ] **Step 7: Regression**

With CAN replay running normally: confirm speed/gear/brightness/sport all render as before, and that an OTA still works from the normal loop.
Expected: no regression.

- [ ] **Step 8: Record results (and any fps/tearing fix)**

```bash
# if build_splash fps was changed or frames re-generated:
git add -A
git commit -m "test(splash): bench-verified; <note fps/tearing outcome>"
```

---

## Self-Review

**Spec coverage:** Asset pipeline (MJPEG, 240×240, LittleFS) → Task 1. TJpg_Decoder decode path + decoupled `splash::play()` + bounded loop + missing-frame skip → Task 2. Boot ordering (after TFT/CAN/fonts, update-mode-first), interframe callback (OTA-poll + speed>0 abort), backlight-on → Task 3. Full/abort/skip/OTA-during-splash/decode-rate/regression tests → Task 4. Risks (decode rate → 12 fps; tearing → sprite buffer) → Task 4 Step 6. **All spec sections map to tasks.**

**Placeholder scan:** No TBD/"handle errors"/"similar to Task N". The SD.h compile note (Task 2 Step 4) is a concrete conditional fix, not a placeholder.

**Type/name consistency:** `splash::play` / `splash::InterFrame` defined in Task 2 (`splash.h`) and used identically in Task 3. `tftOutput` matches TJpg's `SketchCallback` signature `(int16_t,int16_t,uint16_t,uint16_t,uint16_t*)`. Frame path `/splash/%03d.jpg` (0-indexed) matches the `-start_number 0` transcode in Task 1. FQBN identical across Tasks 2–4. `splashInterFrame` uses only globals that already exist in the sketch (`CAN0`, `rxId`, `len`, `rxBuf`, `CAN0_INT`, `ota::service`, `otaUi`).
