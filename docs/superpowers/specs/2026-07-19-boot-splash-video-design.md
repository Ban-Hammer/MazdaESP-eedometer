# Boot Splash Video — Design

**Date:** 2026-07-19
**Status:** Approved design, pre-implementation
**Project:** `Arduino/can_receiver_display` (Mazda MX-5 ND CAN display)

## Problem / Goal

Play a short branded **video splash at boot** on the ST7789 240×240 panel, then hand
off to the normal speed/gear display. The source clip is
`mazdaspeed_splash.mp4` (240×240, H.264, 60 fps, 2.0 s, no audio).

The splash also doubles as a **boot-alive indicator**: since the display otherwise shows
nothing until the first CAN frame (the "Waiting CAN…" screen was removed), a blank panel
at power-on — especially right after an OTA reboot — looks like a brick. The splash fills
that gap and visibly confirms the board came back.

## Feasibility (why the RP2040 handles this)

- **Display bandwidth:** a full 240×240×16-bit frame is 112.5 KB, pushed over PIO SPI
  (~62.5 MHz) in ~15 ms → the panel supports ~50–60 fps. Not the bottleneck.
- **CPU/RAM:** 133 MHz dual-core, 264 KB RAM — enough to JPEG-decode and block-draw with
  no full framebuffer.
- **Storage is the real budget:** frames live in LittleFS (2 MB partition, ~133 KB used by
  fonts). MJPEG at 15 fps is ~190 KB total — trivial next to ~1.8 MB free.

## Decisions (locked)

- **Content:** the real clip, already 240×240 → **no crop/letterbox/stretch**, 1:1 to panel.
- **Encoding:** **MJPEG** — one baseline JPEG per frame, decoded with **TJpg_Decoder**
  (pairs with TFT_eSPI). ~15 fps, high quality, ~30 frames, ~190 KB.
- **Duration:** play the clip **as-is (~2 s)** at natural speed, then straight to the speedo.
- **Boot behavior:** play on boot, but **abort early if CAN speed > 0** (car moving) so
  driving off is never delayed. Idling at startup → full ~2 s plays.
- **Storage:** frames in **LittleFS** (`/splash/NNN.jpg`), uploaded with the fonts; they
  survive OTA and don't bloat the OTA image.
- **Draw path:** direct-to-panel (no framebuffer) first; sprite-buffer fallback only if
  tearing is objectionable.

## Non-Goals

- Audio (no hardware).
- Video longer than the source clip, slow-motion stretching, or looping.
- Reaching a hard 3 s (source is 2 s; play as-is was chosen).
- Runtime-configurable / OTA-swappable splash content beyond re-running the LittleFS upload.

## Architecture

Two new pieces plus a small `setup()` hook; kept decoupled from CAN/OTA via a callback.

- **Asset pipeline** — `tools/build_splash.sh` transcodes the mp4 → JPEG frames into
  `Arduino/can_receiver_display/data/splash/`. Rerunnable.
- **`splash` module** (`splash.h` / `splash.cpp`) — plays the frames from LittleFS,
  decoupled from the rest of the sketch.
- **`.ino` integration** — a `splashInterFrame()` callback (owns the CAN/OTA coupling) and
  one `splash::play(...)` call in `setup()`.

### Module interface

```cpp
namespace splash {
  typedef bool (*InterFrame)();   // return true to abort playback early
  // Play /splash/000.jpg, 001.jpg, … from LittleFS at ~fps, calling `between` once per
  // frame. No-op if the first frame is missing. Leaves the screen black on return.
  void play(TFT_eSPI& tft, uint8_t fps, InterFrame between);
}
```

- `splash.cpp` configures TJpg_Decoder with an output callback that does
  `tft.pushImage(...)` per decoded block — **no full framebuffer**.
- It iterates frames **from `/splash/000.jpg` upward until the next file is absent**
  (no hardcoded count — robust to re-transcodes), `drawFsJpg(0,0,path, LittleFS)` each,
  paced with `millis()` to the target fps (if decode is slower, playback runs at decode
  speed).
- **Strictly bounded:** the frame loop is capped at a hard `MAX_FRAMES` (e.g. 300) and has
  no unbounded waits — so it can never hang `setup()`. This preserves update-mode-first
  (the board always reaches `loop()`).
- On return (finished or aborted) it leaves the screen black for a clean redraw.

### `.ino` integration

```cpp
// in setup(), AFTER tft.init()+setRotation, CAN init, and font loading:
splash::play(tft, 15, splashInterFrame);

// callback — owns CAN/OTA coupling, called once per frame:
static bool splashInterFrame() {
  ota::service(otaUi);                 // stay updatable during the splash
  if (!digitalRead(CAN0_INT)) {        // non-blocking CAN peek
    CAN0.readMsgBuf(&rxId, &len, rxBuf);
    if (rxId == 0x202) {
      unsigned int raw = (rxBuf[2] << 8) | rxBuf[3];
      if ((raw + 50) / 100 > 0) return true;   // moving → abort
    }
  }
  return false;
}
```

