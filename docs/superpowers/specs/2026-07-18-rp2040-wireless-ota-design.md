# In-Car Wireless OTA for the RP2040 CAN Display — Design

**Date:** 2026-07-18
**Status:** Approved design, pre-implementation
**Project:** `Arduino/can_receiver_display` (Mazda MX-5 ND CAN display)

## Problem

The display runs on an **Adafruit Feather RP2040 CAN Bus** board. Its CAN controller
and transceiver are **integrated on the Feather** and cannot be relocated, and no spare
CAN hardware is available — so the RP2040 must remain the board that talks to the CAN
bus. The RP2040 has **no wireless radio**, which is why firmware updates were previously
USB-only (see `CLAUDE.md` "Planned Work").

A spare **Seeed XIAO ESP32-S3** (Wi-Fi + BLE, 8 MB flash, 8 MB PSRAM) is available. This
design adds **permanent in-car wireless firmware updates** to the RP2040 by wiring the
XIAO in as a Wi-Fi-to-UART bridge that relays a new firmware image to the RP2040, which
flashes itself using the arduino-pico OTA path.

This supersedes the "Firmware updates are USB-only (decided)" note in `CLAUDE.md`, whose
premise ("no wireless radio") no longer holds now that the XIAO provides one.

## Goals

- Update RP2040 firmware wirelessly while the car is parked, without opening the dash.
- Reuse the existing integrated CAN hardware — **no new CAN parts, no transceiver purchase.**
- Be safe against power loss (in-car power is unreliable) and against a bad upload.
- Keep a wireless path to recover from a broken update without physical access, with USB
  BOOTSEL as the ultimate physical fallback.

## Non-Goals

- Over-the-internet updates / remote hosting (the XIAO is its own AP; you update on-site).
- Cryptographically signed images (WPA2 access control + integrity checksum is sufficient).
- A/B dual-image automatic rollback (arduino-pico uses a single app slot; see Recovery).
- Updating the XIAO itself wirelessly (out of scope; flash it via USB-C).

## Chosen Approach (Approach A)

**UART pipe, RP2040 self-flashes.** The XIAO hosts a WPA2 SoftAP with a web upload page,
receives the `.bin` over Wi-Fi, and streams it to the RP2040 over a 2-wire UART. The
RP2040 feeds the stream into arduino-pico's `Update` (`Update.begin` / `writeStream` /
`Update.end`), which stages the image to LittleFS; on reboot the arduino-pico **stage-3
OTA bootloader** (power-fail-safe, ~12 KB) copies it into application flash.

Rejected alternatives:
- **SPI link** — SPI0 is fully committed to the TFT + MCP2515; speed gain is irrelevant
  for an occasional parked update. More wires, more complexity, no benefit.
- **SWD from the XIAO (external programmer)** — most robust for un-bricking, but no
  turnkey ESP32→RP2040 SWD flasher exists; it means implementing an OpenOCD-style flash
  loader on the XIAO. Disproportionate build effort.
- **XIAO buffers/verifies the whole image before sending** — folded into Approach A as
  "verify checksum on the RP2040 before commit" without requiring XIAO-side buffering.

## Architecture

Two boards, clear responsibilities, one well-defined interface (the UART protocol).

- **RP2040 CAN Feather** — unchanged runtime role (CAN receive + TFT display). Gains:
  (1) an OTA-enabled flash layout, (2) a `Serial1` **update listener** that can enter
  "update mode", receive an image, verify it, and commit it.
- **XIAO ESP32-S3** — no CAN, no display. Runs a **WPA2 SoftAP + HTTP upload page** and
  a **Wi-Fi→UART bridge**. It is otherwise a dumb pipe; the RP2040 owns flashing.

```
[phone/laptop] --WiFi/WPA2--> [XIAO ESP32-S3] --UART (Serial1)--> [RP2040 Feather] --> flash
                              SoftAP + upload page  3 wires        Update.writeStream
```

### Boundaries

- **XIAO firmware:** owns Wi-Fi, the upload page, and the UART framing on its side. Knows
  nothing about RP2040 flash internals.
- **RP2040 update module:** owns UART framing on its side, the update state machine,
  checksum verification, and the arduino-pico `Update` calls. Independent of display/CAN.
- **RP2040 application (existing sketch):** CAN + display. Coupled to the update module
  only by "yield to update mode on handshake".

## Hardware & Wiring

### Signal wiring (3 wires)

