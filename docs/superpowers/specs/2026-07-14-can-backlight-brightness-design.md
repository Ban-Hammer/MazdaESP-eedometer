# CAN-Driven Backlight Brightness — Design

- **Date:** 2026-07-14
- **Project:** `Arduino/can_receiver_display` (Adafruit Feather RP2040, ST7789 240×240 IPS)
- **Status:** Approved for implementation planning
- **Supersedes:** `auto_brightness_plan.txt` (analog voltage-sensing approach — now obsolete)

## Summary

Sync the TFT display backlight to the Mazda gauge-cluster illumination by reading
the cluster's brightness level **directly from the CAN bus** (ID `0x09F`), instead
of sensing the analog voltage across the cluster's backlight LEDs. Tracking is
continuous / proportional. No sensing hardware is added; the only new wire is the
backlight PWM control to the TFT `BLK` pin, which is required for any brightness
control at all.

## Why CAN instead of analog sensing

`0x09F` is broadcast periodically (~3–4 Hz in the bench capture) and carries the
illumination state. Reading it is strictly simpler and more reliable than the
analog approach previously drafted in `auto_brightness_plan.txt`:

- No voltage divider, no ADC input, no RC filter, no clamp.
- No soldering to the cluster PCB / backlight LEDs.
- No analog calibration or drift; the value is exact and digital.

The community DBC (`mazda_2017.dbc`) labels `0x09F` as `BO_ 159 MSG_11` but leaves
it **undecoded** (no signals). The byte meanings below are our own bench decode.

## CAN decode (`0x09F`)

Observed live on the bench by spinning the dimmer wheel and toggling day mode.
SavvyCAN data column → firmware buffer index (the sketch's `rxBuf[]` is 0-indexed):

| Field            | SavvyCAN col | `rxBuf` index | Range                    | Meaning                        |
|------------------|--------------|---------------|--------------------------|--------------------------------|
| Brightness level | D2           | `rxBuf[1]`    | `0x0D` (13) … `0x64` (100) | Night dimmer-wheel position    |
| Day-mode byte    | D6           | `rxBuf[5]`    | `0x48` (night) / `0xC8` (day) | `0x48` at night, flips to `0xC8` in day mode |

SavvyCAN's D-columns are 1-based, so `Dn` = `rxBuf[n-1]`. All other bytes are
ignored. Values may differ on a real car (only bench data so far); byte offsets are
`#define`d so a change is one line. *(Confirmed live on the bench by turning the
dimmer wheel and toggling day mode.)*

## Brightness mapping

Output is an 8-bit PWM duty (0–255) driving the backlight.

- **Day:** if `D6 >= DAY_MODE_THRESHOLD` → target = **255** (full brightness).
- **Night:** otherwise map `D2` from `[0x0D … 0x64]` → `[NIGHT_PWM_MIN … NIGHT_PWM_MAX]`.
  The input is constrained to that range and never maps to 0, so the screen stays
  readable at the dimmest setting.

Tunable constants (start values; all adjustable on the bench):

```c
#define BRIGHT_ID           0x09F
#define BRIGHT_LEVEL_IDX    1      // SavvyCAN D2
#define DAY_MODE_IDX        5      // SavvyCAN D6
#define DAY_MODE_THRESHOLD  0xA0   // D6 at/above this = daylight mode -> full
#define NIGHT_LEVEL_MIN     0x0D   // dimmest dimmer-wheel value
#define NIGHT_LEVEL_MAX     0x64   // brightest dimmer-wheel value
#define NIGHT_PWM_MIN       12     // display PWM at dimmest night
#define NIGHT_PWM_MAX       120    // display PWM at brightest night
#define BL_PIN              10     // GP10 -> TFT BLK
```

**Optional gamma:** perceived brightness is nonlinear. If the low end looks steppy,
apply a gamma lookup table before `analogWrite`. Deferred until observed on hardware.

## Smoothing

On each `0x09F` receipt, compute and store `targetBrightness`. In `loop()`, every
~25 ms, ramp `currentBrightness` toward the target by a small fixed step (e.g. ±4)
and write it out. Ramping is independent of the CAN message rate, so brightness
glides between levels instead of stepping and cannot flicker on jittery input.

## Backlight hardware

- **New wire:** TFT `BLK` → **GP10** (free, PWM-capable; does not conflict with SPI
  on GP2–GP6 or the MCP2515 on GP19/GP22).
- **PWM:** `analogWriteFreq(1000)` (~1 kHz), `analogWriteRange(255)`. Raise toward
  ~20 kHz if the backlight coil-whines audibly.
- **`BLK` drive caveat (confirm before wiring):** if `BLK` is a high-impedance
  control input on the module, drive it directly from GP10. If `BLK` is actually the
  LED **supply** node, a GPIO cannot source that current — insert a small N-MOSFET
  (e.g. 2N7002 / AO3400), GP10 → gate, low-side switching the backlight. Check with a
  multimeter / the module datasheet first.
- **TFT_eSPI:** keep `TFT_BL` **undefined** in `User_Setup.h` so the firmware owns the
  pin. (Per CLAUDE.md, a library update resets `User_Setup.h` to stock — re-verify.)

## CAN filter change

Fit `0x09F` into the existing hardware-filter scheme. RXB1 uses mask `0x07FF`
(exact 11-bit match) with only filter 2 (`0x131`) currently used. Add filter 3:

```c
CAN0.init_Filt(3, 0, 0x009F0000);
```

RXB0 (`0x202` / `0x228`) and the `0x131` filter are untouched; `0x09F` is accepted
alongside the existing IDs with no mask conflict.

## `loop()` / `setup()` integration

**`setup()`** — initialize the backlight pin (start at full so the boot/"Waiting
CAN" screen is visible):

```c
pinMode(BL_PIN, OUTPUT);
analogWriteFreq(1000);
analogWriteRange(255);
analogWrite(BL_PIN, currentBrightness);   // currentBrightness starts at 255
```

**`loop()`**
- On `0x09F` receipt: parse D2/D6, compute `targetBrightness`.
- Every ~25 ms: ramp `currentBrightness` toward target; `analogWrite(BL_PIN, …)`
  (or via the gamma LUT if enabled).
- On CAN timeout (the existing "Waiting CAN…" branch): make **no** brightness
  change — `targetBrightness` holds its last value, so the waiting screen dims with
  everything else rather than blasting to full. Tracking resumes when frames return.

## Demo mode

In `runDemo()`, add a backlight sweep (full → dim → full) so brightness control is
demonstrable without the bus connected.

## Bench verification / calibration steps

1. Print the `0x09F` payload to Serial; spin the dimmer and toggle day mode; confirm
   the D2/D6 offsets and their ranges. Adjust the `*_IDX` / `*_MIN` / `*_MAX`
   `#define`s if the live values differ.
2. Tune `NIGHT_PWM_MIN`/`NIGHT_PWM_MAX` for comfortable night viewing and
   `DAY_MODE_THRESHOLD` for a clean day/night switch.
3. Confirm the `BLK` drive method (direct GPIO vs. MOSFET) before final wiring.

## Out of scope (YAGNI)

- Analog LED-voltage sensing (`auto_brightness_plan.txt`) — dropped in favor of CAN.
- PWM duty-cycle measurement — unnecessary; the level is digital on the bus.
- A separate ambient-light sensor — the cluster already provides the setting.