`ota::begin()` still runs first in `setup()` (update-mode-first); the splash stays
updatable because the callback polls `ota::service()` every frame. An OTA that arrives
mid-splash runs its full session and reboots, naturally interrupting the splash.

### Boot ordering (setup)

1. `ota::begin(FW_VERSION)` — update listener up first.
2. `Serial`, LittleFS, font check.
3. CAN init (`CAN0.begin` + filters + `MCP_NORMAL`).
4. `tft.init()`, `tft.setRotation(SCREEN_ROTATION)`, backlight PWM, sprites/fonts.
5. `splash::play(tft, 15, splashInterFrame)` — plays (or aborts), leaves screen black.
6. `loop()` — normal CAN-driven display.

The splash runs after `setRotation`, so frames render in the same orientation as the
speedo.

## Asset Pipeline

`tools/build_splash.sh`:
- Input: `mazdaspeed_splash.mp4`. Output: `Arduino/can_receiver_display/data/splash/NNN.jpg`.
- `ffmpeg -i … -vf fps=15 -q:v 4 -start_number 0 data/splash/%03d.jpg` — baseline
  MJPEG, **0-indexed** (`000.jpg`…) to match the module's `/splash/NNN.jpg` loop.
- Clears the target dir first so stale frames don't linger; prints frame count + total size.
- Frames reach the device via the existing **`arduino-littlefs-upload`** step (same as
  fonts — see CLAUDE.md); OTA does not deliver them.

Measured baseline (15 fps, high quality): ~30 frames, ~188 KB. Budget ceiling for the
pipeline check: < ~400 KB.

## Edge Cases

| Case | Behavior |
|---|---|
| Frames not uploaded to LittleFS | `play()` no-ops (first-frame existence check); boot continues |
| Corrupt/undecodable frame | TJpg error → skip frame, continue |
| Decode slower than target fps | Runs at decode speed; bounded by fixed frame count |
| Already moving at boot (speed > 0) | Aborts on frame 0 → near-instant speedo (covers OTA-reboot-while-driving) |
| OTA arrives mid-splash | `ota::service()` takes over, updates, reboots |
| Leftover splash pixels | `play()` ends with `fillScreen(BLACK)`; loop redraws |

**Critical safety rule:** `splash::play()` must be strictly bounded so it can never hang
`setup()` — otherwise the board never reaches `loop()` and the OTA listener stops being
polled (lockout). Bounded playback + missing-frame skip protect boot and update-mode-first.

## Risks & Fallbacks

- **Decode rate** (15 fps ≈ 67 ms/frame) is realistic but **unverified on hardware**. If
  choppy, drop to 12 fps or reduce JPEG complexity. Measured in testing.
- **Tearing:** direct-to-panel draw has no double-buffer. If objectionable, decode into a
  240×240 sprite (112 KB, fits in 264 KB RAM) and push whole frames (tear-free). Start
  simple; upgrade only if needed.

## Testing Strategy

Bench: real cluster + ESP32 CAN replay (SavvyCAN), so "display still works after" is
verifiable.

### Tier 1 — asset pipeline (host, no hardware)
Assert `build_splash.sh` output: ~30 frames, each **240×240 baseline JPEG**, sequentially
named, total under ~400 KB (via `ffprobe`/`identify`).

### Tier 2 — on-hardware bench
1. **Full playback** — power on with CAN idle (speed 0) → splash plays ~2 s → speedo.
2. **Abort-on-motion** — power on with CAN replaying speed > 0 → splash cuts out fast → speedo.
3. **Graceful skip** — boot before uploading frames (`/splash` absent) → normal display, no hang.
4. **OTA-during-splash** — push an update mid-splash → it takes over and updates.
5. **Decode-rate measurement** — log ms/frame over Serial → confirm ~15 fps or choose 12;
   decide sprite-buffer need from observed tearing.
6. **Post-splash cleanliness** — no splash pixels ghosting behind the speedo.

### Tier 3 — regression
Normal CAN display (speed/gear/brightness/sport) unaffected; OTA still works from the loop.

**Non-negotiable gates:** graceful skip on missing frames, and bounded playback — both
protect boot and update-mode-first.

## Resolved Implementation Details (as built target)

- **Source:** `mazdaspeed_splash.mp4` (240×240, 60 fps, 2.0 s).
- **Transcode:** `ffmpeg -vf fps=15 -q:v 4` (high quality) → ~30 baseline JPEGs, ~188 KB.
- **On-device path:** `/splash/000.jpg …` in LittleFS (uploaded with fonts).
- **Library:** TJpg_Decoder (Bodmer), added to `Arduino/libraries`, TFT_eSPI output callback.
- **Target rate:** 15 fps, direct-to-panel; fall back to 12 fps / sprite buffer per testing.
- **Abort:** CAN `0x202` decoded speed > 0.
