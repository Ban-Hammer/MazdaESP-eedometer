#include <TFT_eSPI.h>
#include <mcp_can.h>
#include <SPI.h>
#include <FS.h>
#include <LittleFS.h>
#include "ota_update.h"
#include "splash.h"

#define FW_VERSION "v0.9.9"

#define SPEED_FONT "speed112_2"
#define GEAR_FONT "gear96"
#define DM_FONT "dm40"
#define SPORT_MODE_FONT "mazdaspeed18"

// ---- DEMO MODE ---------------------------------------------------------------
// When defined, the display runs a self-contained animation (speed sweeps,
// gear cycling, sport-mode label) instead of reading the CAN bus. Comment this
// line out to return to normal CAN-driven operation.
// #define DEMO_MODE
// ------------------------------------------------------------------------------

#define CS_PIN 19

// ---- Backlight (TFT BLK pin, PWM dimming) ------------------------------------
#define BL_PIN 10        // GP10 -> TFT BLK (free; not SPI GP2-6 or CAN GP19/22)

// Dashboard illumination on CAN 0x09F (SavvyCAN D-columns are 1-based; Dn = rxBuf[n-1]):
//   D2 (rxBuf[1]) = night dimmer level, 0x0D..0x64
//   D6 (rxBuf[5]) = day-mode byte, 0x48 (night) -> 0xC8 (day)
#define BRIGHT_ID           0x09F
#define BRIGHT_LEVEL_IDX    1      // SavvyCAN D2 = night dimmer level
#define DAY_MODE_IDX        5      // SavvyCAN D6 = day-mode byte
#define DAY_MODE_THRESHOLD  0xA0   // D6 at/above this => daylight => full brightness
#define NIGHT_LEVEL_MIN     0x0D   // dimmest dimmer-wheel value
#define NIGHT_LEVEL_MAX     0x64   // brightest dimmer-wheel value
#define NIGHT_PWM_MIN       1      // display PWM at dimmest night (readable floor)
#define NIGHT_PWM_MAX       50    // display PWM at brightest night

// Sport mode on CAN 0x09E, last byte (SavvyCAN D8 = rxBuf[7]): bit 0x10 = on.
#define SPORT_ID            0x09E
#define SPORT_IDX           7      // SavvyCAN D8 = last byte
#define SPORT_MASK          0x10   // bit set => sport mode engaged

// ---- Display stabilization (stop readout flicker at value boundaries) --------
// SPEED_HYST: raw-count (0.01 km/h) deadband past each km/h rounding boundary, so a
//   real speed sitting on an x.5 value doesn't flip the digit every frame.
// GEAR_DEBOUNCE: 0x228 (D-mode) frames a new gear number must persist (~20ms each)
//   before it shows, to ignore brief 2<->3 hunting at a shift point.
#define SPEED_HYST          25
#define GEAR_DEBOUNCE       4

// ---- Screen orientation & label layout ---------------------------------------
// Every on-screen position lives here so the display can be re-aimed without
// hunting through the draw code. Panel is 240x240 (see TFT_eSPI User_Setup.h).
#define SCREEN_ROTATION     2      // tft.setRotation(): 0/1/2/3 = 0/90/180/270 deg

// Speed digits (top). NUDGE_X shifts wide / leading-"1" numbers left so the
// leading digit stays put (see drawSpeedGear()).
#define SPEED_X             0
#define SPEED_Y             15
#define SPEED_NUDGE_X       (-25)

// Big gear glyph (bottom), gear96 draws ~67px tall from GEAR_Y. GEAR_Y is kept a
// little high of centre so the glyph clears the MAZDASPEED band above and the
// panel's bottom edge. (The old bottom-crop was a font-metrics bug: gear96.vlw
// declared descent -1 while its glyphs dip 1px below the baseline, so
// printToSprite built a 2px-too-short sprite; fixed by patching the .vlw descent
// to 9 -- not a position bug.)
#define GEAR_X              0
#define GEAR_Y              125    // top of the big gear letter/number
#define GEAR_M_Y            (GEAR_Y + 35)  // manual "M" indicator, below the D

// Orange MAZDASPEED label band (between the speed and the gear).
#define SPORT_BAND_X        0
#define SPORT_BAND_Y        110
#define SPORT_BAND_W        240
#define SPORT_BAND_H        15
#define SPORT_TEXT_X        10
#define SPORT_TEXT_Y        10

long unsigned int rxId;
unsigned char len = 0;
unsigned char rxBuf[8];

unsigned int speedRaw = 0;
int speed = 0;
int prevSpeed = -1;

unsigned int gearRaw = 0;
String gear = "P";
String prevGear = "";

