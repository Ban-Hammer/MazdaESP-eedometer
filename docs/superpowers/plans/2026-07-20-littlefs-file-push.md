# Per-File LittleFS Push + File Browser Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Wireless per-file management of the RP2040's LittleFS via the XIAO page — browse folders, upload/replace individual files into the current folder, and delete files, reusing the existing UART OTA protocol. No reboot.

**Architecture:** Extend `ota_protocol` with 5 message types (list/write/delete) reusing the framing + CRC32 + block-ACK data phase. The RP2040 factors its block-receive loop to feed either `Update` (firmware) or a `File` (new), and adds three dispatch cases; writes go to a temp file that is CRC-verified then atomically renamed. The XIAO adds `/api/list|upload|delete` HTTP endpoints translating to UART frames. A dark two-column dashboard page (firmware | file browser) replaces the current upload page.

**Tech Stack:** RP2040 (arduino-pico, LittleFS, Updater), XIAO ESP32-S3 (WiFi SoftAP, WebServer, PSRAM), shared C++17 `ota_protocol`, g++ host tests, arduino-cli.

## Global Constraints

- Spec: `docs/superpowers/specs/2026-07-20-littlefs-file-push-design.md`.
- **FQBNs:** RP2040 `rp2040:rp2040:adafruit_feather_can:flash=8388608_2097152,freq=250`; XIAO `esp32:esp32:XIAO_ESP32S3:PSRAM=opi`.
- Framing/CRC32/block-ACK (`OTA_BLOCK=4096`) unchanged — new features reuse them.
- **New MsgTypes:** `LIST_REQ=0x08, LIST_ENTRY=0x09, LIST_END=0x0A, WRITE_FILE=0x0B, DELETE_FILE=0x0C`. **New ResultCodes:** `NOT_FOUND=0x05, WRITE_ERROR=0x06`.
- **Payload fits `MAX_PAYLOAD=64`:** `WRITE_FILE` = 8 (len+crc) + path; `LIST_ENTRY` = 5 (flags+size) + name; cap name/path so total ≤ 64. Define `OTA_MAX_NAME=48`.
- **Temp file:** `/.upload.tmp` — hidden from listings, renamed to dest on CRC success (`lfs_rename` replaces an existing dest atomically).
- **Path rules:** must start with `/`, reject any `..`, length ≤ 63.
- **Verified LittleFS API (arduino-pico 5.7.0):** `Dir d = LittleFS.openDir(path)`; `d.next()`, `d.fileName()` (String), `d.fileSize()` (size_t), `d.isDirectory()`; `FSInfo fi; LittleFS.info(fi)` → `fi.totalBytes`/`fi.usedBytes` (uint64_t); `LittleFS.rename/remove/exists`.
- File ops do **not** take over the TFT; only firmware flash does. No reboot for file ops.

---

## File Structure

```
Arduino/libraries/ota_protocol/src/ota_protocol.h    # MODIFY: MsgTypes, ResultCodes, helpers
Arduino/libraries/ota_protocol/src/ota_protocol.cpp  # MODIFY: helper impls
Arduino/libraries/ota_protocol/test/test_frame.cpp   # MODIFY: list_entry + writehdr tests
Arduino/can_receiver_display/ota_update.cpp          # MODIFY: factor receiver + 3 dispatch cases
Arduino/xiao_ota_bridge/xiao_ota_bridge.ino          # MODIFY: /api/list|upload|delete + refactor stream
Arduino/xiao_ota_bridge/upload_page.h                # REWRITE: dark two-column dashboard page
```

---

## Task 1: Protocol extension + host tests

**Files:**
- Modify: `Arduino/libraries/ota_protocol/src/ota_protocol.h`, `.../ota_protocol.cpp`, `.../test/test_frame.cpp`

**Interfaces:**
- Produces: new `MsgType`/`ResultCode` values; `list_entry_encode/decode`, `writehdr_encode/decode`; used by RP2040 (Task 2) and XIAO (Task 3).

- [ ] **Step 1: Extend the enums + declare helpers (header)**

