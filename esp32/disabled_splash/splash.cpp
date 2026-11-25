#include "splash.h"
#include <Arduino.h>
#include <TFT_eSPI.h>
#include <TJpg_Decoder.h>
#include <FS.h>
#include <LittleFS.h>

namespace {
  TFT_eSPI* g_tft = nullptr;
  const int MAX_FRAMES = 300;

  // TJpg_Decoder output callback: push one decoded MCU block to the panel.
  bool tftOutput(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bitmap) {
    if (!g_tft) return false;
    if (y >= g_tft->height()) return false;   // block off-screen: stop this frame
    g_tft->pushImage(x, y, w, h, bitmap);
    return true;                              // keep decoding
  }
}

namespace splash {

void play(TFT_eSPI& tft, uint8_t fps, InterFrame between) {
  if (!LittleFS.exists("/splash/000.jpg")) return;   // nothing to play

  g_tft = &tft;
  TJpgDec.setJpgScale(1);
  TJpgDec.setSwapBytes(true);          // correct colour order for TFT_eSPI pushImage
  TJpgDec.setCallback(tftOutput);

  const uint32_t frameInterval = (fps > 0) ? (1000u / fps) : 0;
  char path[24];
  for (int i = 0; i < MAX_FRAMES; ++i) {
    snprintf(path, sizeof(path), "/splash/%03d.jpg", i);
    if (!LittleFS.exists(path)) break;               // past the last frame

    uint32_t t0 = millis();
    TJpgDec.drawFsJpg(0, 0, path, LittleFS);         // a bad frame just draws nothing

    if (between && between()) break;                  // abort (moving, or OTA took over)

    uint32_t elapsed = millis() - t0;                 // pace to target fps
    if (elapsed < frameInterval) delay(frameInterval - elapsed);
  }

  tft.fillScreen(TFT_BLACK);   // clean slate for the normal display
  g_tft = nullptr;
}

} // namespace splash