unsigned int current_gearRaw = 0;
int current_gear = 1;
int prevCurrentGear = -1;
int gearCand = -1;      // gear-number debounce: candidate value + frames it has held
int gearCandN = 0;

bool manual_gear = false;
bool prevManual = false;
bool sport = false;
int prevSportState = -1;  // -1 = unknown, 0 = off, 1 = on

// Backlight brightness state (0-255 PWM duty). Starts full so the boot screen
// is readable.
uint8_t currentBrightness = 255;
uint8_t targetBrightness  = 255;   // brightness we are ramping toward

#define RAMP_INTERVAL_MS 25    // how often the ramp advances
#define RAMP_STEP        4     // PWM counts per ramp tick
unsigned long lastRampMillis = 0;

#define CAN0_INT 22
MCP_CAN CAN0(CS_PIN);

TFT_eSPI tft = TFT_eSPI();
TFT_eSprite speed_spr = TFT_eSprite(&tft);
TFT_eSprite gear_spr = TFT_eSprite(&tft);
TFT_eSprite mazdaspeed = TFT_eSprite(&tft);
TFT_eSprite dm = TFT_eSprite(&tft);

// Draw OTA status to the TFT while an update session runs (passed to ota::service).
void otaUi(ota::Ui phase, uint8_t pct, const char* msg) {
  switch (phase) {
    case ota::Ui::STARTED:
      tft.fillScreen(TFT_BLACK);
      tft.setTextColor(TFT_WHITE, TFT_BLACK);
      tft.setTextDatum(MC_DATUM);
      tft.drawString("Updating...", tft.width() / 2, tft.height() / 2 - 20, 4);
      tft.setTextDatum(TL_DATUM);
      break;
    case ota::Ui::PROGRESS: {
      int w = tft.width() - 40, x = 20, y = tft.height() / 2 + 10;
      tft.drawRect(x, y, w, 16, TFT_WHITE);
      tft.fillRect(x + 1, y + 1, (w - 2) * pct / 100, 14, TFT_GREEN);
      break;
    }
    case ota::Ui::SUCCESS:
      tft.fillScreen(TFT_BLACK);
      tft.setTextDatum(MC_DATUM);
      tft.drawString("Rebooting...", tft.width() / 2, tft.height() / 2, 4);
      tft.setTextDatum(TL_DATUM);
      break;
    case ota::Ui::FAILED:
      tft.fillScreen(TFT_BLACK);
      tft.setTextColor(TFT_RED, TFT_BLACK);
      tft.setTextDatum(MC_DATUM);
      tft.drawString(msg ? msg : "Update failed", tft.width() / 2, tft.height() / 2, 4);
      tft.setTextDatum(TL_DATUM);
      delay(1500);
      // Force a full redraw of the normal display when the session resumes.
      prevSpeed = -1; prevGear = ""; prevCurrentGear = -1; prevSportState = -1;
      break;
  }
}

// Called once per splash frame: keep the board updatable, and abort the splash the
// moment real CAN data shows the car is moving.
static bool splashInterFrame() {
  rp2040.wdt_reset();                      // feed the watchdog during the boot splash
  ota::service(otaUi);                     // an OTA mid-splash takes over + reboots
  if (!digitalRead(CAN0_INT)) {            // non-blocking CAN peek
    CAN0.readMsgBuf(&rxId, &len, rxBuf);
    if (rxId == 0x202) {
      unsigned int raw = (rxBuf[2] << 8) | rxBuf[3];
      if ((raw + 50) / 100 > 0) return true;   // moving -> abort
    }
  }
  return false;
}

// Map a 0x09F frame to a target backlight PWM duty (0-255).
// Daylight mode -> full; otherwise scale the night dimmer level, clamped so the
// screen never goes fully dark.
uint8_t computeTargetBrightness(uint8_t level, uint8_t dayByte) {
  if (dayByte >= DAY_MODE_THRESHOLD) return 255;
  if (level < NIGHT_LEVEL_MIN) level = NIGHT_LEVEL_MIN;
  if (level > NIGHT_LEVEL_MAX) level = NIGHT_LEVEL_MAX;
  return (uint8_t)map(level, NIGHT_LEVEL_MIN, NIGHT_LEVEL_MAX,
                      NIGHT_PWM_MIN, NIGHT_PWM_MAX);
}