In `ota_protocol.h`, add to `enum class MsgType`:
```cpp
  LIST_REQ     = 0x08,  // payload: dir path
  LIST_ENTRY   = 0x09,  // payload: flags(1) + size(u32 LE) + name
  LIST_END     = 0x0A,  // payload: total(u32 LE) + used(u32 LE)
  WRITE_FILE   = 0x0B,  // payload: len(u32) + crc32(u32) + dest path; then block-ACK data phase
  DELETE_FILE  = 0x0C,  // payload: path
```
Add to `enum class ResultCode`:
```cpp
  NOT_FOUND    = 0x05,
  WRITE_ERROR  = 0x06,
```
Add near the other declarations:
```cpp
static const size_t OTA_MAX_NAME = 48;   // cap for names/paths in a single frame

// LIST_ENTRY payload: flags(1, bit0=isDir) + size(u32 LE) + name (no NUL).
// Returns bytes written, or 0 if name too long / out_cap too small.
size_t list_entry_encode(bool isDir, uint32_t size, const char* name, uint8_t* out, size_t out_cap);
bool   list_entry_decode(const uint8_t* payload, size_t plen, bool* isDir, uint32_t* size,
                         char* name, size_t name_cap);

// WRITE_FILE payload: len(u32 LE) + crc32(u32 LE) + path (no NUL).
size_t writehdr_encode(uint32_t len, uint32_t crc, const char* path, uint8_t* out, size_t out_cap);
bool   writehdr_decode(const uint8_t* payload, size_t plen, uint32_t* len, uint32_t* crc,
                       char* path, size_t path_cap);
```

- [ ] **Step 2: Write failing tests**

Append to `test/test_frame.cpp` (before `int main`):
```cpp
static void test_list_entry_roundtrip() {
  uint8_t p[MAX_PAYLOAD];
  size_t n = list_entry_encode(true, 6321, "000.jpg", p, sizeof(p));
  CHECK(n > 0);
  bool isDir; uint32_t sz; char name[OTA_MAX_NAME];
  CHECK(list_entry_decode(p, n, &isDir, &sz, name, sizeof(name)));
  CHECK_EQ(isDir, true);
  CHECK_EQ(sz, 6321u);
  CHECK(std::string(name) == "000.jpg");
}

static void test_list_entry_rejects_long_name() {
  char big[OTA_MAX_NAME + 5]; for (auto& c : big) c = 'x'; big[OTA_MAX_NAME + 4] = 0;
  uint8_t p[MAX_PAYLOAD];
  CHECK_EQ(list_entry_encode(false, 0, big, p, sizeof(p)), 0u);
}

static void test_writehdr_roundtrip() {
  uint8_t p[MAX_PAYLOAD];
  size_t n = writehdr_encode(40000, 0xDEADBEEFu, "/splash/012.jpg", p, sizeof(p));
  CHECK(n > 0);
  uint32_t len, crc; char path[OTA_MAX_NAME];
  CHECK(writehdr_decode(p, n, &len, &crc, path, sizeof(path)));
  CHECK_EQ(len, 40000u);
  CHECK_EQ(crc, 0xDEADBEEFu);
  CHECK(std::string(path) == "/splash/012.jpg");
}
```
Add to `main()` (needs `#include <string>` at top of the test file):
```cpp
  RUN(test_list_entry_roundtrip);
  RUN(test_list_entry_rejects_long_name);
  RUN(test_writehdr_roundtrip);
```

- [ ] **Step 3: Run tests — verify they fail**

Run: `cd /home/honk/mazda/Arduino/libraries/ota_protocol/test && make test_frame && ./test_frame`
Expected: FAIL / link error (helpers undefined).

- [ ] **Step 4: Implement the helpers (cpp)**

In `ota_protocol.cpp` (the `put_u32`/`get_u32` statics already exist), add:
```cpp
size_t list_entry_encode(bool isDir, uint32_t size, const char* name, uint8_t* out, size_t out_cap) {
  size_t nlen = 0; while (name[nlen]) nlen++;
  if (nlen > OTA_MAX_NAME) return 0;
  size_t total = 1 + 4 + nlen;
  if (out_cap < total || total > MAX_PAYLOAD) return 0;
  out[0] = isDir ? 1 : 0;
  put_u32(out + 1, size);
  for (size_t i = 0; i < nlen; ++i) out[5 + i] = (uint8_t)name[i];
  return total;
}
bool list_entry_decode(const uint8_t* payload, size_t plen, bool* isDir, uint32_t* size,
                       char* name, size_t name_cap) {
  if (plen < 5) return false;
  *isDir = (payload[0] & 1) != 0;
  *size = get_u32(payload + 1);
  size_t nlen = plen - 5;
  if (nlen >= name_cap) return false;
  for (size_t i = 0; i < nlen; ++i) name[i] = (char)payload[5 + i];
  name[nlen] = 0;
  return true;
}
size_t writehdr_encode(uint32_t len, uint32_t crc, const char* path, uint8_t* out, size_t out_cap) {
  size_t plen = 0; while (path[plen]) plen++;
  if (plen > OTA_MAX_NAME) return 0;
  size_t total = 8 + plen;
  if (out_cap < total || total > MAX_PAYLOAD) return 0;
  put_u32(out, len); put_u32(out + 4, crc);
  for (size_t i = 0; i < plen; ++i) out[8 + i] = (uint8_t)path[i];
  return total;
}
bool writehdr_decode(const uint8_t* payload, size_t plen, uint32_t* len, uint32_t* crc,
                     char* path, size_t path_cap) {
  if (plen < 8) return false;
  *len = get_u32(payload); *crc = get_u32(payload + 4);
  size_t n = plen - 8;
  if (n >= path_cap) return false;
  for (size_t i = 0; i < n; ++i) path[i] = (char)payload[8 + i];
  path[n] = 0;
  return true;
}
```