| Link          | Feather RP2040        | XIAO ESP32-S3      | Notes                              |
|---------------|-----------------------|--------------------|------------------------------------|
| UART TX → RX  | GP0 (TX, `Serial1`)   | chosen RX GPIO     | 3.3 V logic both sides, no shifter |
| UART RX ← TX  | GP1 (RX, `Serial1`)   | chosen TX GPIO     |                                    |
| Ground        | GND                   | GND                | common reference (required)        |

- GP0/GP1 are free in the current sketch (it uses GP2–6, GP10, GP19, GP22).
- The ESP32-S3 UART maps to almost any GPIO; two free XIAO pins are chosen at build time.
- Handshake is **in-band over UART** (magic-byte command) — no extra wires.

### Power

- Both boards are fed from the **shared 3.3 V buck rail** (car ~13 V → 3.3 V), into each
  board's `3V3` pin (back-feeding the XIAO's 3V3, bypassing its onboard LDO).
- The XIAO's Wi-Fi TX draws ~300–400 mA in short bursts. On a shared rail these bursts can
  dip the voltage and brown out the RP2040 mid-display or mid-flash. **Requirements:**
  - Buck rated for the summed load with margin (~1 A class).
  - **Bulk capacitance** on the rail (~470 µF) plus a **local cap at the XIAO**
    (100 µF + 0.1 µF) to absorb Wi-Fi current spikes.
- **Precondition:** updates are only possible when the buck rail is live. Whether the rail
  is constant or switched (ACC/IGN) is an install choice; the update flow assumes power is
  present for the duration.

### RP2040 flash layout (8 MB)

- arduino-pico OTA stages the image **in LittleFS**, and the stage-3 bootloader copies it
  into app flash on reboot. LittleFS must therefore hold **fonts (~135 KB) + the staged
  image (~1 MB)**.
- Reserve a LittleFS partition of **~2–3 MB** (headroom); remainder to the sketch.
- The exact Arduino IDE **Tools → Flash Size / OTA-enable** setting is pinned down during
  implementation; the mechanism (LittleFS staging + stage-3 bootloader) is fixed.

## Update Protocol / Data Flow

A framed protocol over `Serial1`. Normal operation is untouched until an update is pushed.

**Steady state:** RP2040 runs the CAN/display loop and also listens on `Serial1` for a
handshake. XIAO SoftAP idle/down.

**Update sequence:**
1. Park, join the XIAO **WPA2 AP**, open the upload page, select the `.bin` (+ checksum),
   upload.
2. XIAO validates the file is present, reads its length and expected checksum.
3. XIAO sends `ENTER_UPDATE`. RP2040 acks, **pauses the display loop**, shows an
   "Updating…" screen, stops touching CAN/TFT.
4. XIAO sends a header (image length, checksum), then streams image bytes. RP2040 feeds
   them into `Update.writeStream(Serial1)` (staged to LittleFS).
5. On `Update.end()`, RP2040 **verifies the staged image checksum**:
   - **Mismatch → reject:** discard staged image, report `FAIL`, resume normal display.
     No command file, no reboot.
   - **Match → commit:** write the OTA command file, report `OK`, reboot.
6. Stage-3 bootloader applies the staged image (power-fail-safe) and boots new firmware.

**Flow control & timing:** XIAO paces the stream; RP2040 uses the UART FIFO +
`writeStream` backpressure. Baud ~460k–921k → ~1 MB in a few seconds. An RP2040-side
watchdog/timeout aborts back to normal operation if the stream stalls.

**Auxiliary command — `PING`/version:** the upload page can query the RP2040's currently
running firmware version over UART, to confirm the link is alive and show the current
version before pushing.

## Error Handling & Recovery

### Failure matrix

| When it fails                              | Behavior                                             | Outcome                    |
|--------------------------------------------|------------------------------------------------------|----------------------------|
| Upload/stream stalls or times out          | Watchdog aborts update mode, resumes CAN/display     | Old firmware intact        |
| Checksum mismatch after `writeStream`      | Staged image discarded, `FAIL` to XIAO, no command   | Old firmware intact, no reboot |
| Power lost **during streaming** (staging)  | No command file written yet                          | Boots old firmware, safe   |
| Power lost **during bootloader apply**     | Power-fail-safe bootloader resumes copy next boot    | Completes on next power-up |
| XIAO crashes/reboots mid-transfer          | RP2040 times out → abort to normal                   | Old firmware intact        |