// Advance currentBrightness toward targetBrightness by at most RAMP_STEP each
// RAMP_INTERVAL_MS, then apply it. Self-throttling; safe to call every loop().
void updateBacklight() {
  unsigned long now = millis();
  if (now - lastRampMillis < RAMP_INTERVAL_MS) return;
  lastRampMillis = now;

  int cur = currentBrightness;
  int tgt = targetBrightness;
  if (cur < tgt)      cur += min(RAMP_STEP, tgt - cur);
  else if (cur > tgt) cur -= min(RAMP_STEP, cur - tgt);

  currentBrightness = (uint8_t)cur;
  analogWrite(BL_PIN, currentBrightness);
}

void setup() {
  // Update-mode-first: bring up the OTA listener before anything that could
  // crash (CAN/TFT/fonts), so the board stays remotely recoverable.
  ota::begin(FW_VERSION);

  // Hardware watchdog: auto-reboot if the firmware hangs for 8 s (max ~8.3 s on
  // RP2040), turning a wedge into a self-recovery instead of a USB reflash. Fed in
  // loop(), splashInterFrame() (per splash frame), and ota's block/header loops —
  // any blocking op that can run near 8 s must feed it (see wdt_reset calls).
  // Skipped in DEMO_MODE (its long animation delays would trip the 8 s window).
#ifndef DEMO_MODE
  rp2040.wdt_begin(8000);
#endif

  Serial.begin(115200);
  // while (!Serial) delay(10);

  if (!LittleFS.begin()) {
    Serial.println("LittleFS init failed!");
  } else {
    Serial.println("LittleFS available!");
    ota::cleanupStaging();   // sweep spent OTA leftovers (/firmware.bin, /otacommand.bin)
  }

  const char* fontFiles[] = {
    "/" SPEED_FONT ".vlw", "/" GEAR_FONT ".vlw",
    "/" DM_FONT ".vlw",    "/" SPORT_MODE_FONT ".vlw",
  };
  bool allFonts = true;
  for (const char* f : fontFiles) {
    if (!LittleFS.exists(f)) {
      Serial.print("Font missing: "); Serial.println(f);
      allFonts = false;
    }
  }
  if (allFonts) Serial.println("All fonts are there!");

  if (CAN0.begin(MCP_STDEXT, CAN_500KBPS, MCP_16MHZ) == CAN_OK) {
    Serial.println("MCP2515 Initialized Successfully!");
  } else {
    Serial.println("Error Initializing MCP2515...");
  }
  pinMode(CAN0_INT, INPUT_PULLUP);

  CAN0.init_Mask(0, 0, 0x02FF0000);
  CAN0.init_Filt(0, 0, 0x02020000);
  CAN0.init_Filt(1, 0, 0x02280000);

  // RXB1: exact 11-bit match, used to let 0x131 (manual-mode flag) through.
  CAN0.init_Mask(1, 0, 0x07FF0000);
  CAN0.init_Filt(2, 0, 0x01310000);
  // RXB1 also lets 0x09F (dashboard illumination / brightness) through.
  CAN0.init_Filt(3, 0, 0x009F0000);
  // RXB1 also lets 0x09E (sport-mode flag) through.
  CAN0.init_Filt(4, 0, 0x009E0000);

  CAN0.setMode(MCP_NORMAL);

  tft.init();
  tft.setRotation(SCREEN_ROTATION);
  tft.fillScreen(TFT_BLACK);

  // Backlight PWM: ~1 kHz, 8-bit range. The first updateBacklight() call in loop()
  // drives the pin; currentBrightness starts at full so the boot screen is visible.
  pinMode(BL_PIN, OUTPUT);
  analogWriteFreq(1000);
  analogWriteRange(255);

  speed_spr.setColorDepth(16);
  gear_spr.setColorDepth(16);
  dm.setColorDepth(16);
  mazdaspeed.setColorDepth(16);

  speed_spr.loadFont(SPEED_FONT, LittleFS);
  gear_spr.loadFont(GEAR_FONT, LittleFS);
  mazdaspeed.loadFont(SPORT_MODE_FONT, LittleFS);
  dm.loadFont(DM_FONT, LittleFS);

  speed_spr.setTextColor(TFT_WHITE, TFT_BLACK);
  gear_spr.setTextColor(TFT_WHITE, TFT_BLACK);
  mazdaspeed.setTextColor(TFT_ORANGE, TFT_BLACK);
  dm.setTextColor(TFT_WHITE, TFT_BLACK);

  // Boot splash. Backlight is otherwise first driven in loop()'s updateBacklight(),
  // which runs only after this, so turn it on here or the splash renders dark.
  analogWrite(BL_PIN, 255);
  splash::play(tft, 15, splashInterFrame);   // ~2s MJPEG splash (aborts if moving)

  // Fade the initial speed/gear in from black to continue the splash's fade-out.
  // This ramps the glyph COLOURS (not the backlight), so brightness is unchanged.
  fadeInFromBlack();
}