- [ ] **Step 5: Run the whole suite — verify pass**

Run: `cd /home/honk/mazda/Arduino/libraries/ota_protocol/test && make clean && make`
Expected: all suites `0 failures` (crc32, frame incl. new tests, state_machine).

- [ ] **Step 6: Commit**

```bash
cd /home/honk/mazda
git add Arduino/libraries/ota_protocol
git commit -m "feat(ota_protocol): list/write/delete message types + helpers"
```

---

## Task 2: RP2040 — factor block receiver + file-op dispatch

**Files:**
- Modify: `Arduino/can_receiver_display/ota_update.cpp`

**Interfaces:**
- Consumes: `otap::*` (Task 1); `LittleFS`, `Update`, `Serial1`.
- Produces: RP2040 handling of `LIST_REQ`, `WRITE_FILE`, `DELETE_FILE`; firmware flow unchanged on the wire.

- [ ] **Step 1: Add includes + path validation + a generic block receiver**

At the top of `ota_update.cpp` add:
```cpp
#include <FS.h>
#include <LittleFS.h>
```
In the anonymous namespace, add a path check, block-sink typedef, and refactored receiver:
```cpp
  const char* TMP_PATH = "/.upload.tmp";

  bool valid_path(const char* p) {
    if (!p || p[0] != '/') return false;
    if (strstr(p, "..")) return false;
    return strlen(p) <= 63;
  }

  // A sink consumes one decoded block; returns true on success.
  typedef bool (*BlockSink)(void* ctx, const uint8_t* buf, size_t n);

  // Receive `len` raw bytes in OTA_BLOCK chunks under per-block flow control, feeding
  // each block to `sink` and accumulating CRC32. Sends an ACK after each written block.
  // Caller sends the header-ACK and opens/finalizes the sink. Returns:
  //   OK on full receive + crc match; CRC_MISMATCH; TIMEOUT (stall); WRITE_ERROR (sink).
  ResultCode receive_blocks(uint32_t len, uint32_t crc, BlockSink sink, void* ctx,
                            ota::UiCallback cb) {
    static uint8_t buf[OTA_BLOCK];
    uint32_t acc = crc32_init(), remaining = len; uint8_t last_pct = 255;
    while (remaining) {
      size_t chunk = remaining < OTA_BLOCK ? remaining : OTA_BLOCK;
      if (read_exact(buf, chunk, BYTE_TIMEOUT_MS) != chunk) return ResultCode::TIMEOUT;
      if (!sink(ctx, buf, chunk)) return ResultCode::WRITE_ERROR;
      acc = crc32_update(acc, buf, chunk);
      remaining -= chunk;
      send_frame(MsgType::ACK, nullptr, 0);
      if (cb) { uint8_t pct = (uint8_t)(100u * (uint64_t)(len - remaining) / len);
                if (pct != last_pct) { cb(ota::Ui::PROGRESS, pct, "Updating"); last_pct = pct; } }
    }
    return (crc32_final(acc) == crc) ? ResultCode::OK : ResultCode::CRC_MISMATCH;
  }

  bool updateSink(void*, const uint8_t* b, size_t n) { return Update.write((uint8_t*)b, n) == n; }
  bool fileSink(void* ctx, const uint8_t* b, size_t n) { return ((File*)ctx)->write(b, n) == n; }
```
(Note: `read_exact`, `send_frame`, `send_result`, `BYTE_TIMEOUT_MS` already exist in this file from the firmware OTA implementation.)

- [ ] **Step 2: Point the firmware path at the new receiver**

Replace the body of the existing `receive_and_stage(...)` so it delegates to `receive_blocks` (keeps the same header-ACK-then-blocks wire behavior):
```cpp
  bool receive_and_stage(uint32_t img_len, uint32_t img_crc, ota::UiCallback cb) {
    if (img_len == 0)           { send_result(ResultCode::BAD_LENGTH); return false; }
    if (!Update.begin(img_len)) { send_result(ResultCode::FLASH_ERROR); return false; }
    send_frame(MsgType::ACK, nullptr, 0);                 // header-ack: send block 0
    ResultCode rc = receive_blocks(img_len, img_crc, updateSink, nullptr, cb);
    if (rc != ResultCode::OK) { send_result(rc); return false; }
    return true;
  }
```
(Behavior identical to before: on success the caller `run_session` does `Update.end()` + reboot.)

