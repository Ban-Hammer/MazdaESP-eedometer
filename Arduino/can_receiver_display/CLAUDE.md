# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Adafruit Feather RP2040 CAN-based display for a Mazda vehicle. Reads speed and transmission data from the CAN bus and displays it on a TFT screen. Plays an MJPEG boot splash from LittleFS, and supports **wireless firmware/file updates** via a companion XIAO ESP32-S3 (see "Wireless OTA & File Browser" below). The RP2040 runs overclocked at **250 MHz** (`freq=250`) for splash JPEG-decode headroom.

## Vehicle Compatibility

This project is designed for **Mazda MX-5 ND (2016-2024)**. It is likely not compatible with ND3 (2024+) models due to differences in the electrical system and speedometer design.

## Build and Upload

This is an Arduino project. Build and upload with the Arduino IDE or `arduino-cli`.

**Use the `adafruit_feather_can` FQBN, NOT the generic `adafruit_feather`.** The
board is the Adafruit Feather RP2040 **CAN Bus**; its integrated MCP2515 sits on
the default `SPI` mapped to `spi1` (SCK=14, MOSI=15, MISO=8, CS=19, INT=22). The
generic `adafruit_feather` variant maps `SPI` to 18/19/20 (MOSI 19 even collides
with CAN CS 19), so CAN init fails with repeating `Enter Configuration Mode
Failure...` and — with no CAN — the screen stays blank.

**Also required:** the `flash=` option must reserve an FS partition ≥128 KB, or
the compiled-in OTA path (`ota_update.*` + `Updater`/`PicoOTA`) fails to build
with `Need to define a 128K or larger filesystem to use OTA`.

```bash
# RP2040 display — _can variant + flash=8388608_2097152 (8MB: 6MB sketch / 2MB FS)
arduino-cli compile --fqbn rp2040:rp2040:adafruit_feather_can:flash=8388608_2097152,freq=250 can_receiver_display
arduino-cli upload  -p /dev/ttyACM0 \
  --fqbn rp2040:rp2040:adafruit_feather_can:flash=8388608_2097152,freq=250 can_receiver_display

# XIAO ESP32-S3 OTA bridge (sibling sketch) — PSRAM=opi is REQUIRED: the bridge
# buffers the image in PSRAM, and the board defaults to PSRAM=disabled. Without it
# ps_malloc() fails and every upload reports "FAIL: image too large".
arduino-cli compile --fqbn esp32:esp32:XIAO_ESP32S3:PSRAM=opi ../xiao_ota_bridge
```

In the Arduino IDE, the equivalent is **Tools → Flash Size → "8MB (Sketch: 6MB,
FS: 2MB)"**. See `docs/superpowers/specs/2026-07-18-rp2040-wireless-ota-design.md`
for the OTA design.

### Uploading fonts to LittleFS (required before first run)

The `.vlw` fonts in `data/` must be flashed to the board's LittleFS partition, or
the speed/gear sprites fail to render and the display stays blank (`Font file ...
not found!` on Serial). This is a separate step from uploading the sketch.

On the Arduino IDE (2.x), this is done with the **`arduino-littlefs-upload`**
plugin (by earlephilhower), NOT `arduino-cli`:

1. Install the plugin: download the `.vsix` from
   https://github.com/earlephilhower/arduino-littlefs-upload/releases and place it
   in `~/.arduinoIDE/plugins/` (create the folder if missing — the AppImage does
   not create it automatically), then restart the IDE.
2. Ensure `Tools -> Flash Size` reserves a nonzero **FS** partition — use the same
   "8MB (Sketch: 6MB, FS: 2MB)" layout the OTA build requires (see above). The FS
   holds the fonts (~135 KB) *and* the staged OTA image (~1 MB).
3. Open this sketch (so its `data/` folder is found), close the Serial Monitor
   (it holds the port), then run `Ctrl+Shift+P` -> "Upload LittleFS to
   Pico/RP2040...".

On success, Serial prints `All fonts are there!` at boot.

## Required Libraries

- TFT_eSPI - TFT display driver (already configured for this project)
- mcp_can - MCP2515 CAN controller library

## Hardware Configuration

**MCP2515 CAN Controller**
- CS=GPIO19, INT=GPIO22, 16MHz oscillator, 500kbps

**TFT Display (Generic ST7789 240×240 IPS)**
- Driver: ST7789 via TFT_eSPI library
- Interface: SPI0 using RP2040 PIO (~60MHz; `SPI_FREQUENCY 62500000`)
- Pins: MOSI=GPIO3, SCLK=GPIO2, CS=GPIO5, DC=GPIO6, RST=GPIO4
- Configuration: `~/Arduino/libraries/TFT_eSPI/User_Setup.h` (defines
  `ST7789_DRIVER`, 240×240, `CGRAM_OFFSET`, `RP2040_PIO_SPI`, `TFT_SPI_PORT 0`,
  and the pins above)
- **Gotcha:** re-downloading/updating the TFT_eSPI library resets `User_Setup.h`
  to stock defaults (ILI9341 + ESP8266 pins), which produces a blank screen with
  the backlight on. Restore this project's config after any library update.

