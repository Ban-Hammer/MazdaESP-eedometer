#include "ota_update.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <FS.h>
#include <LittleFS.h>

#if __has_include("secrets.h")
#include "secrets.h"
#endif
#ifndef OTA_AP_SSID
#define OTA_AP_SSID "MX5-Speedo"
#endif
#ifndef OTA_AP_PASS
#define OTA_AP_PASS "changeme123"
#endif

#define OTA_AP_BOOT_WINDOW_MS  (10UL * 60UL * 1000UL)  // AP may only start in the first 10 min
#define OTA_AP_IDLE_MS         (2UL * 60UL * 1000UL)   // stop AP after 2 min with no client

namespace {

WebServer          server(80);
ota::UiCallback    g_cb = nullptr;
const char*        g_ver = "?";

bool      g_apUp = false;
uint32_t  g_lastActive = 0;

// firmware upload progress
size_t    g_total = 0, g_got = 0;
uint8_t   g_lastPct = 255;
bool      g_fwErr = false;

// file upload state
File      g_upFile;
String    g_upPath;
bool      g_upErr = false;
const char* TMP_PATH = "/.upload.tmp";

const char PAGE[] PROGMEM = R"HTML(<!doctype html>
<meta name=viewport content="width=device-width,initial-scale=1">
<title>Speedo update</title>
<style>body{font-family:system-ui;margin:1.2em;max-width:36em}button{margin-left:.5em}pre{background:#eee;padding:.5em;overflow:auto}</style>
<h2>MX-5 speedometer</h2>
<p>Firmware <b>%VER%</b></p>
<h3>Firmware (.bin)</h3>
<input type=file id=fw accept=.bin><button onclick="send('/update',fw.files[0],'')">Flash</button>
<h3>Files (fonts, splash frames)</h3>
Folder <input id=dir value="/" size=10>
<input type=file id=fs multiple><button onclick="many()">Upload</button>
<pre id=out></pre>
<h3>On device</h3><pre id=ls>...</pre><button onclick="ls()">Refresh</button>
<p>Delete path <input id=del size=24><button onclick="rm()">Delete</button></p>
<script>
function send(url,file,path,cb){
  if(!file){out.textContent='pick a file first';return}
  var f=new FormData();f.append('f',file,file.name);
  var x=new XMLHttpRequest();
  x.open('POST',url+(path?'?path='+encodeURIComponent(path):''));
  x.upload.onprogress=e=>out.textContent=file.name+' '+Math.round(100*e.loaded/e.total)+'%';
  x.onload=()=>{out.textContent=file.name+': '+x.responseText;cb&&cb()};
  x.onerror=()=>{out.textContent=file.name+': connection lost (rebooting?)'};
  x.send(f)}
function many(){var d=dir.value;if(!d.endsWith('/'))d+='/';
  var q=[...fs.files];(function next(){var f=q.shift();if(!f){ls();return}send('/upload',f,d+f.name,next)})()}
function ls(){fetch('/ls').then(r=>r.text()).then(t=>document.getElementById('ls').textContent=t)}
function rm(){fetch('/delete?path='+encodeURIComponent(del.value),{method:'POST'}).then(r=>r.text()).then(t=>{out.textContent=t;ls()})}
ls();
</script>)HTML";

bool validPath(const String& p) {
  return p.length() > 1 && p.length() <= 63 && p[0] == '/' && p.indexOf("..") < 0;
}

// Create every parent folder of `path` (LittleFS.mkdir fails harmlessly if present).
void mkParents(const String& path) {
  for (int i = 1; i < (int)path.length(); ++i) {
    if (path[i] == '/') LittleFS.mkdir(path.substring(0, i));
  }
}

void report(ota::Ui phase, uint8_t pct, const char* msg = nullptr) {
  if (g_cb) g_cb(phase, pct, msg);
}

// ---- routes ------------------------------------------------------------------
void handleRoot() {
  String page = FPSTR(PAGE);
  page.replace("%VER%", g_ver);
  server.send(200, "text/html", page);
}

void listDir(const String& dir, String& out, int depth) {
  File d = LittleFS.open(dir);
  if (!d || !d.isDirectory()) return;
  for (File f = d.openNextFile(); f; f = d.openNextFile()) {
    String name = f.name();
    String full = name.startsWith("/") ? name : (dir == "/" ? "/" + name : dir + "/" + name);
    if (f.isDirectory()) { if (depth > 0) listDir(full, out, depth - 1); }
    else out += full + "  " + String((unsigned)f.size()) + "\n";
  }
}

void handleLs() {
  String out;
  listDir("/", out, 2);
  if (out.isEmpty()) out = "(empty)\n";
  server.send(200, "text/plain", out);
}

void handleDelete() {
  String p = server.arg("path");
  if (!validPath(p)) { server.send(400, "text/plain", "bad path"); return; }
  server.send(LittleFS.remove(p) ? 200 : 404, "text/plain", LittleFS.exists(p) ? "failed" : "deleted");
}

// Firmware: body is processed chunk by chunk in the upload handler.
void onFwUpload() {
  HTTPUpload& up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    g_fwErr = false; g_got = 0; g_lastPct = 255;
    g_total = server.header("Content-Length").toInt();
    report(ota::Ui::STARTED, 0);
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) { g_fwErr = true; Update.printError(Serial); }
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (g_fwErr) return;
    if (Update.write(up.buf, up.currentSize) != up.currentSize) { g_fwErr = true; Update.printError(Serial); }
    g_got += up.currentSize;
    if (g_total > 0) {
      uint8_t pct = (uint8_t)min<size_t>(100, g_got * 100 / g_total);
      if (pct != g_lastPct) { g_lastPct = pct; report(ota::Ui::PROGRESS, pct); }
    }
  } else if (up.status == UPLOAD_FILE_END) {
    if (!g_fwErr && !Update.end(true)) { g_fwErr = true; Update.printError(Serial); }
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    Update.abort(); g_fwErr = true;
  }
}