- [ ] **Step 3: Add file-op handlers**

Add these functions in the anonymous namespace:
```cpp
  void handle_list(const char* path) {
    Dir d = LittleFS.openDir(path);
    uint8_t p[MAX_PAYLOAD];
    while (d.next()) {
      String fn = d.fileName();
      const char* nm = fn.c_str();
      const char* slash = strrchr(nm, '/'); if (slash) nm = slash + 1;  // basename
      if (!strcmp(nm, TMP_PATH + 1)) continue;                          // hide temp file
      size_t n = list_entry_encode(d.isDirectory(), (uint32_t)d.fileSize(), nm, p, sizeof(p));
      if (n) send_frame(MsgType::LIST_ENTRY, p, n);
    }
    FSInfo fi; LittleFS.info(fi);
    uint8_t e[8]; header_encode((uint32_t)fi.totalBytes, (uint32_t)fi.usedBytes, e);
    send_frame(MsgType::LIST_END, e, 8);
  }

  void handle_write(const uint8_t* payload, size_t plen, ota::UiCallback cb) {
    uint32_t len, crc; char path[64];
    if (!writehdr_decode(payload, plen, &len, &crc, path, sizeof(path)) || !valid_path(path)
        || len == 0) { send_result(ResultCode::WRITE_ERROR); return; }
    LittleFS.remove(TMP_PATH);
    File f = LittleFS.open(TMP_PATH, "w");
    if (!f) { send_result(ResultCode::WRITE_ERROR); return; }
    send_frame(MsgType::ACK, nullptr, 0);                 // header-ack
    ResultCode rc = receive_blocks(len, crc, fileSink, &f, cb);
    f.close();
    if (rc != ResultCode::OK) { LittleFS.remove(TMP_PATH); send_result(rc); return; }
    LittleFS.remove(path);                                // ensure clean replace
    send_result(LittleFS.rename(TMP_PATH, path) ? ResultCode::OK : ResultCode::WRITE_ERROR);
  }

  void handle_delete(const uint8_t* payload, size_t plen) {
    char path[64];
    size_t n = plen < sizeof(path) ? plen : sizeof(path) - 1;
    memcpy(path, payload, n); path[n] = 0;
    if (!valid_path(path))        { send_result(ResultCode::WRITE_ERROR); return; }
    if (!LittleFS.exists(path))   { send_result(ResultCode::NOT_FOUND); return; }
    send_result(LittleFS.remove(path) ? ResultCode::OK : ResultCode::WRITE_ERROR);
  }
```

- [ ] **Step 4: Wire the three cases into `service()`**

In `service()`'s `switch (f.type)`, add before `default`:
```cpp
      case MsgType::LIST_REQ: {
        char path[64]; size_t n = f.payload_len < sizeof(path) ? f.payload_len : sizeof(path) - 1;
        memcpy(path, f.payload, n); path[n] = 0;
        handle_list(valid_path(path) ? path : "/");
        break;
      }
      case MsgType::WRITE_FILE:
        handle_write(f.payload, f.payload_len, cb);
        break;
      case MsgType::DELETE_FILE:
        handle_delete(f.payload, f.payload_len);
        break;
```

- [ ] **Step 5: Compile**

Run: `arduino-cli compile --fqbn rp2040:rp2040:adafruit_feather_can:flash=8388608_2097152,freq=250 Arduino/can_receiver_display`
Expected: `Sketch uses …` (compiles clean).

- [ ] **Step 6: Commit**

```bash
cd /home/honk/mazda
git add Arduino/can_receiver_display/ota_update.cpp
git commit -m "feat(rp2040): LittleFS list/write/delete over UART; factor block receiver"
```

---

## Task 3: XIAO — file API endpoints

**Files:**
- Modify: `Arduino/xiao_ota_bridge/xiao_ota_bridge.ino`

**Interfaces:**
- Consumes: `otap::*` (Task 1); existing `send_frame`, `recv_frame`, `g_img`/`g_len`, `WebServer server`.
- Produces: `GET /api/list`, `POST /api/upload`, `POST /api/delete`; a reusable `stream_blocks`.

- [ ] **Step 1: Factor the block-streaming loop**

