# RP2040 Wireless OTA Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add permanent in-car wireless firmware updates to the RP2040 CAN display by wiring in a XIAO ESP32-S3 as a Wi-Fi→UART bridge that relays a new image to the RP2040, which self-flashes via arduino-pico OTA.

**Architecture:** A shared, host-testable C++ protocol library (CRC32 + framed control messages + update state machine) is consumed by two firmwares. The XIAO hosts a WPA2 SoftAP + upload page and streams the image over UART; the RP2040 receives it, verifies CRC32, and commits it through the arduino-pico `Update` API, which the power-fail-safe stage-3 bootloader applies on reboot. Pure logic is unit-tested on the host with g++; firmware and integration are verified on the bench (real cluster + SavvyCAN CAN replay).

**Tech Stack:** RP2040 (arduino-pico core, TFT_eSPI, mcp_can, LittleFS, `Update`), XIAO ESP32-S3 (arduino-esp32 core, `WiFi` SoftAP, `WebServer`, PSRAM), shared C++17 library, g++/make host tests, Python 3 bench sender. Built/flashed with the **Arduino IDE 2.x** (no arduino-cli installed).

## Global Constraints

- Spec: `docs/superpowers/specs/2026-07-18-rp2040-wireless-ota-design.md` (authoritative).
- Board that touches CAN stays the **Adafruit Feather RP2040 CAN** — integrated CAN, do not relocate. **No new CAN hardware.**
- Link: **UART, 921600 baud, 8N1**, 3.3 V logic both sides, no level shifter.
- Wiring: XIAO **D6=TX → RP2040 GP1 (RX)**; XIAO **D7=RX ← RP2040 GP0 (TX)**; common **GND**. RP2040 uses `Serial1` (default GP0/GP1). XIAO uses `Serial1` remapped to D6/D7 (keep USB-CDC `Serial` free for debug).
- Integrity: **CRC32 (IEEE 802.3, poly 0xEDB88320, init/final 0xFFFFFFFF)** over the whole image, verified on the RP2040 **before** commit.
- Security: SoftAP is **WPA2-PSK**. Upload page plain HTTP over the AP.
- Sketchbook / libraries path: `/home/honk/mazda/Arduino` (contains `libraries/TFT_eSPI`, `libraries/mcp_can`).
- **Design rule (never violate):** the RP2040 initializes the UART update listener **before** CAN/TFT/fonts (update-mode-first). The stage-3 bootloader is **never** updated over this path.
- Frame wire format (all multi-byte fields little-endian):
  `MAGIC0(0xA5) MAGIC1(0x5A) TYPE(1) LEN_LO LEN_HI PAYLOAD[LEN] CRC32(4, over TYPE..PAYLOAD)`.
  `HEADER` payload = `uint32 img_len` + `uint32 img_crc32`. Data phase = `img_len` **raw** bytes (unframed) after `HEADER` is ACKed.

---

## File Structure

```
docs/superpowers/specs/2026-07-18-rp2040-wireless-ota-design.md   # spec (exists)
docs/superpowers/plans/2026-07-18-rp2040-wireless-ota.md          # this plan

Arduino/libraries/ota_protocol/            # NEW shared library (host-testable, no Arduino deps)
  library.properties
  src/ota_protocol.h
  src/ota_protocol.cpp
  test/check.h
  test/Makefile
  test/test_crc32.cpp
  test/test_frame.cpp
  test/test_state_machine.cpp

Arduino/can_receiver_display/              # EXISTING RP2040 sketch
  can_receiver_display.ino                 # MODIFY: update-mode-first boot + call ota::service()
  ota_update.h                             # NEW: RP2040 update module (interface)
  ota_update.cpp                           # NEW: RP2040 update module (impl)

Arduino/xiao_ota_bridge/                   # NEW XIAO ESP32-S3 sketch
  xiao_ota_bridge.ino                      # SoftAP + upload page + Wi-Fi→UART bridge
  upload_page.h                            # embedded HTML (PROGMEM)
  secrets.h                                # AP SSID/password (gitignored)

tools/
  ota_send.py                              # host bench sender (test RP2040 side over USB-TTL)

.gitignore                                 # ignore build artifacts, secrets.h, test binaries
```

**Responsibilities:** `ota_protocol` owns wire format + CRC + state machine (no I/O, no Arduino). `ota_update` owns the RP2040 UART session + `Update` calls + CRC verify. The `.ino` owns app logic and pausing the display during a session. `xiao_ota_bridge` owns Wi-Fi, the page, and streaming. Files that change together live together.

---

## Task 0: Repo + host test harness scaffolding

**Files:**
- Create: `.gitignore`, `Arduino/libraries/ota_protocol/library.properties`, `Arduino/libraries/ota_protocol/src/ota_protocol.h`, `Arduino/libraries/ota_protocol/src/ota_protocol.cpp`, `Arduino/libraries/ota_protocol/test/check.h`, `Arduino/libraries/ota_protocol/test/Makefile`, `Arduino/libraries/ota_protocol/test/test_smoke.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces: a compiling `otap` namespace stub and a runnable `make` harness that later tasks extend.

- [ ] **Step 1: Initialize git (the repo does not exist yet)**

```bash
cd /home/honk/mazda
git init
git add -A
git commit -m "chore: snapshot existing project before OTA work"
```

Expected: repo created, initial commit with existing files.

- [ ] **Step 2: Add `.gitignore`**

`.gitignore`:
```
# host test binaries
Arduino/libraries/ota_protocol/test/test_crc32
Arduino/libraries/ota_protocol/test/test_frame
Arduino/libraries/ota_protocol/test/test_state_machine
Arduino/libraries/ota_protocol/test/test_smoke
*.o
# secrets
Arduino/xiao_ota_bridge/secrets.h
# large local artifacts
*.AppImage
```

- [ ] **Step 3: Create the library manifest and header stub**

`Arduino/libraries/ota_protocol/library.properties`:
```
name=ota_protocol
version=0.1.0
author=mazda
maintainer=mazda
sentence=Shared UART OTA protocol (CRC32, framing, state machine) for RP2040<->XIAO.
paragraph=No Arduino dependencies; host-testable with g++.
category=Communication
architectures=*
```

`Arduino/libraries/ota_protocol/src/ota_protocol.h`:
```cpp
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace otap {

// ---------- CRC32 (IEEE 802.3) ----------
uint32_t crc32_init();
uint32_t crc32_update(uint32_t crc, const uint8_t* data, size_t len);
uint32_t crc32_final(uint32_t crc);
uint32_t crc32(const uint8_t* data, size_t len);

// ---------- Frame format ----------
static const uint8_t MAGIC0 = 0xA5;
static const uint8_t MAGIC1 = 0x5A;
static const size_t  FRAME_OVERHEAD = 2 /*magic*/ + 1 /*type*/ + 2 /*len*/ + 4 /*crc*/;
static const size_t  MAX_PAYLOAD = 64;

enum class MsgType : uint8_t {
  ENTER_UPDATE = 0x01,
  ACK          = 0x02,
  HEADER       = 0x03,  // payload: u32 img_len (LE) + u32 img_crc32 (LE)
  RESULT       = 0x04,  // payload: 1 byte ResultCode
  PING         = 0x05,
  VERSION      = 0x06,  // payload: version string (no NUL)
  NAK          = 0x07,
};

enum class ResultCode : uint8_t {
  OK           = 0x00,
  CRC_MISMATCH = 0x01,
  FLASH_ERROR  = 0x02,
  TIMEOUT      = 0x03,
  BAD_LENGTH   = 0x04,
};

// Encode a control frame. Returns total bytes written, or 0 if payload too big
// or out_cap too small.
size_t frame_encode(MsgType type, const uint8_t* payload, size_t payload_len,
                    uint8_t* out, size_t out_cap);

enum class ParseStatus : uint8_t { NEED_MORE, FRAME_READY, BAD_CRC };

struct ParsedFrame {
  MsgType type;
  uint8_t payload[MAX_PAYLOAD];
  size_t  payload_len;
};

struct FrameParser {
  uint8_t buf[FRAME_OVERHEAD + MAX_PAYLOAD];
  size_t  have;
};

void        frameparser_reset(FrameParser* p);
// Feed one received byte. Returns FRAME_READY (fills out) once a valid frame is
// complete, BAD_CRC if a fully-received frame fails its CRC (parser auto-resets),
// else NEED_MORE. Resynchronizes on bad magic.
ParseStatus frameparser_push(FrameParser* p, uint8_t byte, ParsedFrame* out);

// Convenience payload (de)serialization for HEADER.
void header_encode(uint32_t img_len, uint32_t img_crc32, uint8_t out[8]);
void header_decode(const uint8_t in[8], uint32_t* img_len, uint32_t* img_crc32);

// ---------- Update state machine (pure) ----------
enum class State  : uint8_t { IDLE, WAIT_HEADER, RECEIVING, VERIFYING, DONE, ABORTED };
enum class Event  : uint8_t { RX_ENTER, RX_HEADER, DATA_COMPLETE, CRC_OK, CRC_FAIL,
                              TIMEOUT, FLASH_FAIL, RESET };
enum class Action : uint8_t { NONE, SEND_ACK, BEGIN_FLASH, COMMIT, ABORT_RESUME };

struct Transition { State next; Action action; };
Transition sm_step(State s, Event e);

} // namespace otap
```

`Arduino/libraries/ota_protocol/src/ota_protocol.cpp` (stub — real bodies added in Tasks 1–3):
```cpp
#include "ota_protocol.h"
namespace otap {
uint32_t crc32_init() { return 0xFFFFFFFFu; }
uint32_t crc32_update(uint32_t crc, const uint8_t*, size_t) { return crc; }
uint32_t crc32_final(uint32_t crc) { return crc ^ 0xFFFFFFFFu; }
uint32_t crc32(const uint8_t* d, size_t n) { return crc32_final(crc32_update(crc32_init(), d, n)); }
size_t frame_encode(MsgType, const uint8_t*, size_t, uint8_t*, size_t) { return 0; }
void frameparser_reset(FrameParser* p) { p->have = 0; }
ParseStatus frameparser_push(FrameParser*, uint8_t, ParsedFrame*) { return ParseStatus::NEED_MORE; }
void header_encode(uint32_t, uint32_t, uint8_t*) {}
void header_decode(const uint8_t*, uint32_t*, uint32_t*) {}
Transition sm_step(State s, Event) { return { s, Action::NONE }; }
} // namespace otap
```

- [ ] **Step 4: Create the test harness**

`Arduino/libraries/ota_protocol/test/check.h`:
```cpp
#pragma once
#include <cstdio>
static int g_checks = 0, g_fails = 0;
#define CHECK(cond) do { g_checks++; if(!(cond)){ g_fails++; \
  std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);} } while(0)
