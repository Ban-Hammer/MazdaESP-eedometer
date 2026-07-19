# CAN-Driven Backlight Brightness Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the TFT display backlight track the Mazda gauge-cluster illumination by reading brightness from CAN ID `0x09F`, with smooth PWM dimming.

**Architecture:** Pure firmware change to the single-file sketch `can_receiver_display.ino`. Add `0x09F` to the MCP2515 receive filters, parse two bytes (D2 = night level, D6 = day-mode) into a target PWM duty, and ramp the backlight (a PWM GPIO wired to the TFT `BLK` pin) toward that target on a fixed timer. No analog sensing hardware.

**Tech Stack:** Adafruit Feather RP2040, Arduino (earlephilhower rp2040 core), TFT_eSPI, mcp_can (MCP2515). Verified with `arduino-cli` + Serial monitor on a bench cluster replaying CAN via SavvyCAN.

## Global Constraints

- **Single-file sketch.** All changes go in `Arduino/can_receiver_display/can_receiver_display.ino`. Do not split into new files (established project pattern — see CLAUDE.md).
- **Board / FQBN:** `rp2040:rp2040:adafruit_feather`. Compile from `Arduino/`:
  `arduino-cli compile --fqbn rp2040:rp2040:adafruit_feather can_receiver_display`
  (or use the Arduino IDE "Verify" button if `arduino-cli` is unavailable).
- **Pins already in use — do not reuse:** SPI/TFT `GP2,3,4,5,6`; MCP2515 `CS=GP19`, `INT=GP22`. Backlight uses free `GP10`.
- **`TFT_BL` must stay undefined** in `TFT_eSPI/User_Setup.h` so firmware owns the backlight pin. A TFT_eSPI library update resets `User_Setup.h` to stock — re-verify after any update (CLAUDE.md).
- **CAN byte convention:** SavvyCAN `D2` = `rxBuf[1]`, `D6` = `rxBuf[5]` (buffer is 0-indexed).
- **Byte offsets/ranges are bench-decoded** and behind `#define`s — a real car may differ; changing them is one line.

## Prerequisites

- The repo at `/home/honk/mazda` is **not** a git repository. The `git commit` step in each task assumes you have run `git init` once (recommended, so each task is an independent rollback point):
  ```bash
  cd /home/honk/mazda && git init && git add -A && git commit -m "chore: baseline before backlight feature"
  ```
  If you choose not to use git, simply skip the commit step in each task.

---

### Task 1: Firmware-controlled backlight (static PWM)

Wire the backlight to a GPIO and drive it from firmware at a fixed level. This
proves the pin, wiring, and PWM before any CAN logic is involved.

**Files:**
- Modify: `Arduino/can_receiver_display/can_receiver_display.ino`

**Interfaces:**
- Consumes: nothing.
- Produces: `#define BL_PIN 10`; global `uint8_t currentBrightness`; a working
  `analogWrite(BL_PIN, …)` backlight, initialized in `setup()`.

**Hardware step (do first):**
- Determine how the module's `BLK` pin behaves with a multimeter / its datasheet:
  - If `BLK` is a **high-impedance control input**, wire `BLK` → `GP10` directly.
  - If `BLK` is the **LED supply node** (drawing >~10 mA), do **not** drive it from
    the GPIO. Insert an N-MOSFET (e.g. 2N7002 / AO3400): `GP10` → gate, MOSFET
    low-side switching the backlight, gate pulldown 100 kΩ to GND.
- If `BLK` was previously tied to 3V3 (always-on), remove that tie so GP10 controls it.

- [ ] **Step 1: Add the backlight pin define**

In `can_receiver_display.ino`, immediately after the line `#define CS_PIN 19`, add:

```c
// ---- Backlight (TFT BLK pin, PWM dimming) ------------------------------------
#define BL_PIN 10        // GP10 -> TFT BLK (free; not SPI GP2-6 or CAN GP19/22)
```

- [ ] **Step 2: Add the brightness global**

After the CAN-liveness globals (right after the line `bool waitingShown = false;`), add:

```c
// Backlight brightness state (0-255 PWM duty). Starts full so the boot and
// "Waiting CAN..." screens are readable.
uint8_t currentBrightness = 255;
```

- [ ] **Step 3: Initialize the backlight in setup()**

In `setup()`, immediately after the line `tft.fillScreen(TFT_BLACK);`, add:

```c
// Backlight PWM: ~1 kHz, 8-bit range, start at full brightness.
pinMode(BL_PIN, OUTPUT);
analogWriteFreq(1000);
analogWriteRange(255);
analogWrite(BL_PIN, currentBrightness);
```

- [ ] **Step 4: Compile**