In `xiao_ota_bridge.ino`, extract the data-phase loop (currently inside `flash_to_rp2040`) into a helper:
```cpp
// Stream `len` bytes from `buf` under per-block flow control; waits for the display's
// ACK after each block. Returns "" on success or an error string.
static String stream_blocks(const uint8_t* buf, size_t len) {
  ParsedFrame f;
  size_t sent = 0;
  while (sent < len) {
    size_t chunk = (len - sent) < OTA_BLOCK ? (len - sent) : OTA_BLOCK;
    Serial1.write(buf + sent, chunk);
    Serial1.flush();
    if (!recv_frame(&f, 5000) || f.type != MsgType::ACK)
      return String("stalled at ") + String((unsigned)sent) + " bytes";
    sent += chunk;
  }
  return "";
}
```
Then in `flash_to_rp2040`, replace the inline `while (sent < g_len) {…}` block with:
```cpp
  String err = stream_blocks(g_img, g_len);
  if (err.length()) return "FAIL: " + err;
```
(Leave the ENTER_UPDATE/ACK/HEADER handshake and final RESULT handling in `flash_to_rp2040` as-is.)

- [ ] **Step 2: Add the list endpoint (JSON)**

Add helper + handler:
```cpp
static void jsonEscape(String& out, const char* s) {
  for (; *s; ++s) { if (*s == '"' || *s == '\\') out += '\\'; out += *s; }
}

static void handleList() {
  String path = server.hasArg("path") ? server.arg("path") : "/";
  while (Serial1.available()) Serial1.read();
  send_frame(MsgType::LIST_REQ, (const uint8_t*)path.c_str(), path.length());

  String json = "{\"path\":\""; jsonEscape(json, path.c_str());
  json += "\",\"entries\":[";
  ParsedFrame f; bool first = true; uint32_t total = 0, used = 0;
  while (recv_frame(&f, 2000)) {
    if (f.type == MsgType::LIST_ENTRY) {
      bool isDir; uint32_t sz; char name[OTA_MAX_NAME];
      if (list_entry_decode(f.payload, f.payload_len, &isDir, &sz, name, sizeof(name))) {
        if (!first) json += ',';
        first = false;
        json += "{\"name\":\""; jsonEscape(json, name);
        json += "\",\"size\":" + String(sz) + ",\"dir\":" + (isDir ? "true" : "false") + "}";
      }
    } else if (f.type == MsgType::LIST_END) {
      header_decode(f.payload, &total, &used);
      break;
    } else break;
  }
  json += "],\"total\":" + String(total) + ",\"used\":" + String(used) + "}";
  server.send(200, "application/json", json);
}
```

- [ ] **Step 3: Add the upload + delete endpoints**

```cpp
static String g_uploadName;   // captured during the multipart stream

static void handleFileUploadData() {
  HTTPUpload& u = server.upload();
  if (u.status == UPLOAD_FILE_START) { g_len = 0; g_uploadName = u.filename; }
  else if (u.status == UPLOAD_FILE_WRITE) {
    if (g_cap && g_len + u.currentSize <= g_cap) { memcpy(g_img + g_len, u.buf, u.currentSize); g_len += u.currentSize; }
    else g_len = g_cap + 1;
  }
}
static void handleFileUploadDone() {
  if (g_cap == 0)    { server.send(500, "text/plain", "FAIL: XIAO PSRAM unavailable"); return; }
  if (g_len == 0)    { server.send(400, "text/plain", "FAIL: empty upload"); return; }
  if (g_len > g_cap) { server.send(413, "text/plain", "FAIL: file too large"); return; }

  String dir = server.hasArg("path") ? server.arg("path") : "/";
  String dest = (dir == "/") ? ("/" + g_uploadName) : (dir + "/" + g_uploadName);

  while (Serial1.available()) Serial1.read();
  uint8_t hdr[MAX_PAYLOAD];
  size_t hn = writehdr_encode(g_len, crc32(g_img, g_len), dest.c_str(), hdr, sizeof(hdr));
  if (!hn) { server.send(400, "text/plain", "FAIL: path too long"); return; }
  send_frame(MsgType::WRITE_FILE, hdr, hn);

  ParsedFrame f;
  if (!recv_frame(&f, 5000) || f.type != MsgType::ACK) { server.send(200, "text/plain", "FAIL: display not ready"); return; }
  String err = stream_blocks(g_img, g_len);
  if (err.length()) { server.send(200, "text/plain", "FAIL: " + err); return; }
  if (!recv_frame(&f, 5000) || f.type != MsgType::RESULT || f.payload_len < 1) { server.send(200, "text/plain", "FAIL: no result"); return; }
  ResultCode rc = (ResultCode)f.payload[0];
  server.send(200, "text/plain", rc == ResultCode::OK ? ("OK: wrote " + dest) : ("FAIL: display error " + String((int)rc)));
}

static void handleDelete() {
  String path = server.hasArg("path") ? server.arg("path") : "";
  if (!path.length()) { server.send(400, "text/plain", "FAIL: no path"); return; }
  while (Serial1.available()) Serial1.read();
  send_frame(MsgType::DELETE_FILE, (const uint8_t*)path.c_str(), path.length());
  ParsedFrame f;
  if (!recv_frame(&f, 3000) || f.type != MsgType::RESULT || f.payload_len < 1) { server.send(200, "text/plain", "FAIL: no result"); return; }
  ResultCode rc = (ResultCode)f.payload[0];
  server.send(200, "text/plain", rc == ResultCode::OK ? "OK" : ("FAIL: code " + String((int)rc)));
}
```

