# Mazda MX-5 ND digital speedometer

A digital speed and gear readout for the Mazda MX-5 ND (2016-2024), built on an **ESP32** and a 240x240 **ST7789 TFT**. It listens to the car's CAN bus and shows the current speed, current gear (blank for Neutral). No nagging shift recommendation!. 
Brightness follows the dashboard: it dims with the instrument cluster at night and stays at full brightness during the day.
As it's an ESP32, it can be updated wirelessly without disassembling the dash should the need arise. 

This repo is geared towards MT vehicles (no pun intended), however I will keep the AT logic and use some form of detection (if possible) to differentiate between them, hopefully leading to a single package.
If dynamic transmission detection is not possible/above my skills, I could _potentially_ provide MT and AT packages. 

Probably not compatible with the ND3 (2024+). In theory the ESP is only reading CAN data, and I don't believe the CAN IDs would have radically changed. Input requested on this.

## BOM

- ESP32 (Any variant should be fine, just make sure it can drive the display comfortably).
- SN65HVD230 CAN transceiver to bridge ESP32's TWAI to the vehicles CAN network. 
- Generic 240x240 ST7789 display

## What it reads from the bus

These values were supplied by the original project, subject to change for MT vehicles:

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

**THIS IS A WIP!**
Migrating the original Arduino stuff to an ESP native language, as well as streamlining some of the setup process (along with a _very_ loose guide). 

## Repo layout

```
Arduino/can_receiver_display/   main RP2040 sketch (display, CAN, OTA client, splash)
Arduino/xiao_ota_bridge/        XIAO ESP32-S3 Wi-Fi update bridge
Arduino/libraries/              vendored mcp_can + shared ota_protocol library
tools/                          build/OTA/splash helper scripts
docs/                           design notes and implementation plans
```

## Development notes

This is mostly building off the original work done by MrSnowball, so massive props to them!
Originally I was looking at using an ESP32 to drive the OE segmented display until I stumbled across MrSnowball's work. 
I have a handful of ESP32s and no Arduinos (and even less Arduino knowledge), so begins cutting out the Arduino and going ESP32 only. 
