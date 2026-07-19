# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Adafruit Feather RP2040 CAN-based display for a Mazda vehicle. Reads speed and transmission data from the CAN bus and displays it on a TFT screen.

## Vehicle Compatibility

This project is designed for **Mazda MX-5 ND (2016-2024)**. It is likely not compatible with ND3 (2024+) models due to differences in the electrical system and speedometer design.

## Build and Upload

This is an Arduino project. Build and upload with the Arduino IDE or `arduino-cli`.

**The RP2040 firmware compiles the wireless-OTA path in (`ota_update.*` +
`Updater`/`PicoOTA`), which requires a filesystem partition ≥128 KB. The FQBN
MUST therefore carry a `flash=` option that reserves an FS — building with the
bare `rp2040:rp2040:adafruit_feather` FQBN now fails with `Need to define a 128K
or larger filesystem to use OTA`.**

```bash
# RP2040 display — flash=8388608_2097152 => 8MB total, 6MB sketch / 2MB FS
arduino-cli compile --fqbn rp2040:rp2040:adafruit_feather:flash=8388608_2097152 can_receiver_display
arduino-cli upload  -p /dev/ttyACM0 \
  --fqbn rp2040:rp2040:adafruit_feather:flash=8388608_2097152 can_receiver_display

# XIAO ESP32-S3 OTA bridge (sibling sketch)
arduino-cli compile --fqbn esp32:esp32:XIAO_ESP32S3 ../xiao_ota_bridge
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

## Testing Setup

Development and testing is done on a bench using a real speedometer cluster. CAN data recorded from the vehicle is replayed using an ESP32 board with SavvyCAN software.

## Known TODOs (from source)

_(none outstanding)_

## Planned Work (future)

- **Firmware updates are USB-only (decided).** The Feather RP2040 (CAN) has no
  wireless radio (neither WiFi nor Bluetooth), and the hardware is fixed to this
  board — so OTA is out of scope. Flash via USB with the Arduino IDE / CLI.
- **Power state management:** detect when the car is off (e.g. CAN bus goes silent
  — `0x09F`/`0x202` stop, or an ignition/accessory signal) and shut down cleanly
  (blank the display, drop the backlight, and enter a low-power/sleep state) instead
  of idling on the "Waiting CAN…" screen. Wake on CAN traffic resuming.
