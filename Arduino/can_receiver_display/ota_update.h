#pragma once
#include <stdint.h>

// RP2040 side of the wireless OTA path. Receives a firmware image over Serial1
// from the XIAO ESP32-S3 bridge and self-flashes via the arduino-pico Updater.
// See docs/superpowers/specs/2026-07-18-rp2040-wireless-ota-design.md

namespace ota {
  enum class Ui : uint8_t { STARTED, PROGRESS, SUCCESS, FAILED };
  typedef void (*UiCallback)(Ui phase, uint8_t pct, const char* msg);

  // Initialize the UART listener. Call this FIRST in setup(), before CAN/TFT/fonts,
  // so the board stays remotely updatable even if later init crashes.
  void begin(const char* fw_version, unsigned long baud = 921600);

  // Poll once per loop(). Non-blocking unless a session is running (during which
  // it blocks to receive the image while the caller has paused the display).
  // On a committed update it reboots and does not return. `cb` may be null.
  void service(UiCallback cb);
}