- [ ] **Step 4: Register the routes in `setup()`**

After the existing `server.on(...)` lines, add:
```cpp
  server.on("/api/list", HTTP_GET, handleList);
  server.on("/api/upload", HTTP_POST, handleFileUploadDone, handleFileUploadData);
  server.on("/api/delete", HTTP_POST, handleDelete);
```

- [ ] **Step 5: Compile**

Run: `arduino-cli compile --fqbn esp32:esp32:XIAO_ESP32S3:PSRAM=opi Arduino/xiao_ota_bridge`
Expected: compiles clean.

- [ ] **Step 6: Commit**

```bash
cd /home/honk/mazda
git add Arduino/xiao_ota_bridge/xiao_ota_bridge.ino
git commit -m "feat(xiao): /api/list, /api/upload, /api/delete file endpoints"
```

---

## Task 4: Web page — dark two-column dashboard

**Files:**
- Rewrite: `Arduino/xiao_ota_bridge/upload_page.h`

**Interfaces:**
- Consumes: `/ping`, `/update`, `/api/list`, `/api/upload`, `/api/delete` (Task 3).
- Produces: the served page (compiled into the XIAO sketch).

- [ ] **Step 1: Replace the page with the dashboard**

Overwrite `Arduino/xiao_ota_bridge/upload_page.h` with:
```cpp
#pragma once
static const char UPLOAD_PAGE[] PROGMEM = R"HTML(
<!doctype html><html><head><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1">
<title>MAZDASPEED Display</title>
<style>
:root{--bg:#0a0b0d;--card:#15171b;--line:#282c33;--fg:#e6e8ea;--dim:#8b929c;--acc:#ff6a00}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--fg);
 font:15px/1.4 system-ui,-apple-system,Segoe UI,Roboto,sans-serif}
header{display:flex;align-items:center;gap:.6rem;padding:1rem 1.25rem;border-bottom:1px solid var(--line)}
header .dot{width:10px;height:10px;border-radius:50%;background:#444}
header .dot.ok{background:var(--acc);box-shadow:0 0 8px var(--acc)}
header h1{font-size:1rem;letter-spacing:.14em;margin:0;font-weight:600}
header .ver{margin-left:auto;color:var(--dim);font:12px ui-monospace,monospace}
main{max-width:900px;margin:0 auto;padding:1.25rem;display:grid;gap:1.25rem;
 grid-template-columns:1fr 1fr}
@media(max-width:640px){main{grid-template-columns:1fr}}
.card{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:1.1rem}
.card h2{margin:.1rem 0 .9rem;font-size:.72rem;letter-spacing:.16em;color:var(--dim);font-weight:700}
button{font:inherit;color:#111;background:var(--acc);border:0;border-radius:8px;
 padding:.5rem .9rem;cursor:pointer;font-weight:600}
button.ghost{background:transparent;color:var(--fg);border:1px solid var(--line);font-weight:500}
button:disabled{opacity:.5;cursor:default}
input[type=file]{color:var(--dim);font-size:.85rem;max-width:100%}
.bar{height:8px;background:#000;border-radius:6px;overflow:hidden;margin:.7rem 0 0}
.bar>i{display:block;height:100%;width:0;background:var(--acc);transition:width .2s}
.status{margin-top:.6rem;font:12px ui-monospace,monospace;color:var(--dim);white-space:pre-wrap}
.crumb{display:flex;gap:.3rem;align-items:center;font:13px ui-monospace,monospace;color:var(--dim);margin-bottom:.5rem}
.crumb b{color:var(--fg)}
.space{margin-left:auto;color:var(--dim)}
ul.files{list-style:none;margin:0 0 .8rem;padding:0;border:1px solid var(--line);border-radius:8px;overflow:hidden}
ul.files li{display:flex;align-items:center;gap:.6rem;padding:.5rem .7rem;border-top:1px solid var(--line)}
ul.files li:first-child{border-top:0}
ul.files li .nm{flex:1;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;cursor:default}
ul.files li.dir .nm{cursor:pointer;color:var(--acc)}
ul.files li .sz{color:var(--dim);font:12px ui-monospace,monospace}
.x{background:transparent;border:0;color:var(--dim);cursor:pointer;font-size:1rem;padding:.1rem .3rem}
.x:hover{color:#ff5a5a}
</style></head><body>
<header><span class=dot id=dot></span><h1>MAZDASPEED DISPLAY</h1><span class=ver id=ver>…</span></header>
<main>
  <section class=card>
    <h2>FIRMWARE</h2>
    <input type=file id=fw accept=".bin">
    <div><button id=fwbtn onclick=flashFw()>Flash ▸</button></div>
    <div class=bar><i id=fwbar></i></div>
    <div class=status id=fwlog></div>
  </section>
  <section class=card>
    <h2>FILES</h2>
    <div class=crumb id=crumb></div>
    <ul class=files id=list></ul>
    <input type=file id=up>
    <div style="margin-top:.5rem"><button id=upbtn onclick=upload()>⬆ Upload here</button></div>
    <div class=bar><i id=upbar></i></div>
    <div class=status id=uplog></div>
  </section>
</main>
<script>
let cwd="/";
const $=id=>document.getElementById(id);
fetch('/ping').then(r=>r.text()).then(t=>{$('ver').textContent=t;$('dot').className='dot ok';})
  .catch(()=>{$('ver').textContent='(no reply)';});

function fmt(n){return n<1024?n+' B':n<1048576?(n/1024).toFixed(1)+' KB':(n/1048576).toFixed(2)+' MB';}

function renderCrumb(){
  const parts=cwd.split('/').filter(Boolean);
  let html='<b style="cursor:pointer" onclick="nav(\'/\')">/</b>',acc='';
  parts.forEach(p=>{acc+='/'+p;html+=' ‹ <span style="cursor:pointer" onclick="nav(\''+acc+'\')">'+p+'</span>';});
  $('crumb').innerHTML=html;
}
function nav(p){cwd=p;list();}
function list(){
  renderCrumb();
  fetch('/api/list?path='+encodeURIComponent(cwd)).then(r=>r.json()).then(d=>{
    const ul=$('list');ul.innerHTML='';
    if(cwd!=='/'){const up=cwd.replace(/\/[^/]+$/,'')||'/';
      ul.insertAdjacentHTML('beforeend','<li class=dir><span class=nm onclick="nav(\''+up+'\')">.. ⬆</span></li>');}
    d.entries.sort((a,b)=>(b.dir-a.dir)||a.name.localeCompare(b.name));
    d.entries.forEach(e=>{
      const li=document.createElement('li');li.className=e.dir?'dir':'';
      const full=(cwd==='/'?'':cwd)+'/'+e.name;
      if(e.dir){li.innerHTML='<span class=nm>▸ '+e.name+'</span>';
        li.querySelector('.nm').onclick=()=>nav(full);}
      else{li.innerHTML='<span class=nm>'+e.name+'</span><span class=sz>'+fmt(e.size)+'</span>';
        const x=document.createElement('button');x.className='x';x.textContent='🗑';
        x.onclick=()=>del(full,x);li.appendChild(x);}
      ul.appendChild(li);
    });
    const sp=document.createElement('div');sp.className='space';
    sp.textContent=fmt(d.used)+' / '+fmt(d.total);
    $('crumb').appendChild(sp);
  }).catch(()=>{$('list').innerHTML='<li>display not responding</li>';});
}
function del(path,btn){
  if(btn.dataset.armed){
    fetch('/api/delete?path='+encodeURIComponent(path),{method:'POST'})
      .then(r=>r.text()).then(t=>{$('uplog').textContent=t;list();});
  } else { btn.dataset.armed=1;btn.textContent='✓✕';btn.title='confirm delete';
    setTimeout(()=>{if(btn.dataset.armed){delete btn.dataset.armed;btn.textContent='🗑';}},2500);}
}
function xhrUpload(url,file,bar,log,done){
  const x=new XMLHttpRequest();x.open('POST',url);
  x.upload.onprogress=e=>{$(bar).style.width=(80*e.loaded/e.total)+'%';};
  x.onload=()=>{$(bar).style.width='100%';$(log).textContent=x.responseText;done&&done();};
  x.onerror=()=>{$(log).textContent='upload error';};
  const fd=new FormData();fd.append('f',file);x.send(fd);
}
function flashFw(){
  const f=$('fw').files[0];if(!f){$('fwlog').textContent='pick a .bin';return;}
  $('fwlog').textContent='uploading…';xhrUpload('/update',f,'fwbar','fwlog');
}
function upload(){
  const f=$('up').files[0];if(!f){$('uplog').textContent='pick a file';return;}
  $('uplog').textContent='uploading '+f.name+'…';
  xhrUpload('/api/upload?path='+encodeURIComponent(cwd),f,'upbar','uplog',()=>{$('upbar').style.width='0';list();});
}
list();
</script></body></html>
)HTML";
```

