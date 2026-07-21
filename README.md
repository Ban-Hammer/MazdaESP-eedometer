# Mazda MX-5 ND digital speedometer

A digital speed and gear readout for the Mazda MX-5 ND (2016-2024), built on an
**Adafruit Feather RP2040 CAN Bus** and a 240x240 **ST7789 TFT**. It listens to the
car's CAN bus and shows the current speed, the selected gear (with the actual
gear number in Drive, and an M indicator in manual mode), and a SPEED
banner when sport mode is engaged. Brightness follows the dashboard: it dims
with the instrument cluster at night and stays at full brightness
during the day.

Firmware and files can be updated wirelessly through a small companion board (a
XIAO ESP32-S3 in my case, any other ESP32 with WiFi should fit) that stays wired to the display, for easy firmware flashing.

As far as I'm aware it should be compatible with any ND1/ND2 dash. I have GT automatic, with color-dashboard retrofitted and teste on that.
Manuals may need some tweaking in gear detecting.

Probably not compatible with the ND3 (2024+) facelift - the electrical system
and cluster changed.

## Hardware

- Adafruit Feather RP2040 **CAN Bus** (the variant with the integrated MCP2515),
  overclocked to 250 MHz for JPEG decode headroom during the boot splash
- Generic 240x240 ST7789 IPS display, driven over PIO SPI at ~60 MHz
- *Optional: Seeed XIAO ESP32-S3 as a wireless update bridge*

Display wiring (in my case):
| Display | Adafruit RP2040 CAN |
|---------|---------------------|
| SCL | SDA |
| SDA | SCL |
| RES | 4 |
| DC | 6 |
| CS | 5 |
| BLK | 10 |

VCC and GND are shared. (I don't know if SDA and SCL should be reverted, but it works for me)
The MCP2515 is on the board already (CS=GP19, INT=GP22), running at
500 kbps with a 16 MHz oscillator.

## What it reads from the bus

All values were decoded on the bench from recorded traffic; not all values are present in public DBC files, so the byte
meanings below are this project's own decode.

| CAN ID | Data | Meaning |
|--------|------|---------|
| 0x202 | bytes 2-3 | Speed (raw x 0.01 = km/h) |
| 0x228 | byte 0, bits 0-2 | Gear selector (P=1, R=2, N=3, D=4) |
| 0x228 | byte 4, bits 1-4 | Current gear number when in D |
| 0x131 | byte 2, bit 0x80 | Manual mode engaged |
| 0x09F | byte 1 | Dashboard dimmer level, 0x0D (dimmest) to 0x64 (brightest) (aka night-mode) |
| 0x09F | byte 5, bit 0x80 | Day-mode toggle (forces full brightness) (aka day-mode) |
| 0x09A | byte 0 | Headlamp status: 0x00 off, 0x10 DRL, 0x30 low beam, 0xC0 high beam |
| 0x09E | byte 7, bit 0x10 | Sport mode |

One detail worth knowing: night dimming keys off the *actual* lamp state, not
the headlight switch. The car reports 0x10 (not 0x30) when the switch is on but
it's bright out, so the screen only dims when the low/high beams are genuinely
lit for darkness. DRLs never dim it, and the dash day/night button overrides
everything.

## Building and flashing

This is an Arduino project; build with the IDE or `arduino-cli`. Two things
will bite you if you skip them:

1. **Use the `adafruit_feather_can` FQBN, not the generic `adafruit_feather`.**
   The generic variant maps SPI to the wrong pins (MOSI 19 even collides with
   the CAN chip select), so CAN init fails with an endless
   `Enter Configuration Mode Failure...` and the screen stays blank.
2. **Reserve a filesystem partition of at least 128 KB** (`flash=` option, or
   Tools -> Flash Size in the IDE). The fonts, splash frames, and staged OTA
   images all live on LittleFS, and the OTA code won't even compile without it.

```sh
# RP2040 display (8 MB flash: 6 MB sketch / 2 MB FS)
arduino-cli compile --fqbn rp2040:rp2040:adafruit_feather_can:flash=8388608_2097152,freq=250 \
  Arduino/can_receiver_display
arduino-cli upload -p /dev/ttyACM0 \
  --fqbn rp2040:rp2040:adafruit_feather_can:flash=8388608_2097152,freq=250 \
  Arduino/can_receiver_display
```

Required libraries: TFT_eSPI and mcp_can (a copy of mcp_can is vendored under
`Arduino/libraries/`). TFT_eSPI needs its `User_Setup.h` configured for this
project (ST7789, 240x240, RP2040 PIO SPI, the pins above, and `TFT_BL` left
undefined so the firmware owns the backlight pin). Note that updating the
library resets `User_Setup.h` to defaults, which gets you a backlit but blank
screen - restore the config after any library update.

### Fonts (required before first run)

The display renders with anti-aliased `.vlw` fonts stored on LittleFS, and
uploading them is a separate step from flashing the sketch. Without them the
screen stays blank and Serial prints `Font file ... not found!`.

In Arduino IDE 2.x, install the
[arduino-littlefs-upload](https://github.com/earlephilhower/arduino-littlefs-upload)
plugin, open the sketch, close the Serial Monitor, and run Ctrl+Shift+P ->
"Upload LittleFS to Pico/RP2040...". On success the board prints
`All fonts are there!` at boot.

Each font contains only the glyphs it needs (to save space), so don't reuse
them for arbitrary text - a missing glyph renders as an empty box.

## Wireless updates

`Arduino/xiao_ota_bridge/` is a sketch for a XIAO ESP32-S3, that is permanently
wired to the RP2040, to update PR2040 over UART (921600 baud: XIAO D6 -> GP1, D7 <- GP0, common
ground). It brings up a WPA2 access point with a small web dashboard, from
which you can:

- upload a new firmware image (the RP2040 stages it on LittleFS and reboots
  into the bootloader to apply it)
- browse, upload, and delete LittleFS files - handy for swapping splash frames
  or fonts without USB

Build it with `esp32:esp32:XIAO_ESP32S3:PSRAM=opi` - PSRAM is required because
the bridge buffers the whole image there, and the board defaults to PSRAM off.
Wi-Fi credentials go in a gitignored `secrets.h` (copy `secrets.h.example`).

The framing protocol shared by both sides (CRC32, block ACKs) lives in
`Arduino/libraries/ota_protocol` and has host-side tests. `tools/ota_send.py`
speaks the same protocol over a USB-TTL adapter for bench testing without the
XIAO.

## Boot splash

On power-up the display plays a short MJPEG clip from LittleFS before switching
to the gauge view, and skips it if the car is already moving.
`tools/build_splash.sh` transcodes an mp4 into the frame files with ffmpeg,
`tools/check_splash.sh` validates them, and `tools/push_splash.sh` pushes them
to the car over Wi-Fi via the XIAO.

## Repo layout

```
Arduino/can_receiver_display/   main RP2040 sketch (display, CAN, OTA client, splash)
Arduino/xiao_ota_bridge/        XIAO ESP32-S3 Wi-Fi update bridge
Arduino/libraries/              vendored mcp_can + shared ota_protocol library
tools/                          build/OTA/splash helper scripts
docs/                           design notes and implementation plans
```

## Development notes

Development happens on the bench against a real speedometer cluster, with CAN
traffic recorded from the car and replayed through another ESP32 via SavvyCAN.
The design docs under `docs/superpowers/` cover the OTA protocol, the splash
pipeline, and the backlight logic in more detail.

I started this project 2 years ago, and could not possibly finish it if not Claude (I suck at Arduino C-like language).