#define CHECK_EQ(a,b) do { g_checks++; auto _a=(a); auto _b=(b); if(!(_a==_b)){ g_fails++; \
  std::printf("FAIL %s:%d: (%s) == (%s)\n", __FILE__, __LINE__, #a, #b);} } while(0)
#define RUN(fn) do { std::printf("== %s\n", #fn); fn(); } while(0)
#define REPORT() do { std::printf("%d checks, %d failures\n", g_checks, g_fails); \
  return g_fails ? 1 : 0; } while(0)
```

`Arduino/libraries/ota_protocol/test/Makefile`:
```make
CXX ?= g++
CXXFLAGS = -std=c++17 -Wall -Wextra -I../src
SRC = ../src/ota_protocol.cpp
TESTS = test_smoke test_crc32 test_frame test_state_machine

all: run

run: $(TESTS)
	@for t in $(TESTS); do echo "== running $$t =="; ./$$t || exit 1; done

test_%: test_%.cpp $(SRC) ../src/ota_protocol.h check.h
	$(CXX) $(CXXFLAGS) -o $@ $< $(SRC)

clean:
	rm -f $(TESTS)
.PHONY: all run clean
```

`Arduino/libraries/ota_protocol/test/test_smoke.cpp`:
```cpp
#include "check.h"
#include "ota_protocol.h"
int main() {
  // crc32 of empty input is 0 by convention
  CHECK_EQ(otap::crc32(nullptr, 0), 0x00000000u);
  REPORT();
}
```

- [ ] **Step 5: Build and run the harness**

Run:
```bash
cd /home/honk/mazda/Arduino/libraries/ota_protocol/test && make test_smoke && ./test_smoke
```
Expected: compiles clean, prints `1 checks, 0 failures`, exit 0.

- [ ] **Step 6: Commit**

```bash
cd /home/honk/mazda
git add Arduino/libraries/ota_protocol .gitignore
git commit -m "chore: scaffold ota_protocol library + host test harness"
```

---

## Task 1: CRC32

**Files:**
- Modify: `Arduino/libraries/ota_protocol/src/ota_protocol.cpp` (crc32 bodies)
- Test: `Arduino/libraries/ota_protocol/test/test_crc32.cpp`

**Interfaces:**
- Consumes: `otap::crc32_init/update/final/crc32` declarations from Task 0.
- Produces: correct CRC32 usable by frame codec (Task 2) and both firmwares.

- [ ] **Step 1: Write the failing test**

`Arduino/libraries/ota_protocol/test/test_crc32.cpp`:
```cpp
#include "check.h"
#include "ota_protocol.h"
#include <cstring>

static void test_known_vectors() {
  const char* s = "123456789";
  CHECK_EQ(otap::crc32((const uint8_t*)s, 9), 0xCBF43926u); // canonical CRC-32/ISO-HDLC
  CHECK_EQ(otap::crc32(nullptr, 0), 0x00000000u);
  const char* a = "a";
  CHECK_EQ(otap::crc32((const uint8_t*)a, 1), 0xE8B7BE43u);
}

static void test_incremental_matches_oneshot() {
  const char* s = "The quick brown fox";
  size_t n = std::strlen(s);
  uint32_t c = otap::crc32_init();
  c = otap::crc32_update(c, (const uint8_t*)s, 5);
  c = otap::crc32_update(c, (const uint8_t*)s + 5, n - 5);
  CHECK_EQ(otap::crc32_final(c), otap::crc32((const uint8_t*)s, n));
}

int main() {
  RUN(test_known_vectors);
  RUN(test_incremental_matches_oneshot);
  REPORT();
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd /home/honk/mazda/Arduino/libraries/ota_protocol/test && make test_crc32 && ./test_crc32`
Expected: FAIL on the known-vector checks (stub returns wrong values).

- [ ] **Step 3: Implement CRC32 (replace the stub bodies)**

In `ota_protocol.cpp`, replace the three crc32_* stubs:
```cpp
uint32_t crc32_init() { return 0xFFFFFFFFu; }

uint32_t crc32_update(uint32_t crc, const uint8_t* data, size_t len) {
  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (int b = 0; b < 8; ++b)
      crc = (crc >> 1) ^ (0xEDB88320u & (~((crc & 1u) - 1u)));
  }
  return crc;
}

uint32_t crc32_final(uint32_t crc) { return crc ^ 0xFFFFFFFFu; }