- [ ] **Step 2: Compile the XIAO sketch with the new page**

Run: `arduino-cli compile --fqbn esp32:esp32:XIAO_ESP32S3:PSRAM=opi Arduino/xiao_ota_bridge`
Expected: compiles clean; flash usage rises modestly.

- [ ] **Step 3: Commit**

```bash
cd /home/honk/mazda
git add Arduino/xiao_ota_bridge/upload_page.h
git commit -m "feat(xiao): dark two-column dashboard page with file browser"
```

---

## Task 5: On-hardware bench verification

**Files:** none (verification; fixes only if a check fails).

**Interfaces:** consumes the full assembly + CAN replay bench.

- [ ] **Step 1: Flash both boards**

```bash
arduino-cli upload -p <rp2040> --fqbn rp2040:rp2040:adafruit_feather_can:flash=8388608_2097152,freq=250 Arduino/can_receiver_display
arduino-cli upload -p <xiao>   --fqbn esp32:esp32:XIAO_ESP32S3:PSRAM=opi Arduino/xiao_ota_bridge
```
Join AP **MAZDASPEED**, open `http://192.168.4.1/`.

- [ ] **Step 2: List + navigate** — root shows the `.vlw` fonts + `/splash` folder with sizes and a free-space readout. Click `/splash` → frames listed; breadcrumb back to `/`.