Run (from `/home/honk/mazda/Arduino`):
```bash
arduino-cli compile --fqbn rp2040:rp2040:adafruit_feather can_receiver_display
```
Expected: compiles with no errors (prints "Sketch uses … bytes").

- [ ] **Step 5: Bench-verify static control**

Temporarily change Step 2's initializer to `uint8_t currentBrightness = 40;`,
re-compile, upload
(`arduino-cli upload -p <PORT> --fqbn rp2040:rp2040:adafruit_feather can_receiver_display`),
and confirm the screen is visibly **dim**. Then restore it to `255`, re-upload, and
confirm the screen is **full brightness**. This proves GP10/BLK controls brightness.

- [ ] **Step 6: Commit**

```bash
cd /home/honk/mazda
git add Arduino/can_receiver_display/can_receiver_display.ino
git commit -m "feat: firmware-controlled TFT backlight on GP10"
```

---

### Task 2: Receive 0x09F and print its payload

Add `0x09F` to the hardware receive filters and log its bytes, so you can confirm
the D2/D6 decode live on the bench before wiring it to brightness.

**Files:**
- Modify: `Arduino/can_receiver_display/can_receiver_display.ino`

**Interfaces:**
- Consumes: the existing `rxId` / `rxBuf` receive path in `loop()`.
- Produces: `0x09F` frames delivered to `loop()`; a Serial debug line per frame.

- [ ] **Step 1: Add the receive filter for 0x09F**

In `setup()`, immediately after the line `CAN0.init_Filt(2, 0, 0x01310000);`, add:

```c
// RXB1 (exact 11-bit match) also lets 0x09F (dashboard illumination) through.
CAN0.init_Filt(3, 0, 0x009F0000);
```

- [ ] **Step 2: Add a temporary debug print in loop()**

In `loop()`, inside the `if (!digitalRead(CAN0_INT)) { … }` block, right after the
`if (rxId == 0x131) { … }` block closes, add:

```c
if (rxId == 0x09F) {
  // TEMP decode check: watch D2 (level) and D6 (day mode) while turning the
  // dimmer wheel / toggling day mode on the bench. Removed once confirmed.
  Serial.print("0x09F:");
  for (int i = 0; i < 8; i++) {
    Serial.print(' ');
    if (rxBuf[i] < 16) Serial.print('0');
    Serial.print(rxBuf[i], HEX);
  }
  Serial.print("  D2=0x"); Serial.print(rxBuf[1], HEX);
  Serial.print(" D6=0x");  Serial.println(rxBuf[5], HEX);
}
```

- [ ] **Step 3: Compile**

```bash
arduino-cli compile --fqbn rp2040:rp2040:adafruit_feather can_receiver_display
```
Expected: compiles with no errors.

- [ ] **Step 4: Bench-verify the decode**

Upload, open Serial Monitor at 115200. With the cluster/replay running:
- Spin the dimmer wheel — confirm **`D2` changes** across roughly `0x0D`…`0x64`.
- Toggle full daylight mode — confirm **`D6` changes** toward `0xC8` (and back to ~`0x48`).

If a different byte tracks the dimmer/day state, note the correct SavvyCAN column and
map it to its `rxBuf` index (`Dn` = `rxBuf[n-1]`); you'll set the `#define`s in Task 3.

- [ ] **Step 5: Commit**

```bash
cd /home/honk/mazda
git add Arduino/can_receiver_display/can_receiver_display.ino
git commit -m "feat: receive 0x09F and log payload for decode check"
```

---

### Task 3: Map 0x09F to a target brightness

Add the tunable constants and a pure mapping function, and compute a
`targetBrightness` from each `0x09F` frame. Still logs to Serial; does not yet drive
the backlight (Task 4 does the ramp/apply).

**Files:**
- Modify: `Arduino/can_receiver_display/can_receiver_display.ino`

**Interfaces:**
- Consumes: `rxBuf[]` from the `0x09F` receive path (Task 2).
- Produces:
  - `#define`s `BRIGHT_ID`, `BRIGHT_LEVEL_IDX`, `DAY_MODE_IDX`, `DAY_MODE_THRESHOLD`,
    `NIGHT_LEVEL_MIN/MAX`, `NIGHT_PWM_MIN/MAX`.
  - `uint8_t computeTargetBrightness(uint8_t level, uint8_t dayByte)` → PWM duty 0–255.
  - global `uint8_t targetBrightness`.

- [ ] **Step 1: Add the mapping constants**

Immediately after the `#define BL_PIN 10` line from Task 1, add:

```c
#define BRIGHT_ID           0x09F
#define BRIGHT_LEVEL_IDX    1      // SavvyCAN D2 = night dimmer level
#define DAY_MODE_IDX        5      // SavvyCAN D6 = day-mode byte
#define DAY_MODE_THRESHOLD  0xA0   // D6 at/above this => daylight => full brightness
#define NIGHT_LEVEL_MIN     0x0D   // dimmest dimmer-wheel value
#define NIGHT_LEVEL_MAX     0x64   // brightest dimmer-wheel value
#define NIGHT_PWM_MIN       12     // display PWM at dimmest night (readable floor)
#define NIGHT_PWM_MAX       120    // display PWM at brightest night
```

- [ ] **Step 2: Add the target global**

Directly below `uint8_t currentBrightness = 255;` (Task 1, Step 2), add:

```c
uint8_t targetBrightness  = 255;   // brightness we are ramping toward
```

- [ ] **Step 3: Add the mapping function**

Above `void setup() {`, add:

```c
// Map a 0x09F frame to a target backlight PWM duty (0-255).
// Daylight mode -> full; otherwise scale the night dimmer level, clamped so the
// screen never goes fully dark.
uint8_t computeTargetBrightness(uint8_t level, uint8_t dayByte) {
  if (dayByte >= DAY_MODE_THRESHOLD) return 255;
  if (level < NIGHT_LEVEL_MIN) level = NIGHT_LEVEL_MIN;
  if (level > NIGHT_LEVEL_MAX) level = NIGHT_LEVEL_MAX;
  return (uint8_t)map(level, NIGHT_LEVEL_MIN, NIGHT_LEVEL_MAX,
                      NIGHT_PWM_MIN, NIGHT_PWM_MAX);
}
```

- [ ] **Step 4: Compute the target in the 0x09F handler**

Replace the temporary debug block from Task 2 (the whole `if (rxId == 0x09F) { … }`)
with:

```c
if (rxId == 0x09F) {
  targetBrightness = computeTargetBrightness(rxBuf[BRIGHT_LEVEL_IDX],
                                             rxBuf[DAY_MODE_IDX]);
  Serial.print("bright level=0x"); Serial.print(rxBuf[BRIGHT_LEVEL_IDX], HEX);
  Serial.print(" day=0x");         Serial.print(rxBuf[DAY_MODE_IDX], HEX);
  Serial.print(" -> target=");     Serial.println(targetBrightness);
}
```

- [ ] **Step 5: Compile**

```bash
arduino-cli compile --fqbn rp2040:rp2040:adafruit_feather can_receiver_display
```
Expected: compiles with no errors.

- [ ] **Step 6: Bench-verify the target tracks the dimmer**

Upload; watch Serial. Turning the dimmer up/down should move `target=` across roughly
`12`…`120`; entering daylight mode should snap `target=255`. (Backlight itself does
not change yet — that is Task 4.)

- [ ] **Step 7: Commit**

```bash
cd /home/honk/mazda
git add Arduino/can_receiver_display/can_receiver_display.ino
git commit -m "feat: map 0x09F to target backlight brightness"
```

---

### Task 4: Smoothly ramp the backlight to the target

Drive the backlight from `currentBrightness`, ramping it toward `targetBrightness`
on a fixed timer so changes glide instead of stepping.

**Files:**
- Modify: `Arduino/can_receiver_display/can_receiver_display.ino`

**Interfaces:**
- Consumes: `currentBrightness`, `targetBrightness`, `BL_PIN`.
- Produces: `void updateBacklight()`; ramp constants; a per-loop call that applies
  `analogWrite(BL_PIN, currentBrightness)`. Replaces Task 1's static backlight write.

- [ ] **Step 1: Add ramp constants and timer global**

Directly below `uint8_t targetBrightness = 255;` (Task 3, Step 2), add:

```c
#define RAMP_INTERVAL_MS 25    // how often the ramp advances
#define RAMP_STEP        4     // PWM counts per ramp tick
unsigned long lastRampMillis = 0;
```

- [ ] **Step 2: Add the ramp function**

Directly below `computeTargetBrightness(...)` (Task 3, Step 3), add:

```c
// Advance currentBrightness toward targetBrightness by at most RAMP_STEP each
// RAMP_INTERVAL_MS, then apply it. Self-throttling; safe to call every loop().
void updateBacklight() {
  unsigned long now = millis();
  if (now - lastRampMillis < RAMP_INTERVAL_MS) return;
  lastRampMillis = now;

  int cur = currentBrightness;
  int tgt = targetBrightness;
  if (cur < tgt)      cur += min(RAMP_STEP, tgt - cur);
  else if (cur > tgt) cur -= min(RAMP_STEP, cur - tgt);

  currentBrightness = (uint8_t)cur;
  analogWrite(BL_PIN, currentBrightness);
}
```