uint32_t crc32(const uint8_t* d, size_t n) {
  return crc32_final(crc32_update(crc32_init(), d, n));
}
```
(Leave `crc32` as-is if identical; keep the convenience overload from the stub.)

- [ ] **Step 4: Run test to verify it passes**

Run: `make test_crc32 && ./test_crc32`
Expected: PASS, `... 0 failures`.

- [ ] **Step 5: Commit**

```bash
cd /home/honk/mazda
git add Arduino/libraries/ota_protocol
git commit -m "feat(ota_protocol): implement CRC32 with tests"
```

---

## Task 2: Control-frame codec + HEADER helpers

**Files:**
- Modify: `Arduino/libraries/ota_protocol/src/ota_protocol.cpp` (frame_encode, parser, header helpers)
- Test: `Arduino/libraries/ota_protocol/test/test_frame.cpp`

**Interfaces:**
- Consumes: `crc32*` (Task 1); `MsgType`, `FrameParser`, `ParsedFrame`, `ParseStatus`, constants (Task 0).
- Produces: `frame_encode`, `frameparser_reset`, `frameparser_push`, `header_encode`, `header_decode` used by `ota_update` (Task 5) and the XIAO (Task 8).

- [ ] **Step 1: Write the failing test**

`Arduino/libraries/ota_protocol/test/test_frame.cpp`:
```cpp
#include "check.h"
#include "ota_protocol.h"
using namespace otap;

// Push every byte of `buf` and return the status of the last byte.
static ParseStatus feed(FrameParser* p, const uint8_t* buf, size_t n, ParsedFrame* out) {
  ParseStatus st = ParseStatus::NEED_MORE;
  for (size_t i = 0; i < n; ++i) st = frameparser_push(p, buf[i], out);
  return st;
}

static void test_roundtrip_no_payload() {
  uint8_t wire[64];
  size_t n = frame_encode(MsgType::PING, nullptr, 0, wire, sizeof(wire));
  CHECK_EQ(n, FRAME_OVERHEAD);
  FrameParser p; frameparser_reset(&p);
  ParsedFrame f;
  CHECK_EQ((int)feed(&p, wire, n, &f), (int)ParseStatus::FRAME_READY);
  CHECK_EQ((int)f.type, (int)MsgType::PING);
  CHECK_EQ(f.payload_len, 0u);
}

static void test_roundtrip_with_payload() {
  uint8_t hdr[8]; header_encode(0x00100000u, 0xCBF43926u, hdr);
  uint8_t wire[64];
  size_t n = frame_encode(MsgType::HEADER, hdr, 8, wire, sizeof(wire));
  CHECK_EQ(n, FRAME_OVERHEAD + 8);
  FrameParser p; frameparser_reset(&p);
  ParsedFrame f;
  CHECK_EQ((int)feed(&p, wire, n, &f), (int)ParseStatus::FRAME_READY);
  CHECK_EQ((int)f.type, (int)MsgType::HEADER);
  CHECK_EQ(f.payload_len, 8u);
  uint32_t len, crc; header_decode(f.payload, &len, &crc);
  CHECK_EQ(len, 0x00100000u);
  CHECK_EQ(crc, 0xCBF43926u);
}

static void test_bad_crc_rejected() {
  uint8_t wire[64];
  size_t n = frame_encode(MsgType::ACK, nullptr, 0, wire, sizeof(wire));
  wire[2] = (uint8_t)MsgType::NAK; // corrupt the type after CRC computed
  FrameParser p; frameparser_reset(&p);
  ParsedFrame f;
  CHECK_EQ((int)feed(&p, wire, n, &f), (int)ParseStatus::BAD_CRC);
}

static void test_resync_on_leading_garbage() {
  uint8_t wire[64];
  size_t n = frame_encode(MsgType::PING, nullptr, 0, wire, sizeof(wire));
  FrameParser p; frameparser_reset(&p);
  ParsedFrame f;
  frameparser_push(&p, 0x00, &f);
  frameparser_push(&p, 0xFF, &f);
  frameparser_push(&p, 0xA5, &f); // stray magic0 then non-magic1
  frameparser_push(&p, 0x11, &f);
  CHECK_EQ((int)feed(&p, wire, n, &f), (int)ParseStatus::FRAME_READY);
  CHECK_EQ((int)f.type, (int)MsgType::PING);
}

static void test_encode_rejects_oversize() {
  uint8_t big[MAX_PAYLOAD + 1] = {0};
  uint8_t wire[128];
  CHECK_EQ(frame_encode(MsgType::VERSION, big, sizeof(big), wire, sizeof(wire)), 0u);
}

int main() {
  RUN(test_roundtrip_no_payload);
  RUN(test_roundtrip_with_payload);
  RUN(test_bad_crc_rejected);
  RUN(test_resync_on_leading_garbage);
  RUN(test_encode_rejects_oversize);
  REPORT();
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd /home/honk/mazda/Arduino/libraries/ota_protocol/test && make test_frame && ./test_frame`
Expected: FAIL (stubs return 0 / NEED_MORE).

- [ ] **Step 3: Implement the codec (replace stubs)**

In `ota_protocol.cpp`, replace the frame/header stubs:
```cpp
static void put_u32(uint8_t* p, uint32_t v) {
  p[0]=v&0xFF; p[1]=(v>>8)&0xFF; p[2]=(v>>16)&0xFF; p[3]=(v>>24)&0xFF;
}
static uint32_t get_u32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
}

void header_encode(uint32_t img_len, uint32_t img_crc32, uint8_t out[8]) {
  put_u32(out, img_len); put_u32(out + 4, img_crc32);
}
void header_decode(const uint8_t in[8], uint32_t* img_len, uint32_t* img_crc32) {
  *img_len = get_u32(in); *img_crc32 = get_u32(in + 4);
}

size_t frame_encode(MsgType type, const uint8_t* payload, size_t payload_len,
                    uint8_t* out, size_t out_cap) {
  if (payload_len > MAX_PAYLOAD) return 0;
  size_t total = FRAME_OVERHEAD + payload_len;
  if (out_cap < total) return 0;
  out[0] = MAGIC0; out[1] = MAGIC1;
  out[2] = (uint8_t)type;
  out[3] = (uint8_t)(payload_len & 0xFF);
  out[4] = (uint8_t)((payload_len >> 8) & 0xFF);
  for (size_t i = 0; i < payload_len; ++i) out[5 + i] = payload[i];
  // CRC over TYPE..PAYLOAD (bytes [2 .. 5+payload_len))
  uint32_t c = crc32(out + 2, 3 + payload_len);
  put_u32(out + 5 + payload_len, c);
  return total;
}

void frameparser_reset(FrameParser* p) { p->have = 0; }

