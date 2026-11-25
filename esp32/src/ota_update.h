#pragma once
#include <stdint.h>

// Native ESP32 wireless update path (replaces the XIAO bridge + UART protocol).
// The ESP32 hosts a WPA2 SoftAP with a small web page at http://192.168.4.1/ that
// can flash a firmware .bin and upload/delete LittleFS files (fonts, splash frames).

namespace ota {
  enum class Ui : uint8_t { STARTED, PROGRESS, SUCCESS, FAILED };
  typedef void (*UiCallback)(Ui phase, uint8_t pct, const char* msg);

  // Register web routes. Cheap; does not start Wi-Fi.
  void begin(const char* fw_version);

  // Call every loop(). `allowed` should be true only when it is safe to run Wi-Fi
  // (e.g. the car is stationary). The AP only starts within the first
  // OTA_AP_BOOT_WINDOW_MS after boot, and shuts down again after
  // OTA_AP_IDLE_MS with no connected client. Reboot to re-open the window.
  // While a firmware/file upload is running this blocks inside the web handler
  // and calls `cb` so the caller can draw progress. On a committed firmware
  // update it reboots and does not return. `cb` may be null.
  void service(UiCallback cb, bool allowed);
}
