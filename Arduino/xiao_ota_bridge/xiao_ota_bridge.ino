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

static String flash_to_rp2040() {
  drain_rx();
  send_frame(MsgType::ENTER_UPDATE, nullptr, 0);
  ParsedFrame f;
  if (!recv_frame(&f, 3000) || f.type != MsgType::ACK) return "FAIL: no ACK from display";

  uint8_t hdr[8]; header_encode(g_len, crc32(g_img, g_len), hdr);
  send_frame(MsgType::HEADER, hdr, 8);
  if (!recv_frame(&f, 5000) || f.type != MsgType::ACK) return "FAIL: display not ready after header";

  String err = stream_blocks(g_img, g_len);
  if (err.length()) return "FAIL: " + err;

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
  if (g_cap == 0)      { server.send(500, "text/plain", "FAIL: XIAO PSRAM unavailable - reflash XIAO with PSRAM=opi"); return; }
  if (g_len == 0)      { server.send(400, "text/plain", "FAIL: empty upload"); return; }
  if (g_len > g_cap)   { server.send(413, "text/plain", "FAIL: image too large"); return; }
  String r = flash_to_rp2040();
  server.send(200, "text/plain", r);
}

// ---- LittleFS file API (list / upload / delete) ----

static void jsonEscape(String& out, const char* s) {
  for (; *s; ++s) { if (*s == '"' || *s == '\\') out += '\\'; out += *s; }
}

static void handleList() {
  String path = server.hasArg("path") ? server.arg("path") : "/";
  drain_rx();
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

  drain_rx();
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
  drain_rx();
  send_frame(MsgType::DELETE_FILE, (const uint8_t*)path.c_str(), path.length());
  ParsedFrame f;
  if (!recv_frame(&f, 3000) || f.type != MsgType::RESULT || f.payload_len < 1) { server.send(200, "text/plain", "FAIL: no result"); return; }
  ResultCode rc = (ResultCode)f.payload[0];
  server.send(200, "text/plain", rc == ResultCode::OK ? "OK" : ("FAIL: code " + String((int)rc)));
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
  server.on("/api/list", HTTP_GET, handleList);
  server.on("/api/upload", HTTP_POST, handleFileUploadDone, handleFileUploadData);
  server.on("/api/delete", HTTP_POST, handleDelete);
  server.begin();
}

void loop() { server.handleClient(); }
