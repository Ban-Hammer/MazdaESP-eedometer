#pragma once
#include <stdint.h>

class TFT_eSPI;   // forward declaration (used by reference only)

namespace splash {
  // Return true to abort playback early (checked once per frame).
  typedef bool (*InterFrame)();

  // Play /splash/000.jpg, 001.jpg, … from LittleFS at ~fps, calling `between` once per
  // frame. No-op if /splash/000.jpg is absent. Bounded (stops at first gap, capped at
  // MAX_FRAMES). Leaves the screen black on return.
  void play(TFT_eSPI& tft, uint8_t fps, InterFrame between);
}
