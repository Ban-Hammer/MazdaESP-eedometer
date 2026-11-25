# ESP32-only port (HiLetgo ESP32-DevKitC-32 + SN65HVD230 + ST7789)

**Status: written but not compiled or run on hardware.** Expect to fix a typo or two on first build.

## Wiring

| SN65HVD230 | ESP32 |
|---|---|
| 3V3 / GND | 3V3 / GND |
| D (TXD) | GPIO 25 |
| R (RXD) | GPIO 26 |
| Rs | GND (high-speed mode) |
| CANH / CANL | OBD-II pin 6 / pin 14 (HS-CAN) |

In the car, do not add termination: you are tapping a bus that is already terminated at both ends, so remove or disable the 120 ohm resistor if your SN65HVD230 breakout has one fitted. On a bench with only two nodes (this board and the replay device), the bus needs 120 ohm at each end.

| ST7789 | ESP32 |
|---|---|
| SCL | GPIO 18 |
| SDA | GPIO 23 |
| CS | GPIO 5 |
| DC | GPIO 19 |
| RST | GPIO 21 |
| BLK | GPIO 22 |
| VCC / GND | 3V3 / GND |

Power the board from a 5 V automotive USB/buck supply, not from the OBD-II port's raw 12 V.

## Build and flash (PlatformIO)

```
pio run -t upload      # firmware
pio run -t uploadfs    # fonts from data/  (needed once, and after font changes)
pio device monitor
```

Arduino IDE users: copy `src/*` into a sketch folder, copy `arduino_ide/User_Setup.h` over TFT_eSPI's, install TFT_eSPI + TJpg_Decoder, pick *ESP32 Dev Module* with the *Default 4MB with spiffs* partition scheme, and use the `arduino-littlefs-upload`-style plugin for `data/`.

## CAN mode

`-DCAN_LISTEN_ONLY=1` (default, in `platformio.ini`) never ACKs or transmits, which is what you want in the car. For bench replay from another ESP32/SavvyCAN rig, set it to `0` so this node ACKs frames, otherwise the sender sees errors.

Bitrate is 500 kbps. The TWAI controller accepts all IDs; the decoder ignores everything it doesn't know.

## Capturing CAN data (finding RPM / gear frames)

A second build, `logger`, dumps every frame on the bus over USB serial as GVRET-style CSV, which SavvyCAN can import. It is listen-only and uses the same wiring (no display needed).

```
pio run -e logger -t upload
stty -F /dev/ttyUSB0 921600 raw -echo
cat /dev/ttyUSB0 | tee drive1.csv      # Ctrl+C to stop
grep -v '^#' drive1.csv > drive1_clean.csv
```

Delete any garbage lines before the `Time Stamp,ID,...` header (the board prints boot noise when the port opens). `# missed frames` lines mean the serial link couldn't keep up; the capture is still usable, just with gaps. Flash `pio run -t upload` (no `-e`) to go back to the speedometer.

## Wireless updates

Copy `src/secrets.h.example` to `src/secrets.h` and set a real password. The ESP32 then hosts a WPA2 AP (`MX5-Speedo`) at `http://192.168.4.1/`:

- flash a firmware `.bin` (`.pio/build/esp32dev/firmware.bin`)
- upload / delete LittleFS files (fonts). Reboot afterwards so new fonts load.

To keep Wi-Fi from running while driving, the AP only starts in the first 10 minutes after power-up and only while speed is 0. It shuts off after 2 minutes with no client. Power-cycle to re-open the window.

## What changed from the original

| Original | Now |
|---|---|
| Adafruit Feather RP2040 CAN + MCP2515 | ESP32 TWAI + SN65HVD230 |
| `mcp_can` masks/filters, INT pin polling | accept-all filter, drain RX queue each loop |
| RP2040 PIO SPI @ 62.5 MHz | ESP32 VSPI @ 40 MHz |
| `analogWrite` backlight | LEDC PWM (works on arduino-esp32 2.x and 3.x) |
| XIAO ESP32-S3 bridge + UART protocol + `Updater` | on-chip WebServer + `Update` |
| RP2040 hardware watchdog | not ported yet (see TODO) |

Unchanged: speed/gear CAN decoding, brightness logic, debouncing. Removed for the manual-transmission build: the D/M indicator (CAN 0x131, `dm40` font), the sport-mode label (CAN 0x09E, `mazdaspeed18` font) and the boot splash (see `disabled_splash/README.md`).

## TODO / known gaps

- Hardware/task watchdog (`esp_task_wdt`) is not wired up.
- Power-state handling (sleep when CAN goes silent) is still unimplemented, as in the original.
- `tools/*.sh` and `CLAUDE.md` in the main repo still describe the RP2040 build.
- **Manual-transmission gear:** the gear code is still the original automatic-transmission logic (CAN 0x228: selector P/R/N/D, gear number only read while in D). A manual car may not report gear that way, so the gear digit may stay on its default or show nothing useful. Capture the bus (e.g. with SavvyCAN while shifting) and the decode in `handleFrame()` can be pointed at the right frame.
