// XIAO ESP32-S3 in-car OTA bridge.
// Hosts a WPA2 SoftAP + upload page; relays a firmware image to the RP2040 CAN
// display over UART (Serial1) using the shared ota_protocol framing.
//
// Board: XIAO_ESP32S3.  Tools -> PSRAM: "OPI PSRAM" (image is buffered in PSRAM).
// Wiring: XIAO D6(TX) -> RP2040 GP1(RX), XIAO D7(RX) <- RP2040 GP0(TX), GND-GND.
// See docs/superpowers/specs/2026-07-18-rp2040-wireless-ota-design.md

#include <WiFi.h>
#include <WebServer.h>
#include <ota_protocol.h>
#include "secrets.h"
#include "upload_page.h"

using namespace otap;

static const unsigned long LINK_BAUD = 921600;
// XIAO ESP32-S3: D6 = TX, D7 = RX.
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
  while ((int32_t)(end - millis()) > 0) {
    while (Serial1.available()) {
      if (frameparser_push(&p, (uint8_t)Serial1.read(), out) == ParseStatus::FRAME_READY)
        return true;
    }
  }
  return false;
}

static void drain_rx() { while (Serial1.available()) Serial1.read(); }

static String flash_to_rp2040() {
  drain_rx();
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
  drain_rx();
  send_frame(MsgType::VERSION, nullptr, 0);   // request version
  ParsedFrame f;
  if (recv_frame(&f, 1500) && f.type == MsgType::VERSION) {
    String v; for (size_t i = 0; i < f.payload_len; ++i) v += (char)f.payload[i];
    server.send(200, "text/plain", v);
  } else {
    server.send(200, "text/plain", "(display not responding)");
  }
}

// Multipart upload: buffer file into PSRAM, then (on done) flash.
static void handleUploadData() {
  HTTPUpload& u = server.upload();
  if (u.status == UPLOAD_FILE_START) {
    g_len = 0;
  } else if (u.status == UPLOAD_FILE_WRITE) {
    if (g_len + u.currentSize <= g_cap) {
      memcpy(g_img + g_len, u.buf, u.currentSize);
      g_len += u.currentSize;
    } else {
      g_len = g_cap + 1;   // mark overflow; handleUploadDone reports it
    }
  }
}
static void handleUploadDone() {
  if (g_len == 0)      { server.send(400, "text/plain", "FAIL: empty upload"); return; }
  if (g_len > g_cap)   { server.send(413, "text/plain", "FAIL: image too large"); return; }
  String r = flash_to_rp2040();
  server.send(200, "text/plain", r);
}

void setup() {
  Serial.begin(115200);
  Serial1.begin(LINK_BAUD, SERIAL_8N1, PIN_RX, PIN_TX);

  g_cap = 2 * 1024 * 1024;                 // 2 MB ceiling (image ~1 MB)
  g_img = (uint8_t*)ps_malloc(g_cap);
  if (!g_img) { g_cap = 0; Serial.println("PSRAM alloc failed — enable OPI PSRAM"); }

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);           // WPA2-PSK (pass >= 8 chars)
  Serial.print("AP up: "); Serial.println(WiFi.softAPIP());

  server.on("/", HTTP_GET, handleRoot);
  server.on("/ping", HTTP_GET, handlePing);
  server.on("/update", HTTP_POST, handleUploadDone, handleUploadData);
  server.begin();
}

void loop() { server.handleClient(); }
