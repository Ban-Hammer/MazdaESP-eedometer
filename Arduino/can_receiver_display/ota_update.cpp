#include "ota_update.h"
#include <Arduino.h>
#include <Updater.h>          // arduino-pico OTA Updater (part of the Pico core)
#include <ota_protocol.h>
#include <string.h>

using namespace otap;

namespace {
  const char*  g_ver = "?";
  FrameParser  g_parser;

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
  // Returns true iff the whole image was staged AND the CRC matched. On any failure it
  // sends the appropriate RESULT and returns false (nothing committed).
  bool receive_and_stage(uint32_t img_len, uint32_t img_crc, ota::UiCallback cb) {
    if (img_len == 0)              { send_result(ResultCode::BAD_LENGTH); return false; }
    if (!Update.begin(img_len))    { send_result(ResultCode::FLASH_ERROR); return false; }
    uint32_t crc = crc32_init();
    uint32_t remaining = img_len;
    uint8_t  buf[512];
    uint8_t  last_pct = 255;
    while (remaining) {
      size_t chunk = remaining < sizeof(buf) ? remaining : sizeof(buf);
      size_t got = read_exact(buf, chunk, BYTE_TIMEOUT_MS);
      if (got == 0) { send_result(ResultCode::TIMEOUT); return false; }      // stall
      if (Update.write(buf, got) != got) { send_result(ResultCode::FLASH_ERROR); return false; }
      crc = crc32_update(crc, buf, got);
      remaining -= got;
      uint8_t pct = (uint8_t)(100u * (uint64_t)(img_len - remaining) / img_len);
      if (cb && pct != last_pct) { cb(ota::Ui::PROGRESS, pct, "Updating"); last_pct = pct; }
    }
    if (crc32_final(crc) != img_crc) { send_result(ResultCode::CRC_MISMATCH); return false; }
    return true;
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
      default:
        break;
    }
  }
}

} // namespace ota