// Redraw speed and gear from the current global state, only when they change.
void drawSpeedGear() {
  if (speed != prevSpeed) {
    // The "1" glyph is narrow, so numbers with a leading 1 (10-19 and 100-199) plus
    // the wider 3-digit speeds get nudged left via the cursor, keeping the leading
    // "1" in a consistent position. The leading space still clears the left side.
    bool nudgeLeft = (speed >= 100) || (speed >= 10 && speed < 20);
    tft.setCursor(nudgeLeft ? SPEED_NUDGE_X : SPEED_X, SPEED_Y);
    speed_spr.printToSprite(" " + (String)(speed) + "          ");
    prevSpeed = speed;
  }

  if (gear != prevGear
      || (gear == "D" && current_gear != prevCurrentGear)
      || (gear == "D" && manual_gear != prevManual)) {
    tft.setCursor(GEAR_X, GEAR_Y);
    if (gear == "D") {
      // gear96 has no "D": draw the big gear NUMBER (space-padded to clear the row,
      // and shifted right to leave room for the D/M column on the left).
      gear_spr.printToSprite("   " + (String)(current_gear) + "        ");
      // Auto/manual indicator column from dm40: "D" on top, "M" under it.
      // The gear-number sprite above does NOT reach the "M" row, so a released
      // manual gate would leave the "M" on screen. Clear the M cell every redraw
      // (its exact footprint via textWidth/fontHeight), then draw "M" only in
      // manual mode. Order matters: clear the M row here, *before* the "D", so the
      // "D" repaints its overlapping bottom edge afterwards.
      // NOTE: single-char printToSprite renders nothing here; the string must be
      // space-padded (like the gear/speed sprites) so a wide sprite is created.
      tft.fillRect(GEAR_X, GEAR_M_Y, dm.textWidth(" M  "), dm.fontHeight(), TFT_BLACK);
      tft.setCursor(GEAR_X, GEAR_Y);
      dm.printToSprite(" D  ");
      if (manual_gear) {
        tft.setCursor(GEAR_X, GEAR_M_Y);
        dm.printToSprite(" M  ");
      }
    } else {
      // P / R / N in the big gear font (leading/trailing spaces clear the old glyph)
      gear_spr.printToSprite(" " + gear + "            ");
    }
    prevGear = gear;
    prevCurrentGear = current_gear;
    prevManual = manual_gear;
  }
}

// Draw/clear the orange "MAZDASPEED" label (between the speed and gear), on change only.
void drawSport() {
  int st = sport ? 1 : 0;
  if (st == prevSportState) return;
  tft.fillRect(SPORT_BAND_X, SPORT_BAND_Y, SPORT_BAND_W, SPORT_BAND_H, TFT_BLACK);
  if (sport) {
    tft.setCursor(SPORT_TEXT_X, SPORT_TEXT_Y);
    mazdaspeed.printToSprite("MAZDASPEED");
  }
  prevSportState = st;
}

// Fade the initial speed/gear glyphs up from black by ramping their text COLOURS
// (backlight untouched). Redraws the current state each step so the numbers/letters
// materialize from the black the splash left behind.
void fadeInFromBlack() {
  const int STEPS = 16;
  for (int i = 1; i <= STEPS; i++) {
    uint8_t v = (uint8_t)(255 * i / STEPS);
    uint16_t white  = tft.color565(v, v, v);
    uint16_t orange = tft.color565(v, (uint8_t)(v * 165 / 255), 0);
    speed_spr.setTextColor(white, TFT_BLACK);
    gear_spr.setTextColor(white, TFT_BLACK);
    dm.setTextColor(white, TFT_BLACK);
    mazdaspeed.setTextColor(orange, TFT_BLACK);
    prevSpeed = -1; prevGear = ""; prevCurrentGear = -1; prevSportState = -1;
    drawSpeedGear();
    drawSport();
    delay(30);
  }
  // Land on the exact final colours for normal operation.
  speed_spr.setTextColor(TFT_WHITE, TFT_BLACK);
  gear_spr.setTextColor(TFT_WHITE, TFT_BLACK);
  dm.setTextColor(TFT_WHITE, TFT_BLACK);
  mazdaspeed.setTextColor(TFT_ORANGE, TFT_BLACK);
}