void onFwDone() {
  bool ok = !g_fwErr && !Update.hasError() && g_got > 0;
  server.sendHeader("Connection", "close");
  server.send(ok ? 200 : 500, "text/plain", ok ? "OK - rebooting" : "FAILED");
  if (ok) { report(ota::Ui::SUCCESS, 100); delay(800); ESP.restart(); }
  else    { report(ota::Ui::FAILED, 0, "Update failed"); }
}

// Files: written to a temp file then renamed, so a dropped upload never leaves a
// half-written font behind.
void onFileUpload() {
  HTTPUpload& up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    g_upErr = false;
    g_upPath = server.arg("path");
    if (!validPath(g_upPath)) { g_upErr = true; return; }
    mkParents(g_upPath);
    LittleFS.remove(TMP_PATH);
    g_upFile = LittleFS.open(TMP_PATH, "w");
    if (!g_upFile) g_upErr = true;
    report(ota::Ui::STARTED, 0);
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (!g_upErr && g_upFile && g_upFile.write(up.buf, up.currentSize) != up.currentSize) g_upErr = true;
  } else if (up.status == UPLOAD_FILE_END) {
    if (g_upFile) g_upFile.close();
    if (!g_upErr) {
      LittleFS.remove(g_upPath);
      if (!LittleFS.rename(TMP_PATH, g_upPath)) g_upErr = true;
    }
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    if (g_upFile) g_upFile.close();
    LittleFS.remove(TMP_PATH);
    g_upErr = true;
  }
}

void onFileDone() {
  server.send(g_upErr ? 500 : 200, "text/plain", g_upErr ? "FAILED" : "OK");
  report(g_upErr ? ota::Ui::FAILED : ota::Ui::SUCCESS, 100, g_upErr ? "Upload failed" : nullptr);
}

// ---- AP lifecycle ---------------------------------------------------------------
void startAp() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(OTA_AP_SSID, OTA_AP_PASS);
  server.begin();
  g_apUp = true;
  g_lastActive = millis();
  Serial.printf("OTA AP up: %s  http://%s/\n", OTA_AP_SSID, WiFi.softAPIP().toString().c_str());
}

void stopAp() {
  server.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  g_apUp = false;
  Serial.println("OTA AP down");
}

}  // namespace

namespace ota {

void begin(const char* fw_version) {
  g_ver = fw_version;
  static const char* hdrs[] = {"Content-Length"};
  server.collectHeaders(hdrs, 1);
  server.on("/", HTTP_GET, handleRoot);
  server.on("/ls", HTTP_GET, handleLs);
  server.on("/delete", HTTP_POST, handleDelete);
  server.on("/update", HTTP_POST, onFwDone, onFwUpload);
  server.on("/upload", HTTP_POST, onFileDone, onFileUpload);
}

void service(UiCallback cb, bool allowed) {
  g_cb = cb;
  uint32_t now = millis();
  if (g_apUp) {
    bool clients = WiFi.softAPgetStationNum() > 0;
    if (clients) g_lastActive = now;
    if (!clients && (!allowed || now - g_lastActive > OTA_AP_IDLE_MS)) { stopAp(); return; }
    server.handleClient();
  } else if (allowed && now < OTA_AP_BOOT_WINDOW_MS) {
    startAp();
  }
}

}  // namespace ota