Every failure **before the command file is written** leaves running firmware untouched;
every failure **after** is covered by the power-fail-safe bootloader.

### The one gap: valid-but-broken image

arduino-pico has a single app slot, so there is **no automatic A/B rollback**. A build
that compiles and checksums correctly but crashes on boot would be committed. Defenses:

- **Design rule — update-mode-first boot order.** The RP2040 initializes the `Serial1`
  update listener **before** touching CAN, TFT, or LittleFS fonts. A build that later
  crashes in display/CAN code still accepts a fresh update over the XIAO, so a fix can be
  pushed wirelessly instead of opening the dash. *(Validated by the lockout-recovery test.)*
- **Hard rule — never OTA the bootloader.** The stage-3 bootloader is flashed once via USB
  and never updated over this path, so the one operation that could brick the board is
  never exposed to OTA.
- **Ultimate fallback — USB BOOTSEL.** Physical BOOTSEL + drag-and-drop UF2 un-bricks
  anything. Requires dash access; last resort only.

## Security

- **Access control:** SoftAP is **WPA2-PSK** (password in XIAO config). Only you can join,
  so only you can reach the upload page — this is the primary gate.
- **Integrity:** MD5/CRC of the image is verified on the RP2040 before commit; catches
  corruption. Combined with WPA2 access control this is sufficient (crypto signing was
  deliberately excluded as overkill for a personal car).
- **Transport:** the upload page is plain HTTP over the AP (self-contained, no cert),
  acceptable because WPA2 already encrypts the private-AP link.
- **Attack surface:** the AP is password-protected whenever up. Gating it behind a physical
  "enable update" button is a possible future add, not in scope.

## Testing Strategy

Bench available: real speedometer cluster + an ESP32 replaying recorded CAN via SavvyCAN,
so "does the display still work after an update" is verifiable, not just "did it flash".

### Tier 1 — host-unit-tested pure logic (no hardware)
Factor framing, protocol state machine, and checksum into natively-compilable functions:
- Frame encode/decode: headers, length, command bytes — round-trip + malformed input.
- Checksum compute/verify: known vector passes; single-bit flip fails.
- Update state machine: legal transitions, and every error path lands in `ABORT → resume`.

### Tier 2 — on-hardware bench sequence (build each before the next)
1. **UART bring-up** — loopback/echo; confirms wiring, common ground, baud, no idle brownout.
2. **Handshake + PING/version** — `ENTER_UPDATE`/ack and version query exchange correctly.
3. **Checksum gate** — known image with correct vs corrupted checksum; reject discards and
   does **not** reboot, accept proceeds.
4. **Stage-without-commit** — `writeStream` to LittleFS but skip the command file; staged
   file exists and **old firmware still boots** (non-destructive path proven).
5. **End-to-end happy path** — upload a genuinely different working build (bumped version +
   visible on-screen change) via the page; confirm reboot into the new version (PING +
   on-screen) and that CAN/display still render on the bench.

### Tier 3 — failure injection ("trust it in the car" gates)
- **Corrupt image** → rejected, old firmware resumes.
- **Power cut during streaming** → boots old firmware.
- **Power cut during bootloader apply** → new firmware completes on next power-up
  (deliberately validate the power-fail-safe claim, don't just trust it).
- **Stream stall** (kill the XIAO mid-transfer) → RP2040 times out, resumes.
- **Lockout-recovery** → flash a deliberately broken app (hangs in display init) and confirm
  a good image can **still** be pushed over the XIAO — proves update-mode-first works.

### Tier 4 — rail + security checks
- **Brownout:** monitor the 3.3 V rail during a full flash under Wi-Fi bursting; caps + buck
  headroom must hold it steady.
- **Access control:** AP rejects joins without the WPA2 password; upload page unreachable
  until joined.

**Non-negotiable gates before in-car deployment:** power-cut-during-apply recovery, and
lockout-recovery via update-mode-first.

## Open Implementation Details (to pin down during planning)

- Exact arduino-pico Flash Size / OTA menu selection and LittleFS partition size.
- UART baud rate (target 460k–921k) and framing format (magic bytes, header fields).
- Checksum algorithm (MD5 vs CRC32) and whether the image is GZIP-compressed in transit
  (the stage-3 bootloader can decompress GZIP).
- Chosen XIAO GPIO pins for UART TX/RX.
- Upload page UX (progress, current-version display, success/failure states).