**Backlight (PWM dimming)**
- TFT `BLK` pin → GPIO10, driven as ~1kHz PWM (`analogWrite`, 0–255).
- `BLK` on this module is a high-impedance control input, so GPIO10 drives it directly.
- Keep `TFT_BL` **undefined** in `User_Setup.h` so the firmware owns the pin (the
  library-update gotcha above resets this too).
- Brightness auto-tracks the gauge cluster via CAN `0x09F` (see below): the level is
  mapped to a PWM target and smoothly ramped. Tunable `#define`s in the sketch:
  `NIGHT_PWM_MIN`/`NIGHT_PWM_MAX` (night range), `DAY_MODE_THRESHOLD`,
  `RAMP_STEP`/`RAMP_INTERVAL_MS` (ramp speed). On CAN dropout, brightness holds its
  last value (the "Waiting CAN…" screen dims with everything else).

## CAN Protocol

| CAN ID | Data | Description |
|--------|------|-------------|
| 0x202  | bytes[2:3] | Speed (raw × 0.01 = km/h) |
| 0x228  | byte[0] bits 0-2 | Gear selector (P=1, R=2, N=3, D=4) |
| 0x228  | byte[4] bits 1-4 | Current gear number (when in D) |
| 0x131  | byte[2] bit 7 (0x80) | Manual (M) mode engaged (byte[1]==0x04 marks the D/M gate; D itself is read from 0x228) |
| 0x09F  | byte[1] | Dashboard illumination / brightness level, 0x0D (dimmest) … 0x64 (brightest) — night dimmer wheel |
| 0x09F  | byte[5] | Day-mode byte: 0x48 at night, 0xC8 in full daylight mode (→ display full brightness) |
| 0x09E  | byte[7] bit 0x10 | Sport mode: last byte 0x10 = on, 0x00 = off (→ MAZDASPEED label) |

Note: `0x09F` is `MSG_11` in `mazda_2017.dbc` (present but undecoded upstream); the
byte meanings above are this project's bench decode. SavvyCAN's 1-based `Dn` column
= `byte[n-1]` here (D2 = byte[1], D6 = byte[5]).

## Architecture

Single-file Arduino sketch with interrupt-driven CAN message processing:

1. CAN interrupt triggers on message receipt
2. Messages filtered by hardware masks: RXB0 (mask 0x02FF) for IDs 0x202 and 0x228; RXB1 (mask 0x07FF, exact match) for 0x131, 0x09F, and 0x09E
3. Speed and gear data extracted and displayed via TFT_eSprite objects

**LittleFS** is essential for this project to store fonts and other resources. Font files (.vlw) are stored under `/data/` and loaded at startup. These .vlw fonts are essential for smooth anti-aliased text rendering.

### Fonts are character-subset .vlw files

Each `.vlw` was generated with ONLY the glyphs it needs, to save space. Rendering a
character that is not in a font's subset shows an empty box ("tofu"). Current subsets:

| Font (`.vlw`)   | Sprite       | Glyphs it contains        | Used for |
|-----------------|--------------|---------------------------|----------|
| `speed112_2`    | `speed_spr`  | `0123456789`              | Speed digits |
| `gear96`        | `gear_spr`   | `123456NPR` (**no `D`, no `M`**) | Big gear letter (P/R/N) or D-mode gear number |
| `dm40`          | `dm`         | `D M`                     | Small "D"/"M" label (drawn beside the big gear number) |
| `mazdaspeed18`  | `mazdaspeed` | `ADEMPSZ` (i.e. the letters of "MAZDASPEED") | Orange sport/"MAZDASPEED" label |

Implications:
- **"D" must be drawn from `dm40`, not `gear96`.** In Drive, show the big gear NUMBER
  from `gear96` plus a small `D` from `dm40`. Drawing `"D"` with `gear96` renders a box.
- The sport label text is **`MAZDASPEED`** (all its letters are the only glyphs in
  `mazdaspeed18`). Any other word will show boxes for missing letters.
- To add characters (e.g. an `M` for manual mode in the big font), regenerate the
  relevant `.vlw` with the extra glyphs included.

### `.vlw` descent must cover the glyphs, or `printToSprite` crops the bottom

`printToSprite()` builds its sprite `ascent + descent` tall (from the `.vlw` header)
and anchors the baseline at `ascent`. If a font's **declared descent is smaller than
how far its glyphs actually dip below the baseline**, the sprite is too short and the
bottom pixel rows are cropped — visible as antialiased letters/numbers clipped a few
pixels at the bottom, regardless of where the sprite is pushed on screen.

`gear96.vlw` shipped with `descent = -1` while its digits/letters reach +1px below the
baseline (a 2px shortfall) — its header was patched to `descent = 9` for comfortable
bottom margin. The alternates `speed112.vlw` and `speed112_3.vlw` had the **same
`descent = -1`** flaw and were patched to `descent = 9` too (the active speed font
`speed112_2` already declares `descent = 16`, so it was never affected). Originals are
backed up alongside each file as `*.vlw.bak`. Check any new `.vlw` with a header dump
(declared `descent` ≥ actual `max(glyphHeight − glyphDy)`, plus a pixel or two for the
antialiased edge); the proper fix is to regenerate the font with enough descent rather
than hand-patching.