- [ ] **Step 3: Upload a font** — in `/`, upload a tweaked `.vlw`; status `OK: wrote /<name>`, list size updates. Reboot → the new font renders on the display.

- [ ] **Step 4: Upload a frame** — enter `/splash`, upload a `.jpg`; it appears in the list.

- [ ] **Step 5: Delete** — delete a high-numbered `/splash` frame (arm 🗑 → ✓). Reboot → splash plays the shorter sequence (player stops at the gap).

- [ ] **Step 6: Corrupt-push safety** — begin an upload and interrupt it (or upload with a forced-bad byte); status shows FAIL (CRC_MISMATCH), and the **original file is unchanged** (size in list unchanged; `/.upload.tmp` not present).

- [ ] **Step 7: Firmware regression** — flash a normal `.bin` via the FIRMWARE card; it still uploads, reboots, and the version updates (confirms the block-receiver refactor didn't break firmware OTA).

- [ ] **Step 8: Display regression** — with CAN replay running, speed/gear/brightness/sport and the boot splash all behave as before.

- [ ] **Step 9: Record results**

```bash
git commit --allow-empty -m "test(fs): bench-verified list/upload/delete + firmware regression"
```

---

## Self-Review

**Spec coverage:** 5 message types + helpers → Task 1. Factored block receiver + list/write/delete dispatch + temp-then-rename + path validation → Task 2. `/api/list|upload|delete` + JSON + stream refactor → Task 3. Dark two-column dashboard with browse/upload/delete/free-space → Task 4. Tier-1 host tests → Task 1; Tier-2 bench (list/nav/upload font+frame/delete/corrupt-safety/firmware regression) → Task 5; Tier-3 display regression → Task 5 Step 8. Edge cases (corrupt→original intact, overwrite via rename, hidden temp, bad path rejected, no-response handling) → Tasks 2–4. **All spec sections map to tasks.**

**Placeholder scan:** No TBD/"handle errors"/"similar to Task N". `<rp2040>`/`<xiao>` in Task 5 are ports the operator fills from `arduino-cli board list` — operational, not code placeholders.

**Type/name consistency:** `list_entry_encode/decode`, `writehdr_encode/decode`, `OTA_MAX_NAME`, new `MsgType`/`ResultCode` values defined in Task 1 and used identically in Tasks 2–3. `receive_blocks(len,crc,sink,ctx,cb)` defined in Task 2 and used by both `receive_and_stage` and `handle_write`. `stream_blocks(buf,len)` defined in Task 3 Step 1, reused by firmware + `/api/upload`. Endpoints `/api/list|upload|delete` registered in Task 3, called by the page in Task 4. `header_encode/decode` reused for `LIST_END` (total/used) consistently on both sides. Frame payloads stay within `MAX_PAYLOAD=64` per Global Constraints.