ParseStatus frameparser_push(FrameParser* p, uint8_t byte, ParsedFrame* out) {
  // Assemble into p->buf with resync on magic.
  if (p->have == 0) { if (byte != MAGIC0) return ParseStatus::NEED_MORE; }
  else if (p->have == 1) {
    if (byte != MAGIC1) { p->have = (byte == MAGIC0) ? 1 : 0; if (p->have==1) p->buf[0]=MAGIC0; return ParseStatus::NEED_MORE; }
  }
  if (p->have >= sizeof(p->buf)) p->have = 0; // overflow guard: drop
  p->buf[p->have++] = byte;

  if (p->have < 5) return ParseStatus::NEED_MORE;           // need len field
  size_t payload_len = (size_t)p->buf[3] | ((size_t)p->buf[4] << 8);
  if (payload_len > MAX_PAYLOAD) { frameparser_reset(p); return ParseStatus::NEED_MORE; }
  size_t total = FRAME_OVERHEAD + payload_len;
  if (p->have < total) return ParseStatus::NEED_MORE;

  uint32_t want = get_u32(p->buf + 5 + payload_len);
  uint32_t got  = crc32(p->buf + 2, 3 + payload_len);
  if (want != got) { frameparser_reset(p); return ParseStatus::BAD_CRC; }

  out->type = (MsgType)p->buf[2];
  out->payload_len = payload_len;
  for (size_t i = 0; i < payload_len; ++i) out->payload[i] = p->buf[5 + i];
  frameparser_reset(p);
  return ParseStatus::FRAME_READY;
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `make test_frame && ./test_frame`
Expected: PASS, `0 failures`.

- [ ] **Step 5: Commit**

```bash
cd /home/honk/mazda
git add Arduino/libraries/ota_protocol
git commit -m "feat(ota_protocol): frame codec + HEADER helpers with tests"
```

---

## Task 3: Update state machine

**Files:**
- Modify: `Arduino/libraries/ota_protocol/src/ota_protocol.cpp` (`sm_step`)
- Test: `Arduino/libraries/ota_protocol/test/test_state_machine.cpp`

**Interfaces:**
- Consumes: `State`, `Event`, `Action`, `Transition`, `sm_step` (Task 0).
- Produces: `sm_step` used by `ota_update` (Task 5) to drive session logic.

- [ ] **Step 1: Write the failing test**

`Arduino/libraries/ota_protocol/test/test_state_machine.cpp`:
```cpp
#include "check.h"
#include "ota_protocol.h"
using namespace otap;

static void test_happy_path() {
  Transition t;
  t = sm_step(State::IDLE, Event::RX_ENTER);
  CHECK_EQ((int)t.next, (int)State::WAIT_HEADER); CHECK_EQ((int)t.action, (int)Action::SEND_ACK);
  t = sm_step(State::WAIT_HEADER, Event::RX_HEADER);
  CHECK_EQ((int)t.next, (int)State::RECEIVING);   CHECK_EQ((int)t.action, (int)Action::BEGIN_FLASH);
  t = sm_step(State::RECEIVING, Event::DATA_COMPLETE);
  CHECK_EQ((int)t.next, (int)State::VERIFYING);   CHECK_EQ((int)t.action, (int)Action::NONE);
  t = sm_step(State::VERIFYING, Event::CRC_OK);
  CHECK_EQ((int)t.next, (int)State::DONE);        CHECK_EQ((int)t.action, (int)Action::COMMIT);
}

static void test_crc_fail_aborts() {
  Transition t = sm_step(State::VERIFYING, Event::CRC_FAIL);
  CHECK_EQ((int)t.next, (int)State::ABORTED);     CHECK_EQ((int)t.action, (int)Action::ABORT_RESUME);
}

static void test_timeout_and_flashfail_abort_from_any_active_state() {
  State actives[] = { State::WAIT_HEADER, State::RECEIVING, State::VERIFYING };
  for (State s : actives) {
    Transition t1 = sm_step(s, Event::TIMEOUT);
    CHECK_EQ((int)t1.next, (int)State::ABORTED); CHECK_EQ((int)t1.action, (int)Action::ABORT_RESUME);
    Transition t2 = sm_step(s, Event::FLASH_FAIL);
    CHECK_EQ((int)t2.next, (int)State::ABORTED); CHECK_EQ((int)t2.action, (int)Action::ABORT_RESUME);
  }
}

static void test_unexpected_event_aborts() {
  // HEADER before ENTER, DATA_COMPLETE before header, etc.
  Transition t = sm_step(State::IDLE, Event::RX_HEADER);
  CHECK_EQ((int)t.next, (int)State::ABORTED); CHECK_EQ((int)t.action, (int)Action::ABORT_RESUME);
}

static void test_reset_returns_to_idle() {
  CHECK_EQ((int)sm_step(State::ABORTED, Event::RESET).next, (int)State::IDLE);
  CHECK_EQ((int)sm_step(State::DONE, Event::RESET).next, (int)State::IDLE);
  CHECK_EQ((int)sm_step(State::RECEIVING, Event::RESET).next, (int)State::IDLE);
}

int main() {
  RUN(test_happy_path);
  RUN(test_crc_fail_aborts);
  RUN(test_timeout_and_flashfail_abort_from_any_active_state);
  RUN(test_unexpected_event_aborts);
  RUN(test_reset_returns_to_idle);
  REPORT();
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd /home/honk/mazda/Arduino/libraries/ota_protocol/test && make test_state_machine && ./test_state_machine`
Expected: FAIL (stub returns `{s, NONE}`).

- [ ] **Step 3: Implement `sm_step` (replace stub)**

```cpp
Transition sm_step(State s, Event e) {
  if (e == Event::RESET)      return { State::IDLE,    Action::NONE };
  if (e == Event::TIMEOUT ||
      e == Event::FLASH_FAIL) return { State::ABORTED, Action::ABORT_RESUME };

  switch (s) {
    case State::IDLE:
      if (e == Event::RX_ENTER)      return { State::WAIT_HEADER, Action::SEND_ACK };
      break;
    case State::WAIT_HEADER:
      if (e == Event::RX_HEADER)     return { State::RECEIVING,   Action::BEGIN_FLASH };
      break;
    case State::RECEIVING:
      if (e == Event::DATA_COMPLETE) return { State::VERIFYING,   Action::NONE };
      break;
    case State::VERIFYING:
      if (e == Event::CRC_OK)        return { State::DONE,        Action::COMMIT };
      if (e == Event::CRC_FAIL)      return { State::ABORTED,     Action::ABORT_RESUME };
      break;
    default: break;
  }
  return { State::ABORTED, Action::ABORT_RESUME }; // any unexpected event
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `make test_state_machine && ./test_state_machine`
Expected: PASS, `0 failures`.

- [ ] **Step 5: Run the full suite**

Run: `make clean && make`
Expected: all four test binaries build and print `0 failures`.

- [ ] **Step 6: Commit**

```bash
cd /home/honk/mazda
git add Arduino/libraries/ota_protocol
git commit -m "feat(ota_protocol): update state machine with tests"
```

---

## Task 4: RP2040 OTA flash layout + regression baseline

**Files:**
- Modify (IDE settings, no source change): `Arduino/can_receiver_display/can_receiver_display.ino` gets a one-line version marker only.

**Interfaces:**
- Consumes: existing sketch.
- Produces: an OTA-capable flash layout with a LittleFS partition large enough for fonts (~135 KB) + a staged image (~1 MB), verified to still boot and load fonts.

- [ ] **Step 1: Add a version marker to the sketch**

Near the top of `can_receiver_display.ino`, add:
```cpp
#define FW_VERSION "2026.07.18-base"
```
(Used later by the update module's VERSION reply and to prove an update changed the running build.)

- [ ] **Step 2: Confirm the installed arduino-pico core supports OTA**

In the Arduino IDE, Boards Manager → confirm **"Raspberry Pi Pico/RP2040" by Earle F. Philhower** is installed (this core provides the stage-3 OTA bootloader and `Update`). Open `File → Examples → Updater` (or the core's `OTA` example) to confirm the `Update` API is present.
Expected: core installed; `Update` example compiles for the board.

- [ ] **Step 3: Set an OTA + LittleFS flash layout**

In `Tools`:
- Board: **Adafruit Feather RP2040** (as currently used).
- **Flash Size:** choose a scheme that reserves both program space and a **≥2 MB FS** partition (e.g. "8MB (Sketch: ~6MB, FS: 2MB)" or the closest available; the Feather RP2040 has 8 MB). The FS must hold fonts + staged image.
- Confirm any "OTA" / bootloader option required by the core for `Update` is enabled per the `Updater` example's documented settings.

- [ ] **Step 4: Re-flash fonts to the new FS partition**

Because the FS partition changed, re-upload the fonts per `CLAUDE.md`:
close Serial Monitor → `Ctrl+Shift+P` → "Upload LittleFS to Pico/RP2040...".
Expected: upload succeeds.

- [ ] **Step 5: Build, upload, and verify regression**

Upload the (unchanged-behavior) sketch. On the bench with CAN replay running:
Expected on Serial: `All fonts are there!` at boot; speed/gear/brightness render exactly as before. **No behavior change** — this task only re-partitions.

- [ ] **Step 6: Commit**

```bash
cd /home/honk/mazda
git add Arduino/can_receiver_display/can_receiver_display.ino
git commit -m "chore(rp2040): add FW_VERSION marker; document OTA+FS flash layout"
```

Note in the commit body the exact Flash Size menu string chosen (settings aren't in source; record them for reproducibility).

---

## Task 5: RP2040 update module (`ota_update`) + host bench sender

**Files:**
- Create: `Arduino/can_receiver_display/ota_update.h`, `Arduino/can_receiver_display/ota_update.cpp`, `tools/ota_send.py`

**Interfaces:**
- Consumes: `otap::*` (Tasks 1–3); arduino-pico `Update`, `Serial1`, `rp2040.reboot()`.
- Produces:
  - `void ota::begin(const char* fw_version, unsigned long baud = 921600);`
  - `void ota::service(ota::UiCallback cb);` — poll once per loop; runs a full session when `ENTER_UPDATE` arrives; on success it reboots (does not return).
  - `enum class ota::Ui { STARTED, PROGRESS, SUCCESS, FAILED };`
  - `typedef void (*ota::UiCallback)(ota::Ui phase, uint8_t pct, const char* msg);`

- [ ] **Step 1: Confirm the arduino-pico `Update` API surface**

Open the core's `Updater`/OTA header and the `Updater`/`OTA` example. Confirm these exist and note exact signatures/return types before implementing:
`Update.begin(size_t)`, `Update.write(uint8_t*, size_t)`, `Update.end()` (writes the OTA command file), and the reboot call (`rp2040.reboot()`).
Expected: signatures confirmed. If a name differs in the installed version, adapt the calls in Step 3 to match (behavior identical).

- [ ] **Step 2: Write the module interface**

`Arduino/can_receiver_display/ota_update.h`:
```cpp
#pragma once
#include <stdint.h>

namespace ota {
  enum class Ui : uint8_t { STARTED, PROGRESS, SUCCESS, FAILED };
  typedef void (*UiCallback)(Ui phase, uint8_t pct, const char* msg);

  // Initialize the UART listener. Call this FIRST in setup(), before CAN/TFT/fonts.
  void begin(const char* fw_version, unsigned long baud = 921600);

  // Poll once per loop(). Non-blocking unless a session is running (during which
  // it blocks to receive the image while the caller has paused the display).
  // On a committed update it reboots and does not return.
  void service(UiCallback cb);
}
```

- [ ] **Step 3: Write the module implementation**

`Arduino/can_receiver_display/ota_update.cpp`:
```cpp
#include "ota_update.h"
#include <Arduino.h>
#include <Updater.h>          // arduino-pico OTA Updater (confirm header name in Step 1)
#include <ota_protocol.h>

using namespace otap;

namespace {
  const char*    g_ver = "?";
  FrameParser    g_parser;

  const uint32_t BYTE_TIMEOUT_MS  = 3000;   // stall detection during data phase
  const uint32_t FRAME_TIMEOUT_MS = 5000;   // waiting for HEADER after ACK

  void send_frame(MsgType t, const uint8_t* payload, size_t len) {
    uint8_t wire[FRAME_OVERHEAD + MAX_PAYLOAD];
    size_t n = frame_encode(t, payload, len, wire, sizeof(wire));
    if (n) Serial1.write(wire, n);
  }
  void send_result(ResultCode rc) { uint8_t b = (uint8_t)rc; send_frame(MsgType::RESULT, &b, 1); }

  // Read exactly n bytes into buf with a per-read timeout. Returns bytes read.
  size_t read_exact(uint8_t* buf, size_t n, uint32_t timeout_ms) {
    size_t got = 0; uint32_t last = millis();
    while (got < n) {
      if (Serial1.available()) { buf[got++] = (uint8_t)Serial1.read(); last = millis(); }
      else if (millis() - last > timeout_ms) break;
    }
    return got;
  }

  // Blocking data phase: receive img_len raw bytes, stage via Update.write, compute CRC.
  // Returns true if the full image was staged AND crc matched.
  bool receive_and_stage(uint32_t img_len, uint32_t img_crc, ota::UiCallback cb) {
    if (!Update.begin(img_len)) { send_result(ResultCode::FLASH_ERROR); return false; }
    uint32_t crc = crc32_init();
    uint32_t remaining = img_len;
    uint8_t  buf[512];
    uint8_t  last_pct = 255;
    while (remaining) {
      size_t chunk = remaining < sizeof(buf) ? remaining : sizeof(buf);
      size_t got = read_exact(buf, chunk, BYTE_TIMEOUT_MS);
      if (got == 0) { send_result(ResultCode::TIMEOUT); return false; }   // stall
      if (Update.write(buf, got) != got) { send_result(ResultCode::FLASH_ERROR); return false; }
      crc = crc32_update(crc, buf, got);
      remaining -= got;
      uint8_t pct = (uint8_t)(100u * (img_len - remaining) / img_len);
      if (cb && pct != last_pct) { cb(ota::Ui::PROGRESS, pct, "Updating"); last_pct = pct; }
    }
    if (crc32_final(crc) != img_crc) { send_result(ResultCode::CRC_MISMATCH); return false; }
    return true;
  }

  // Runs after an ENTER_UPDATE frame. Drives header -> data -> verify -> commit.
  void run_session(ota::UiCallback cb) {
    State st = State::IDLE;
    Transition t = sm_step(st, Event::RX_ENTER);   // -> WAIT_HEADER, SEND_ACK
    st = t.next;
    if (t.action == Action::SEND_ACK) send_frame(MsgType::ACK, nullptr, 0);
    if (cb) cb(ota::Ui::STARTED, 0, "Updating");

    // Wait for HEADER frame.
    uint32_t start = millis(); ParsedFrame f; bool got_header = false;
    while (millis() - start < FRAME_TIMEOUT_MS) {
      if (!Serial1.available()) continue;
      if (frameparser_push(&g_parser, (uint8_t)Serial1.read(), &f) == ParseStatus::FRAME_READY
          && f.type == MsgType::HEADER && f.payload_len == 8) { got_header = true; break; }
    }
    if (!got_header) { send_result(ResultCode::TIMEOUT); if (cb) cb(ota::Ui::FAILED, 0, "No header"); return; }

    uint32_t img_len, img_crc; header_decode(f.payload, &img_len, &img_crc);
    t = sm_step(st, Event::RX_HEADER); st = t.next;    // -> RECEIVING, BEGIN_FLASH

    bool ok = receive_and_stage(img_len, img_crc, cb);
    t = sm_step(st, ok ? Event::DATA_COMPLETE : Event::CRC_FAIL);   // best-effort; result already sent on failure
    if (!ok) { if (cb) cb(ota::Ui::FAILED, 0, "Rejected"); return; }

    st = t.next;                                       // VERIFYING
    t = sm_step(st, Event::CRC_OK);                    // -> DONE, COMMIT
    if (t.action == Action::COMMIT) {
      if (Update.end()) {                              // writes OTA command file
        send_result(ResultCode::OK);
        if (cb) cb(ota::Ui::SUCCESS, 100, "Rebooting");
        Serial1.flush();
        delay(200);
        rp2040.reboot();                               // bootloader applies staged image
      } else {
        send_result(ResultCode::FLASH_ERROR);
        if (cb) cb(ota::Ui::FAILED, 0, "Commit failed");
      }
    }
  }
} // namespace

namespace ota {

void begin(const char* fw_version, unsigned long baud) {
  g_ver = fw_version ? fw_version : "?";
  Serial1.begin(baud);
  frameparser_reset(&g_parser);
}

void service(UiCallback cb) {
  ParsedFrame f;
  while (Serial1.available()) {
    ParseStatus s = frameparser_push(&g_parser, (uint8_t)Serial1.read(), &f);
    if (s != ParseStatus::FRAME_READY) continue;
    switch (f.type) {
      case MsgType::PING:
        send_frame(MsgType::ACK, nullptr, 0);
        break;
      case MsgType::VERSION: // request-for-version: reply with our version string
        send_frame(MsgType::VERSION, (const uint8_t*)g_ver, strlen(g_ver));
        break;
      case MsgType::ENTER_UPDATE:
        run_session(cb);     // blocks through the session; reboots on success
        frameparser_reset(&g_parser);
        return;
      default:
        break;
    }
  }
}

} // namespace ota
```
Notes for the implementer:
- `Serial1` on the Feather RP2040 is GP0(TX)/GP1(RX) by default — no `setTX/setRX` needed.
- If `Update.end()` in the installed core auto-reboots, the explicit `rp2040.reboot()` is harmless (unreachable). If `begin/write/end` names differ, adapt per Step 1 — semantics are identical.
- On any failure the module returns without writing the OTA command file, so the running firmware is unchanged (matches the spec's failure matrix).

- [ ] **Step 4: Write the host bench sender**

`tools/ota_send.py`:
```python
#!/usr/bin/env python3
"""Bench sender: exercise the RP2040 ota_update module over a USB-TTL adapter.
Usage: ota_send.py /dev/ttyUSB0 firmware.bin
Wiring: adapter TX -> RP2040 GP1(RX), adapter RX -> RP2040 GP0(TX), GND-GND, 3.3V logic."""
import sys, time, struct, zlib
import serial  # pip install pyserial

MAGIC = b"\xA5\x5A"
ENTER, ACK, HEADER, RESULT, PING, VERSION, NAK = 1, 2, 3, 4, 5, 6, 7

def crc32(b): return zlib.crc32(b) & 0xFFFFFFFF

def frame(t, payload=b""):
    body = bytes([t]) + struct.pack("<H", len(payload)) + payload
    return MAGIC + body + struct.pack("<I", crc32(body))

def read_frame(ser, timeout=6.0):
    ser.timeout = timeout; buf = bytearray()
    # naive: read a chunk and scan for one frame
    end = time.time() + timeout
    while time.time() < end:
        b = ser.read(1)
        if not b: continue
        buf += b
        i = buf.find(MAGIC)
        if i < 0 or len(buf) - i < 5: continue
        ln = struct.unpack("<H", buf[i+3:i+5])[0]
        need = i + 5 + ln + 4
        if len(buf) < need: continue
        body = buf[i+2:i+5+ln]; crc = struct.unpack("<I", buf[i+5+ln:need])[0]
        if crc32(body) == crc: return body[0], bytes(body[3:])
        del buf[:i+2]
    return None, None

def main():
    port, path = sys.argv[1], sys.argv[2]
    img = open(path, "rb").read()
    ser = serial.Serial(port, 921600, timeout=1)
    time.sleep(0.2)

    ser.write(frame(PING)); t,_ = read_frame(ser); print("PING ->", t)
    ser.write(frame(ENTER)); t,_ = read_frame(ser)
    assert t == ACK, f"expected ACK, got {t}"
    ser.write(frame(HEADER, struct.pack("<II", len(img), crc32(img))))
    print(f"streaming {len(img)} bytes, crc={crc32(img):08X}")
    ser.write(img); ser.flush()
    t, p = read_frame(ser, timeout=30)
    print("RESULT ->", t, "code", p[0] if p else None)  # 0 == OK

if __name__ == "__main__":
    main()
```

- [ ] **Step 5: Bench-test the RP2040 module (isolated, before the XIAO exists)**

Prep: temporarily add the module to the sketch so it runs. In `setup()` add `ota::begin(FW_VERSION);` as the **first** line, and at the top of `loop()` add `ota::service(nullptr);`. Wire a USB-TTL adapter to GP0/GP1/GND (3.3 V). Build a distinct test image (bump `FW_VERSION` to `...-t5`, change one on-screen pixel/colour), export its `.bin` (Arduino IDE `Sketch → Export Compiled Binary`).

Run:
```bash
pip install pyserial
python3 tools/ota_send.py /dev/ttyUSB0 path/to/exported.bin
```
Expected: `PING -> 2` (ACK), `RESULT -> 4 code 0` (OK), the RP2040 reboots, and on the bench the new build's visible change appears / Serial shows `...-t5`.
Also verify the reject path: corrupt the image (`printf '\x00' | dd ... ` a byte) and resend → `RESULT` code `1` (CRC_MISMATCH), **no reboot**, old firmware keeps running.

- [ ] **Step 6: Commit**

```bash
cd /home/honk/mazda
git add Arduino/can_receiver_display/ota_update.h Arduino/can_receiver_display/ota_update.cpp tools/ota_send.py
git commit -m "feat(rp2040): UART OTA update module + host bench sender"
```

---

## Task 6: Integrate update-mode-first into the existing sketch

**Files:**
- Modify: `Arduino/can_receiver_display/can_receiver_display.ino`

**Interfaces:**
- Consumes: `ota::begin`, `ota::service`, `ota::Ui` (Task 5); existing `tft`.
- Produces: production wiring of the update path into the running app, with a TFT "Updating…" screen.

- [ ] **Step 1: Include the module and add the UI callback**

At the top of `can_receiver_display.ino` (after `#include <TFT_eSPI.h>`), add:
```cpp
#include "ota_update.h"
```
After `TFT_eSPI tft = TFT_eSPI();` add the callback that draws update status to the screen:
```cpp
static void otaUi(ota::Ui phase, uint8_t pct, const char* msg) {
  switch (phase) {
    case ota::Ui::STARTED:
      tft.fillScreen(TFT_BLACK);
      tft.setTextColor(TFT_WHITE, TFT_BLACK);
      tft.setTextDatum(MC_DATUM);
      tft.drawString("Updating...", tft.width()/2, tft.height()/2 - 20, 4);
      break;
    case ota::Ui::PROGRESS: {
      int w = tft.width() - 40, x = 20, y = tft.height()/2 + 10;
      tft.drawRect(x, y, w, 16, TFT_WHITE);
      tft.fillRect(x+1, y+1, (w-2) * pct / 100, 14, TFT_GREEN);
      break;
    }
    case ota::Ui::SUCCESS:
      tft.fillScreen(TFT_BLACK);
      tft.drawString("Rebooting...", tft.width()/2, tft.height()/2, 4);
      break;
    case ota::Ui::FAILED:
      tft.fillScreen(TFT_BLACK);
      tft.setTextColor(TFT_RED, TFT_BLACK);
      tft.drawString(msg ? msg : "Update failed", tft.width()/2, tft.height()/2, 4);
      delay(1500);
      break;
  }
}
```

- [ ] **Step 2: Initialize the listener FIRST in `setup()`**

Make `ota::begin(FW_VERSION);` the **first** statement in `setup()` — before `Serial.begin`, CAN, TFT, or LittleFS/font init. Add a short comment:
```cpp
void setup() {
  ota::begin(FW_VERSION);   // update-mode-first: reachable even if later init crashes
  // ... existing Serial/TFT/CAN/LittleFS init follows ...
}
```

- [ ] **Step 3: Service the update path at the top of `loop()`**

Make `ota::service(otaUi);` the **first** statement in `loop()`:
```cpp
void loop() {
  ota::service(otaUi);   // enters update mode on ENTER_UPDATE; pauses display during a session
  // ... existing CAN read + display logic follows ...
}
```
(While a session runs, `service()` blocks and `otaUi` owns the screen; normal display resumes after a failed session or never returns after a successful one.)

- [ ] **Step 4: Build, upload, and verify normal operation is unchanged**

On the bench with CAN replay: speed/gear/brightness/sport all behave exactly as before; Serial shows `All fonts are there!`.
Expected: no regression from the added calls when no update is happening.

- [ ] **Step 5: Verify entering update mode via the bench sender**

With the USB-TTL adapter still on GP0/GP1, run `python3 tools/ota_send.py <port> <newer.bin>`.
Expected: display switches to "Updating…" + progress bar, then "Rebooting…", then boots the new version; CAN display resumes normally afterward.

- [ ] **Step 6: Commit**

```bash
cd /home/honk/mazda
git add Arduino/can_receiver_display/can_receiver_display.ino
git commit -m "feat(rp2040): wire update-mode-first boot + TFT update UI"
```

---

## Task 7: Hardware assembly — XIAO↔RP2040 wiring + power

**Files:** none (hardware).

**Interfaces:**
- Consumes: the wiring contract in Global Constraints.
- Produces: a powered, wired two-board assembly ready for the XIAO firmware.

- [ ] **Step 1: Wire the UART + ground**

- XIAO **D6 (TX)** → RP2040 **GP1 (RX)**
- XIAO **D7 (RX)** → RP2040 **GP0 (TX)**
- XIAO **GND** ↔ RP2040 **GND**
Keep leads short. No level shifter (both 3.3 V).

- [ ] **Step 2: Wire shared power with decoupling**

- Feed both boards' **`3V3`** pins from the shared 3.3 V buck rail (back-feeding the XIAO `3V3`, bypassing its LDO).
- Add **~470 µF** bulk electrolytic across the rail and **100 µF + 0.1 µF** right at the XIAO `3V3`/`GND`.
- Confirm the buck is rated for the summed load with margin (~1 A class).

- [ ] **Step 3: Idle power check**

Power the assembly (RP2040 flashed with Task 6 firmware; XIAO blank or running any sketch). Measure the 3.3 V rail at idle.
Expected: steady ~3.3 V; RP2040 boots and shows the normal display with CAN replay. No brownouts, no resets.

- [ ] **Step 4: Commit (document the build)**

```bash
cd /home/honk/mazda
git commit --allow-empty -m "docs: XIAO<->RP2040 harness wired (D6->GP1, D7->GP0, shared 3V3 + decoupling)"
```

---

## Task 8: XIAO firmware — SoftAP + upload page + Wi-Fi→UART bridge

**Files:**
- Create: `Arduino/xiao_ota_bridge/xiao_ota_bridge.ino`, `Arduino/xiao_ota_bridge/upload_page.h`, `Arduino/xiao_ota_bridge/secrets.h`

**Interfaces:**
- Consumes: `otap::*` (Tasks 1–3); arduino-esp32 `WiFi`, `WebServer`, PSRAM (`ps_malloc`).
- Produces: the in-car update UI and the UART relay that talks to `ota_update` (Task 5).

- [ ] **Step 1: Create the secrets and the embedded page**

`Arduino/xiao_ota_bridge/secrets.h` (gitignored):
```cpp
#pragma once
#define AP_SSID "mazda-display"
#define AP_PASS "change-this-strong-pass"   // WPA2 requires >= 8 chars
```

`Arduino/xiao_ota_bridge/upload_page.h`:
```cpp
#pragma once
static const char UPLOAD_PAGE[] PROGMEM = R"HTML(
<!doctype html><html><head><meta name=viewport content="width=device-width,initial-scale=1">
<title>Mazda Display OTA</title>
<style>body{font-family:sans-serif;margin:2rem;max-width:32rem}
#bar{height:1rem;background:#0a0;width:0}#log{white-space:pre-wrap}</style></head><body>
<h2>Mazda Display Firmware Update</h2>
<p>Running version: <b id=ver>...</b></p>
<input type=file id=f accept=".bin"><button onclick=up()>Upload &amp; Flash</button>
<div style="border:1px solid #888;margin:1rem 0"><div id=bar></div></div>
<div id=log></div>
<script>
fetch('/ping').then(r=>r.text()).then(t=>ver.textContent=t).catch(()=>ver.textContent='(no reply)');
function up(){let file=f.files[0];if(!file){log.textContent='pick a .bin';return;}
 let x=new XMLHttpRequest();x.open('POST','/update');
 x.upload.onprogress=e=>{bar.style.width=(60*e.loaded/e.total)+'%';};
 x.onload=()=>{bar.style.width='100%';log.textContent=x.responseText;};
 x.onerror=()=>log.textContent='upload error';
 let fd=new FormData();fd.append('fw',file);x.send(fd);}
</script></body></html>
)HTML";
```

- [ ] **Step 2: Write the XIAO firmware**

`Arduino/xiao_ota_bridge/xiao_ota_bridge.ino`:
```cpp
#include <WiFi.h>
#include <WebServer.h>
#include <ota_protocol.h>
#include "secrets.h"
#include "upload_page.h"

using namespace otap;

static const unsigned long LINK_BAUD = 921600;
// XIAO ESP32-S3: D6 = TX (GPIO43), D7 = RX (GPIO44)
static const int PIN_TX = D6, PIN_RX = D7;

WebServer server(80);

// PSRAM image buffer (image ~1 MB; XIAO has 8 MB PSRAM).
static uint8_t* g_img = nullptr;
static size_t   g_cap = 0, g_len = 0;

static void send_frame(MsgType t, const uint8_t* p, size_t n) {
  uint8_t wire[FRAME_OVERHEAD + MAX_PAYLOAD];
  size_t w = frame_encode(t, p, n, wire, sizeof(wire));
  if (w) Serial1.write(wire, w);
}

// Read one control frame within timeout. Returns true on success.
static bool recv_frame(ParsedFrame* out, uint32_t timeout_ms) {
  static FrameParser p; frameparser_reset(&p);
  uint32_t end = millis() + timeout_ms;
  while (millis() < end) {
    while (Serial1.available()) {
      if (frameparser_push(&p, (uint8_t)Serial1.read(), out) == ParseStatus::FRAME_READY) return true;
    }
  }
  return false;
}

static String flash_to_rp2040() {
  // Drain any stale RX.
  while (Serial1.available()) Serial1.read();
  send_frame(MsgType::ENTER_UPDATE, nullptr, 0);
  ParsedFrame f;
  if (!recv_frame(&f, 3000) || f.type != MsgType::ACK) return "FAIL: no ACK from display";

  uint8_t hdr[8]; header_encode(g_len, crc32(g_img, g_len), hdr);
  send_frame(MsgType::HEADER, hdr, 8);
  Serial1.write(g_img, g_len);      // raw data phase
  Serial1.flush();

  if (!recv_frame(&f, 30000) || f.type != MsgType::RESULT || f.payload_len < 1)
    return "FAIL: no result from display";
  ResultCode rc = (ResultCode)f.payload[0];
  if (rc == ResultCode::OK) return "OK: display accepted image, rebooting.";
  return String("FAIL: display rejected image, code ") + String((int)rc);
}

static void handleRoot() { server.send_P(200, "text/html", UPLOAD_PAGE); }

static void handlePing() {
  while (Serial1.available()) Serial1.read();
  send_frame(MsgType::VERSION, nullptr, 0);   // request version
  ParsedFrame f;
  if (recv_frame(&f, 1500) && f.type == MsgType::VERSION) {
    server.send(200, "text/plain", String((const char*)f.payload).substring(0, f.payload_len));
  } else {
    server.send(200, "text/plain", "(display not responding)");
  }
}

// Multipart upload: buffer file into PSRAM, then flash.
static void handleUploadData() {
  HTTPUpload& u = server.upload();
  if (u.status == UPLOAD_FILE_START) { g_len = 0; }
  else if (u.status == UPLOAD_FILE_WRITE) {
    if (g_len + u.currentSize <= g_cap) { memcpy(g_img + g_len, u.buf, u.currentSize); g_len += u.currentSize; }
  }
}
static void handleUploadDone() {
  if (g_len == 0)            { server.send(400, "text/plain", "FAIL: empty upload"); return; }
  if (g_len > g_cap)         { server.send(413, "text/plain", "FAIL: image too large"); return; }
  String r = flash_to_rp2040();
  server.send(200, "text/plain", r);
}

void setup() {
  Serial.begin(115200);
  Serial1.begin(LINK_BAUD, SERIAL_8N1, PIN_RX, PIN_TX);

  g_cap = 2 * 1024 * 1024;                 // 2 MB ceiling (image ~1 MB)
  g_img = (uint8_t*)ps_malloc(g_cap);
  if (!g_img) { g_cap = 0; Serial.println("PSRAM alloc failed"); }

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);           // WPA2-PSK (pass >= 8 chars)
  Serial.print("AP up: "); Serial.println(WiFi.softAPIP());

  server.on("/", HTTP_GET, handleRoot);
  server.on("/ping", HTTP_GET, handlePing);
  server.on("/update", HTTP_POST, handleUploadDone, handleUploadData);
  server.begin();
}

void loop() { server.handleClient(); }
```
Notes:
- Board in IDE: **XIAO_ESP32S3**; enable **PSRAM** (`Tools → PSRAM: "OPI PSRAM"`).
- `D6`/`D7` map to GPIO43/44; `Serial1.begin(baud, cfg, RX, TX)` order is (RX, TX) on arduino-esp32.

- [ ] **Step 3: Build and upload to the XIAO**

Select XIAO_ESP32S3, PSRAM enabled, upload via USB-C.
Expected: Serial prints `AP up: 192.168.4.1`.

- [ ] **Step 4: Verify AP + version query end-to-end**

With the assembly powered (Task 7) and RP2040 running Task 6 firmware:
join Wi-Fi **`mazda-display`** with the WPA2 password → browse to `http://192.168.4.1/`.
Expected: page loads; "Running version" shows the RP2040's `FW_VERSION` (proves the UART link + PING/VERSION path works both directions).

- [ ] **Step 5: Commit**

```bash
cd /home/honk/mazda
git add Arduino/xiao_ota_bridge/xiao_ota_bridge.ino Arduino/xiao_ota_bridge/upload_page.h
git commit -m "feat(xiao): SoftAP + upload page + Wi-Fi->UART OTA bridge"
```
(`secrets.h` stays untracked per `.gitignore`; note in the commit body that it must be created locally.)

---

## Task 9: End-to-end happy-path integration

**Files:** none (verification; small fixes only if needed).

**Interfaces:**
- Consumes: full assembly (Tasks 6–8).
- Produces: proof that a real update flows phone → XIAO → RP2040 → reboot into new firmware.

- [ ] **Step 1: Build two visibly-distinct images**

Build image A (`FW_VERSION "2026.07.18-A"`, e.g. green sport label) and image B (`FW_VERSION "2026.07.18-B"`, e.g. a one-line on-screen change). Export both `.bin`s (`Sketch → Export Compiled Binary`). Flash A via USB as the starting point.

- [ ] **Step 2: Push image B wirelessly**

Join the AP → open page (confirm it shows `-A`) → upload image B → watch.
Expected: TFT shows "Updating…" + progress, then "Rebooting…"; page reports `OK`.

- [ ] **Step 3: Verify the new firmware is running**

After reboot, reload the page.
Expected: "Running version" now shows `2026.07.18-B`, the visible change is present, and CAN speed/gear/brightness render correctly on the bench.

- [ ] **Step 4: Commit result**

```bash
cd /home/honk/mazda
git commit --allow-empty -m "test: end-to-end wireless OTA happy path verified (A->B)"
```

---

## Task 10: Failure-injection gates ("trust it in the car")

**Files:** none (verification; fixes only if a gate fails).

**Interfaces:**
- Consumes: full assembly.
- Produces: evidence for the spec's non-negotiable safety gates.

- [ ] **Step 1: Corrupt image → rejected, old firmware resumes**

Flip a byte in image B's `.bin`, upload it.
Expected: page reports `FAIL ... code 1` (CRC_MISMATCH); TFT shows failure then **resumes the running firmware**; version unchanged; no reboot.

- [ ] **Step 2: Power cut during streaming → old firmware boots**

Start an upload of a good image; kill power to the assembly partway through the data phase; restore power.
Expected: RP2040 boots the **old** firmware (no command file was written). Re-run the update; it succeeds.

- [ ] **Step 3: Power cut during bootloader apply → completes on next boot**

Start a good upload; after the page reports `OK` and the board begins rebooting/applying, cut power during the apply; restore power.
Expected: the stage-3 bootloader **resumes and completes** the copy on the next power-up; the new firmware runs. (Validates the power-fail-safe claim directly.)

- [ ] **Step 4: Stream stall → RP2040 times out, resumes**

Begin an update, then power off the XIAO mid-transfer (simulating a crash).
Expected: after the byte-timeout the RP2040 aborts to normal operation; old firmware runs; a subsequent update succeeds.

- [ ] **Step 5: Lockout-recovery via update-mode-first**

Build a deliberately broken image (e.g. `while(1){}` inserted **after** `ota::begin()` but in font/display init) and flash it wirelessly. Then attempt to push a known-good image over the XIAO.
Expected: even though the app is hung, the update listener (initialized first) still accepts the good image and recovers the board **without opening the dash**. If this fails, STOP — the remote-update premise is unsafe; revisit boot ordering before any in-car use.

- [ ] **Step 6: Record results**

```bash
cd /home/honk/mazda
git commit --allow-empty -m "test: failure-injection gates passed (corrupt/powerloss/stall/lockout)"
```

---

## Task 11: Rail/brownout + security checks

**Files:** none (verification).

**Interfaces:**
- Consumes: full assembly.
- Produces: confirmation the power design and access control hold.

- [ ] **Step 1: Brownout check under Wi-Fi burst**

Monitor the 3.3 V rail (meter or scope) during a full flash while the XIAO AP is active and streaming.
Expected: rail stays within tolerance (no dips that reset the RP2040). If it sags, increase bulk capacitance / buck headroom before in-car use.

- [ ] **Step 2: Access-control check**

Attempt to join the AP with a wrong/blank password; attempt to reach `http://192.168.4.1/` without joining.
Expected: WPA2 rejects the join; the page is unreachable until joined with the correct password.

- [ ] **Step 3: Final commit**

```bash
cd /home/honk/mazda
git commit --allow-empty -m "test: rail brownout + WPA2 access control verified"
```

---

## Self-Review

**Spec coverage:**
- SoftAP + upload page → Task 8. WPA2 → Tasks 8, 11. CRC32 integrity → Tasks 1, 5, 10. Serial self-update via arduino-pico `Update` → Tasks 4, 5. Stage-3 power-fail-safe bootloader → Tasks 4, 10.3. 3-wire UART @921600 → Global Constraints, Tasks 5–8. Shared 3.3 V rail + decoupling → Task 7, 11.1. Update-mode-first boot → Task 6, validated Task 10.5. Never-OTA-the-bootloader → Global Constraints (design honored; no task exposes it). PING/version → Tasks 5, 8. Failure matrix → Task 10. Host-unit-tested pure logic → Tasks 1–3. Bench tiers 2–4 → Tasks 4–11. Flash layout (≥2 MB FS) → Task 4. **All spec sections map to tasks.**
- Open items from the spec are now closed: checksum = CRC32 (Task 1), baud = 921600 (Global Constraints), XIAO pins = D6/D7 (Task 8), LittleFS ≥2 MB (Task 4), GZIP transit not used (raw stream — simpler; noted as dropped).

**Placeholder scan:** No "TBD"/"handle errors"/"similar to Task N". Two legitimate confirm-then-implement steps (Task 4.2 core-OTA support, Task 5.1 `Update` API surface) name the exact expected calls and adapt only if the installed core differs — these are verification steps, not placeholders.

**Type consistency:** `otap::` names (`crc32*`, `frame_encode`, `frameparser_push`, `ParseStatus`, `header_encode/decode`, `sm_step`, `MsgType`, `ResultCode`, `State/Event/Action`) are defined in Task 0 and used identically in Tasks 1–3, 5, 8. `ota::begin/service/Ui/UiCallback` defined in Task 5, used in Task 6. Frame format identical across `ota_send.py`, `ota_update.cpp`, and `xiao_ota_bridge.ino`. Wiring (D6→GP1, D7→GP0) consistent across Global Constraints and Tasks 5, 7, 8.