### Always space-pad `printToSprite()` strings

`spr.printToSprite("D")` with a **single, unpadded character renders nothing** in
this project. Always pass a space-padded string, e.g. `spr.printToSprite(" D  ")`.
Padding serves two purposes:
- It forces `printToSprite` to create a wide enough sprite to actually display
  (bare single-char sprites do not show up here).
- The sprite is filled with the text background colour, so the padding **erases the
  previously drawn value** — this is how the display clears old digits/letters
  (there is no `fillRect` before each draw; the padded sprite does the clearing).

None of these subset fonts contain a space glyph, so spaces only add width + clear;
they never draw anything.

**Exception — the manual "M":** a padded sprite only clears the row it is drawn on.
The "M" (below the "D", shown only in manual mode) sits on a row that no other
sprite repaints, so when manual mode turns off nothing erases it. That one cell is
cleared explicitly before redraw with
`tft.fillRect(0, 205, dm.textWidth(" M  "), dm.fontHeight(), TFT_BLACK)` — the rect
is sized from the font so it exactly matches the M's footprint and can't clip the
neighbouring gear number.

## Wireless OTA & File Browser

Firmware and LittleFS files can be updated wirelessly via a companion **XIAO ESP32-S3**
(`Arduino/xiao_ota_bridge/`) permanently wired to the RP2040. The XIAO hosts a WPA2
SoftAP + web dashboard and bridges Wi-Fi ⇄ UART; the RP2040 owns flashing and its own
filesystem. Design/plan docs: `docs/superpowers/specs/2026-07-18-rp2040-wireless-ota-design.md`
and `2026-07-20-littlefs-file-push-design.md`.

- **Link:** `Serial1` UART, 921600 baud. XIAO D6(TX)→RP2040 GP1(RX), D7(RX)←GP0(TX),
  common GND, shared 3.3 V rail. Shared protocol lib: `Arduino/libraries/ota_protocol`
  (framing + CRC32 + block-ACK flow control; host-tested with `test/`).
- **RP2040 side:** `ota_update.{h,cpp}`. `ota::begin()` runs FIRST in `setup()`
  (update-mode-first: reachable even if later init crashes); `ota::service()` is polled
  at the top of `loop()`. Firmware OTA reboots into the arduino-pico stage-3 bootloader;
  file ops (list/upload/delete) write to `/.upload.tmp` then atomic-rename, no reboot.
- **Staging cleanup:** `ota::cleanupStaging()` (called in `setup()` after `LittleFS.begin`)
  removes the arduino-pico OTA leftovers `/firmware.bin` and `/otacommand.bin`.
- **Build OTA image:** `tools/build_ota.sh [version]` → `build/can_receiver_display.ino.bin`
  (upload via the dashboard's FIRMWARE card). **Must** use the same FQBN as the board.
- **XIAO build:** `esp32:esp32:XIAO_ESP32S3:PSRAM=opi` (PSRAM required — it buffers the
  image; without it uploads report "PSRAM unavailable"). AP creds in a gitignored
  `secrets.h` (copy `secrets.h.example`).

## Boot Splash

`splash.{h,cpp}` plays MJPEG frames from LittleFS `/splash/NNN.jpg` (via TJpg_Decoder)
at boot, then hands off to the speed/gear display. Aborts early if CAN speed > 0 (car
moving). Frames are generated by `tools/build_splash.sh` from `mazdaspeed_splash.mp4`
and uploaded to LittleFS with the fonts. Design: `docs/superpowers/specs/2026-07-19-boot-splash-video-design.md`.
After the splash, `fadeInFromBlack()` ramps the glyph **colours** (not the backlight)
up from black so the display materializes to match the splash's fade-out.

## Display Stabilization

Speed/gear readouts are de-flickered at value boundaries (tunable `#define`s near the
top of the sketch): `SPEED_HYST` (raw-count hysteresis so a steady x.5 km/h doesn't flip
the digit) and `GEAR_DEBOUNCE` (frames a new gear number must persist, to ignore 2↔3
hunting at a shift point).

## Testing Setup

Development and testing is done on a bench using a real speedometer cluster. CAN data recorded from the vehicle is replayed using an ESP32 board with SavvyCAN software.

## Known TODOs (from source)

_(none outstanding)_

## Planned Work (future)

- **Power state management:** detect when the car is off (e.g. CAN bus goes silent
  — `0x09F`/`0x202` stop, or an ignition/accessory signal) and shut down cleanly
  (blank the display, drop the backlight, and enter a low-power/sleep state). Wake on
  CAN traffic resuming. (Note: there is no longer a "Waiting CAN…" screen — with the
  ignition on CAN is always present, so the display just holds its last frame.)