#ifdef DEMO_MODE
// One full pass of the animation. loop() calls this repeatedly so it runs forever.
void runDemo() {
  // Baseline: P, no sport
  sport = false; drawSport();
  gear = "P"; current_gear = 1; drawSpeedGear();
  delay(600);

  // Speed sweep 0 -> 200 -> 0 (km/h)
  for (int s = 0;   s <= 200; s += 2) { speed = s; drawSpeedGear(); delay(25); }
  delay(500);
  for (int s = 200; s >= 0;   s -= 2) { speed = s; drawSpeedGear(); delay(25); }
  delay(500);

  // Selector cycle: P -> R -> N
  gear = "R"; drawSpeedGear(); delay(800);
  gear = "N"; drawSpeedGear(); delay(800);

  // Drive: shift up 1..6 then back down
  gear = "D";
  for (int g = 1; g <= 6; g++) { current_gear = g; drawSpeedGear(); delay(550); }
  for (int g = 6; g >= 1; g--) { current_gear = g; drawSpeedGear(); delay(550); }

  // Back to P
  gear = "P"; drawSpeedGear(); delay(600);

  // Sport mode: MAZDASPEED label + manual "M" indicator + a quick D-gear speed blip
  sport = true; drawSport();
  manual_gear = true;  // demo only: show the "M" alongside MAZDASPEED
  gear = "D"; current_gear = 3; drawSpeedGear();
  for (int s = 0;   s <= 140; s += 3) { speed = s; drawSpeedGear(); delay(20); }
  for (int s = 140; s >= 0;   s -= 3) { speed = s; drawSpeedGear(); delay(20); }
  sport = false; drawSport();
  manual_gear = false;
  gear = "P"; drawSpeedGear();
  delay(800);

  // Backlight sweep so brightness control is visible without the CAN bus.
  for (int b = 255; b >= NIGHT_PWM_MIN; b -= 4) { analogWrite(BL_PIN, b); delay(15); }
  for (int b = NIGHT_PWM_MIN; b <= 255; b += 4) { analogWrite(BL_PIN, b); delay(15); }
}
#endif

void loop() {
  rp2040.wdt_reset();                        // feed the watchdog each iteration

  // Handle any wireless update first; blocks + owns the screen during a session.
  ota::service(otaUi);

#ifdef DEMO_MODE
  runDemo();
  return;
#endif

  updateBacklight();

  if (!digitalRead(CAN0_INT)) {
    CAN0.readMsgBuf(&rxId, &len, rxBuf);

    if (rxId == 0x202) {
      // Speed is raw * 0.01 km/h. Re-round to whole km/h only when raw clears the
      // current cell by SPEED_HYST past the .5 boundary, so a steady x.5 speed doesn't
      // flicker the digit (hysteresis). Larger jumps still update immediately.
      speedRaw = (rxBuf[2] << 8) | rxBuf[3];
      long hi = (long)speed * 100 + 50 + SPEED_HYST;
      long lo = (long)speed * 100 - 50 - SPEED_HYST;
      if ((long)speedRaw >= hi || (long)speedRaw < lo) speed = (speedRaw + 50) / 100;
    }

    if (rxId == 0x228) {
      // read the gear
      gearRaw = rxBuf[0] & 0x07;
      // read the current gear
      switch (gearRaw) {
        case 0x01:
          gear = "P";
          break;
        case 0x02:
          gear = "R";
          break;
        case 0x03:
          gear = "N";
          break;
        case 0x04:
          gear = "D";
          current_gearRaw = rxBuf[4] & 0x1E;
          // Debounce: a new gear number must persist GEAR_DEBOUNCE frames before it
          // shows, so brief 2<->3 hunting at a shift point doesn't flicker the digit.
          {
            int cg = current_gearRaw >> 1;
            if (cg == current_gear)  { gearCand = cg; gearCandN = 0; }
            else if (cg == gearCand) { if (++gearCandN >= GEAR_DEBOUNCE) current_gear = cg; }
            else                     { gearCand = cg; gearCandN = 1; }
          }
          break;
      }
    }

    if (rxId == 0x131) {
      // Manual (M) mode flag. On the bench 0x131 idles at all-zero; byte[2]
      // (SavvyCAN's "3rd pair") flips 0x00 -> 0x80 when the manual gate is
      // engaged, so bit 7 is the manual flag. byte[1] == 0x04 marks the D/M
      // gate, but we already take "D" from 0x228, so only the M bit is needed.
      manual_gear = (rxBuf[2] & 0x80) != 0;
    }

    if (rxId == BRIGHT_ID) {
      targetBrightness = computeTargetBrightness(rxBuf[BRIGHT_LEVEL_IDX],
                                                 rxBuf[DAY_MODE_IDX]);
    }

    if (rxId == SPORT_ID) {
      sport = (rxBuf[SPORT_IDX] & SPORT_MASK) != 0;
    }

    // draw speed/gear and the sport label (each only if changed)
    drawSpeedGear();
    drawSport();
  }
}