- [ ] **Step 3: Call the ramp every loop iteration**

In `loop()`, immediately after the `#ifdef DEMO_MODE … #endif` block at the top
(right before `if (!digitalRead(CAN0_INT)) {`), add:

```c
updateBacklight();
```

- [ ] **Step 4: Remove the now-redundant static write in setup()**

In `setup()` (Task 1, Step 3), delete the line `analogWrite(BL_PIN, currentBrightness);`.
Keep `pinMode`, `analogWriteFreq`, and `analogWriteRange`. The first `updateBacklight()`
call in `loop()` now drives the pin. (Leaving the static write in is harmless but
redundant; remove it so there is a single point of control.)

- [ ] **Step 5: Compile**

```bash
arduino-cli compile --fqbn rp2040:rp2040:adafruit_feather can_receiver_display
```
Expected: compiles with no errors.

- [ ] **Step 6: Bench-verify smooth tracking**

Upload. Turn the dimmer wheel: the screen brightness should **follow smoothly**
across the night range. Switch to daylight mode: the screen should ramp up to full.
There should be no visible flicker or abrupt jumps.

- [ ] **Step 7: Commit**

```bash
cd /home/honk/mazda
git add Arduino/can_receiver_display/can_receiver_display.ino
git commit -m "feat: smooth-ramp backlight to CAN brightness target"
```

---

### Task 5: Edge cases — CAN timeout and demo mode

On CAN dropout, hold the last cluster-commanded brightness (the "Waiting CAN…"
screen dims with everything else — it is not forced to full), and add a backlight
sweep to demo mode so the feature is demonstrable off the bus.

**Files:**
- Modify: `Arduino/can_receiver_display/can_receiver_display.ino`

**Interfaces:**
- Consumes: `BL_PIN`, `NIGHT_PWM_MIN`, the existing `runDemo()`.
- Produces: a brightness sweep inside `runDemo()`. (No brightness change on CAN
  timeout — `targetBrightness` simply retains its last value.)

- [ ] **Step 1: Confirm CAN timeout does NOT change brightness**

No code change is needed in the `else if (!waitingShown && (millis() - lastCanMillis > CAN_TIMEOUT_MS))`
branch. Because `targetBrightness` is only written when a `0x09F` frame arrives, it
naturally **holds its last value** when CAN drops, so `updateBacklight()` keeps the
"Waiting CAN…" screen at the brightness the cluster last commanded. Verify this
branch contains only `showWaitingCan(); waitingShown = true; Serial.println(...)` —
do **not** add any `targetBrightness = …` line here.

- [ ] **Step 2: Add a backlight sweep to demo mode**

Inside the `#ifdef DEMO_MODE` `runDemo()` function, at the very end of the function
(after the final `delay(800);`), add:

```c
// Backlight sweep so brightness control is visible without the CAN bus.
for (int b = 255; b >= NIGHT_PWM_MIN; b -= 4) { analogWrite(BL_PIN, b); delay(15); }
for (int b = NIGHT_PWM_MIN; b <= 255; b += 4) { analogWrite(BL_PIN, b); delay(15); }
```

- [ ] **Step 3: Compile**

```bash
arduino-cli compile --fqbn rp2040:rp2040:adafruit_feather can_receiver_display
```
Expected: compiles with no errors.

- [ ] **Step 4: Bench-verify edge cases**

- Set the dimmer to a low night level, then disconnect CAN (or stop the replay):
  the "Waiting CAN…" screen should stay at that **dim** level (not jump to full).
  Reconnect: brightness resumes tracking the dimmer.
- Temporarily enable `#define DEMO_MODE` (uncomment it near the top), upload, and
  confirm the screen **sweeps dim↔bright** during the demo. Re-comment `DEMO_MODE`
  and re-upload for normal operation.

- [ ] **Step 5: Commit**

```bash
cd /home/honk/mazda
git add Arduino/can_receiver_display/can_receiver_display.ino
git commit -m "feat: full brightness on CAN timeout + demo backlight sweep"
```

---

## Final cleanup

- [ ] Update `Arduino/can_receiver_display/CLAUDE.md`: add `0x09F` to the CAN Protocol
  table (D2 = illumination level `0x0D`–`0x64`, D6 = day-mode `0x48`–`0xC8`), note
  the `GP10` backlight pin under Hardware Configuration, and remove the completed
  backlight TODO from "Known TODOs".
- [ ] Delete the obsolete `Arduino/can_receiver_display/auto_brightness_plan.txt`
  (superseded by this feature; the design doc records why).
- [ ] Commit:
  ```bash
  cd /home/honk/mazda
  git add -A
  git commit -m "docs: record 0x09F + backlight pin, drop obsolete analog plan"
  ```
