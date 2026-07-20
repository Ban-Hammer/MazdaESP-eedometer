#include "ota_update.h"
#include <Arduino.h>
#include <Updater.h>          // arduino-pico OTA Updater (part of the Pico core)
#include <FS.h>
#include <LittleFS.h>
#include <ota_protocol.h>
#include <string.h>

using namespace otap;

namespace {
  const char*  g_ver = "?";
  FrameParser  g_parser;

  const char*    TMP_PATH = "/.upload.tmp";
  const uint32_t BYTE_TIMEOUT_MS  = 3000;   // stall detection during data phase
  const uint32_t FRAME_TIMEOUT_MS = 5000;   // waiting for HEADER after ACK

  bool valid_path(const char* p) {
    if (!p || p[0] != '/') return false;
    if (strstr(p, "..")) return false;
    return strlen(p) <= 63;
  }

  // A sink consumes one decoded block; returns true on success.
  typedef bool (*BlockSink)(void* ctx, const uint8_t* buf, size_t n);

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

  // Receive `len` raw bytes in OTA_BLOCK chunks under per-block flow control, feeding
  // each block to `sink` and accumulating CRC32. Sends an ACK after each written block
  // (the sender waits for it, so no bytes arrive during the write and the FIFO can't
  // overrun). Caller sends the header-ACK and opens/finalizes the sink. Returns:
  //   OK on full receive + crc match; CRC_MISMATCH; TIMEOUT (stall); WRITE_ERROR (sink).
  ResultCode receive_blocks(uint32_t len, uint32_t crc, BlockSink sink, void* ctx,
                            ota::UiCallback cb) {
    static uint8_t buf[OTA_BLOCK];   // static: too big for this call's stack
    uint32_t acc = crc32_init(), remaining = len; uint8_t last_pct = 255;
    while (remaining) {
      size_t chunk = remaining < OTA_BLOCK ? remaining : OTA_BLOCK;
      if (read_exact(buf, chunk, BYTE_TIMEOUT_MS) != chunk) return ResultCode::TIMEOUT;
      if (!sink(ctx, buf, chunk)) return ResultCode::WRITE_ERROR;
      acc = crc32_update(acc, buf, chunk);
      remaining -= chunk;
      send_frame(MsgType::ACK, nullptr, 0);   // block written, send the next one
      if (cb) { uint8_t pct = (uint8_t)(100u * (uint64_t)(len - remaining) / len);
                if (pct != last_pct) { cb(ota::Ui::PROGRESS, pct, "Updating"); last_pct = pct; } }
    }
    return (crc32_final(acc) == crc) ? ResultCode::OK : ResultCode::CRC_MISMATCH;
  }

  bool updateSink(void*, const uint8_t* b, size_t n) { return Update.write((uint8_t*)b, n) == n; }
  bool fileSink(void* ctx, const uint8_t* b, size_t n) { return ((File*)ctx)->write(b, n) == n; }

  // Firmware data phase: stage the incoming image into flash via the Updater. Keeps the
  // same header-ACK-then-blocks wire behavior. Returns true iff staged + CRC matched.
  bool receive_and_stage(uint32_t img_len, uint32_t img_crc, ota::UiCallback cb) {
    if (img_len == 0)           { send_result(ResultCode::BAD_LENGTH); return false; }
    if (!Update.begin(img_len)) { send_result(ResultCode::FLASH_ERROR); return false; }
    send_frame(MsgType::ACK, nullptr, 0);                 // header-ack: send block 0
    ResultCode rc = receive_blocks(img_len, img_crc, updateSink, nullptr, cb);
    if (rc != ResultCode::OK) { send_result(rc); return false; }
    return true;
  }

  // ---- LittleFS file operations (no reboot; do not touch the TFT) ----

  void handle_list(const char* path) {
    Dir d = LittleFS.openDir(path);
    uint8_t p[MAX_PAYLOAD];
    while (d.next()) {
      String fn = d.fileName();
      const char* nm = fn.c_str();
      const char* slash = strrchr(nm, '/'); if (slash) nm = slash + 1;   // basename
      if (!strcmp(nm, TMP_PATH + 1)) continue;                           // hide temp file
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
    if (!valid_path(path))      { send_result(ResultCode::WRITE_ERROR); return; }
    if (!LittleFS.exists(path)) { send_result(ResultCode::NOT_FOUND); return; }
    send_result(LittleFS.remove(path) ? ResultCode::OK : ResultCode::WRITE_ERROR);
  }

  // Runs after an ENTER_UPDATE frame. Drives header -> data -> verify -> commit.
  void run_session(ota::UiCallback cb) {
    State st = State::IDLE;
    Transition t = sm_step(st, Event::RX_ENTER);       // -> WAIT_HEADER, SEND_ACK
    st = t.next;
    if (t.action == Action::SEND_ACK) send_frame(MsgType::ACK, nullptr, 0);
    if (cb) cb(ota::Ui::STARTED, 0, "Updating");

    // Wait for the HEADER frame.
    uint32_t start = millis(); ParsedFrame f; bool got_header = false;
    while (millis() - start < FRAME_TIMEOUT_MS) {
      if (!Serial1.available()) continue;
      if (frameparser_push(&g_parser, (uint8_t)Serial1.read(), &f) == ParseStatus::FRAME_READY
          && f.type == MsgType::HEADER && f.payload_len == 8) { got_header = true; break; }
    }
    if (!got_header) {
      send_result(ResultCode::TIMEOUT);
      if (cb) cb(ota::Ui::FAILED, 0, "No header");
      return;
    }

    uint32_t img_len, img_crc; header_decode(f.payload, &img_len, &img_crc);
    t = sm_step(st, Event::RX_HEADER); st = t.next;    // -> RECEIVING, BEGIN_FLASH

    bool ok = receive_and_stage(img_len, img_crc, cb);
    if (!ok) { sm_step(st, Event::CRC_FAIL); if (cb) cb(ota::Ui::FAILED, 0, "Rejected"); return; }

    st = sm_step(st, Event::DATA_COMPLETE).next;       // -> VERIFYING
    t = sm_step(st, Event::CRC_OK);                    // -> DONE, COMMIT
    if (t.action == Action::COMMIT) {
      if (Update.end()) {                              // finalize: schedule OTA on reboot
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
  Serial1.setFIFOSize(1024);                           // default is 32; absorb RX jitter
  Serial1.begin(baud);                                 // Feather RP2040: GP0=TX, GP1=RX
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
      case MsgType::VERSION:                            // request-for-version
        send_frame(MsgType::VERSION, (const uint8_t*)g_ver, strlen(g_ver));
        break;
      case MsgType::ENTER_UPDATE:
        run_session(cb);                                // blocks; reboots on success
        frameparser_reset(&g_parser);
        return;
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
      default:
        break;
    }
  }
}

} // namespace ota
