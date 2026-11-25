// MX-5 ND digital speedometer - single ESP32 build.
// Ported from the RP2040 + MCP2515 + XIAO OTA-bridge original: CAN now uses the
// ESP32's built-in TWAI controller (via an SN65HVD230 transceiver), the ST7789 runs
// straight off the ESP32's VSPI, and wireless updates are hosted by the ESP32 itself.

#include <Arduino.h>
#include <TFT_eSPI.h>
#include <SPI.h>
#include <FS.h>
#include <LittleFS.h>
#include "driver/twai.h"
#include "ota_update.h"
// #include "splash.h"   // boot splash disabled; code kept in disabled_splash/

#define FW_VERSION "v1.0-esp32"

#define SPEED_FONT "speed112_2"
#define GEAR_FONT "gear96"

// ---- Pins (HiLetgo ESP32-DevKitC-32). TFT pins live in platformio.ini. -------------
#define CAN_TX_PIN  GPIO_NUM_25    // -> SN65HVD230 D (TXD)
#define CAN_RX_PIN  GPIO_NUM_26    // <- SN65HVD230 R (RXD)
#define BL_PIN      22             // -> TFT BLK (PWM dimming)
#define BL_CH       0              // LEDC channel (core 2.x only)

#ifndef CAN_LISTEN_ONLY
#define CAN_LISTEN_ONLY 1
#endif

// Day/night + dimming come from TWO CAN frames, combined so the screen is BRIGHT when
// either the manual day-mode toggle is set OR the headlamps aren't truly lit, and DIM
// only when the lamps are genuinely on for the dark and day-mode isn't forced:
//   bright  if  (0x09F D6 day-mode bit set)  OR  (0x09A headlamps NOT engaged)
//   dim     otherwise (scale the 0x09F dimmer level)
// The car reports 0x10 (not 0x30) on 0x09A in daylight even with the switch on, so
// masking 0x20/0xC0 keeps the screen bright in daylight and DRLs never dim it.
#define BRIGHT_ID           0x09F
#define BRIGHT_LEVEL_IDX    1      // SavvyCAN D2 = night dimmer level
#define DAY_MODE_IDX        5      // SavvyCAN D6 = day-mode byte
#define DAY_MODE_MASK       0x80
#define LAMP_ID             0x09A
#define LAMP_IDX            0
#define LAMP_MASK           0xE0   // 0x20 low beam + 0xC0 high beam; excludes 0x10 DRL
#define NIGHT_LEVEL_MIN     0x0D
#define NIGHT_LEVEL_MAX     0x64
#define NIGHT_PWM_MIN       1
#define NIGHT_PWM_MAX       25

// ---- Display stabilization ---------------------------------------------------------
#define SPEED_HYST          25
#define GEAR_DEBOUNCE       4

// ---- Manual-transmission gear estimate (RPM / speed) --------------------------------
// 1 = estimate gear from RPM/speed (manual car). 0 = original automatic CAN 0x228 logic.
#ifndef GEAR_FROM_RPM
#define GEAR_FROM_RPM       1
#endif
// RPM is CAN 0x202 bytes 0-1, raw * 0.25 (per community DBC; verify at idle: ~700-900).
// Fill GEAR_RATIO with rpm per km/h for gears 1..6 (see README_PORT.md, "Calibrating"),
// then set GEAR_CALIBRATED to 1. Until then the gear area stays blank.
#define GEAR_CALIBRATED     0
static const float GEAR_RATIO[6] = {0, 0, 0, 0, 0, 0};
#define GEAR_TOL            0.07f   // accept a ratio within +/-7% of a gear's ratio
#define GEAR_MIN_KPH        8       // below this the ratio is meaningless
#define GEAR_MIN_RPM        900     // below this (clutch in / coasting) hold, don't guess
#define GEAR_CONFIRM        8       // consecutive matching frames before switching
#define GEAR_HOLD_MS        1500    // keep last gear this long when no match (clutch dip)
#define GEAR_LOG_RATIO      1       // print rpm/kph/ratio on Serial twice a second

// ---- Screen orientation & label layout (panel is 240x240) -------------------------
#define SCREEN_ROTATION     2
#define SPEED_X             0
#define SPEED_Y             15
#define SPEED_NUDGE_X       (-25)
#define GEAR_X              0
#define GEAR_Y              125

#define RAMP_INTERVAL_MS 25
#define RAMP_STEP        4

uint32_t rxId;
uint8_t  len = 0;
uint8_t  rxBuf[8];

unsigned int speedRaw = 0;
unsigned int rpmRaw = 0;
int estGear = 0;               // 0 = unknown/blank
int candGear = 0;
int candCount = 0;
unsigned long lastGearMatchMs = 0;
unsigned long lastRatioLogMs = 0;
int speed = 0;
int prevSpeed = -1;

unsigned int gearRaw = 0;
String gear = "P";
String prevGear = "";

unsigned int current_gearRaw = 0;
int current_gear = 1;
int prevCurrentGear = -1;
int gearCand = -1;
int gearCandN = 0;

uint8_t currentBrightness = 255;
uint8_t targetBrightness  = 255;
bool    lampsOn     = false;
bool    dayMode     = false;
uint8_t dimmerLevel = NIGHT_LEVEL_MAX;
unsigned long lastRampMillis = 0;
unsigned long lastCanCheckMillis = 0;

TFT_eSPI tft = TFT_eSPI();
TFT_eSprite speed_spr = TFT_eSprite(&tft);
TFT_eSprite gear_spr = TFT_eSprite(&tft);

void drawSpeedGear();

// Nearest calibrated gear within tolerance, or 0.
static int matchGear(float ratio) {
  int best = 0;
  float bestErr = GEAR_TOL;
  for (int i = 0; i < 6; ++i) {
    if (GEAR_RATIO[i] <= 0) continue;
    float err = fabsf(ratio - GEAR_RATIO[i]) / GEAR_RATIO[i];
    if (err < bestErr) { bestErr = err; best = i + 1; }
  }
  return best;
}

static void updateGearEstimate() {
  float rpm = rpmRaw * 0.25f;
  float kph = speedRaw * 0.01f;
  unsigned long now = millis();
  float ratio = (kph > 0.1f) ? rpm / kph : 0;

#if GEAR_LOG_RATIO
  if (now - lastRatioLogMs >= 500) {
    lastRatioLogMs = now;
    Serial.printf("rpm=%.0f kph=%.1f ratio=%.1f gear=%d\n", rpm, kph, ratio, estGear);
  }
#endif

#if GEAR_CALIBRATED
  if (kph < GEAR_MIN_KPH) { estGear = 0; candGear = 0; candCount = 0; return; }
  int m = (rpm >= GEAR_MIN_RPM) ? matchGear(ratio) : 0;
  if (m == 0) {
    // Clutch in / between gears: hold the last gear briefly, then blank.
    if (estGear && now - lastGearMatchMs > GEAR_HOLD_MS) estGear = 0;
    candGear = 0; candCount = 0;
    return;
  }
  lastGearMatchMs = now;
  if (m == estGear) { candGear = m; candCount = 0; return; }
  if (m == candGear) { if (++candCount >= GEAR_CONFIRM) { estGear = m; candCount = 0; } }
  else { candGear = m; candCount = 1; }
#endif
}

// ---- Backlight PWM (LEDC; API differs between arduino-esp32 2.x and 3.x) ----------
static void backlightBegin() {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(BL_PIN, 1000, 8);
#else
  ledcSetup(BL_CH, 1000, 8);
  ledcAttachPin(BL_PIN, BL_CH);
#endif
}

static void backlightWrite(uint8_t duty) {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(BL_PIN, duty);
#else
  ledcWrite(BL_CH, duty);
#endif
}

// ---- CAN (TWAI) ---------------------------------------------------------------------
static bool canBegin() {
  twai_general_config_t g = TWAI_GENERAL_CONFIG_DEFAULT(
      CAN_TX_PIN, CAN_RX_PIN, CAN_LISTEN_ONLY ? TWAI_MODE_LISTEN_ONLY : TWAI_MODE_NORMAL);
  g.rx_queue_len = 64;
  twai_timing_config_t t = TWAI_TIMING_CONFIG_500KBITS();
  twai_filter_config_t f = TWAI_FILTER_CONFIG_ACCEPT_ALL();   // filtering is done in loop()
  if (twai_driver_install(&g, &t, &f) != ESP_OK) return false;
  return twai_start() == ESP_OK;
}

// Pop one frame (non-blocking) into rxId / len / rxBuf. Extended and remote frames are
// reported with rxId = 0xFFFFFFFF so no decoder matches them.
static bool canRead() {
  twai_message_t m;
  if (twai_receive(&m, 0) != ESP_OK) return false;
  memset(rxBuf, 0, sizeof(rxBuf));
  if (m.extd || m.rtr) { rxId = 0xFFFFFFFF; len = 0; return true; }
  rxId = m.identifier;
  len = m.data_length_code > 8 ? 8 : m.data_length_code;
  memcpy(rxBuf, m.data, len);
  return true;
}

// Restart the controller if it ever drops off the bus. Checked about once a second.
static void canMaintain() {
  unsigned long now = millis();
  if (now - lastCanCheckMillis < 1000) return;
  lastCanCheckMillis = now;
  twai_status_info_t s;
  if (twai_get_status_info(&s) != ESP_OK) return;
  if (s.state == TWAI_STATE_BUS_OFF) twai_initiate_recovery();
  else if (s.state == TWAI_STATE_STOPPED) twai_start();
}

// ---- OTA status UI --------------------------------------------------------------------
void otaUi(ota::Ui phase, uint8_t pct, const char* msg) {
  const int TOP_Y = 40;
  const int BAR_Y = 66;
  switch (phase) {
    case ota::Ui::STARTED:
      tft.fillScreen(TFT_BLACK);
      tft.setTextColor(TFT_WHITE, TFT_BLACK);
      tft.setTextDatum(MC_DATUM);
      tft.drawString("Updating...", tft.width() / 2, TOP_Y, 4);
      tft.setTextDatum(TL_DATUM);
      break;
    case ota::Ui::PROGRESS: {
      int w = tft.width() - 40, x = 20;
      tft.drawRect(x, BAR_Y, w, 16, TFT_WHITE);
      tft.fillRect(x + 1, BAR_Y + 1, (w - 2) * pct / 100, 14, TFT_GREEN);
      break;
    }
    case ota::Ui::SUCCESS:
      tft.fillScreen(TFT_BLACK);
      tft.setTextColor(TFT_WHITE, TFT_BLACK);
      tft.setTextDatum(MC_DATUM);
      tft.drawString("Done", tft.width() / 2, TOP_Y, 4);
      tft.setTextDatum(TL_DATUM);
      // Firmware updates reboot; file uploads fall through, so repaint everything.
      prevSpeed = -1; prevGear = ""; prevCurrentGear = -1;
      delay(600);
      tft.fillScreen(TFT_BLACK);
      break;
    case ota::Ui::FAILED:
      tft.fillScreen(TFT_BLACK);
      tft.setTextColor(TFT_RED, TFT_BLACK);
      tft.setTextDatum(MC_DATUM);
      tft.drawString(msg ? msg : "Update failed", tft.width() / 2, TOP_Y, 4);
      tft.setTextDatum(TL_DATUM);
      delay(1500);
      tft.fillScreen(TFT_BLACK);
      prevSpeed = -1; prevGear = ""; prevCurrentGear = -1;
      break;
  }
}

// (splashInterFrame() removed along with the boot splash; see disabled_splash/.)

uint8_t computeTargetBrightness() {
  if (dayMode || !lampsOn) return 255;
  uint8_t level = dimmerLevel;
  if (level < NIGHT_LEVEL_MIN) level = NIGHT_LEVEL_MIN;
  if (level > NIGHT_LEVEL_MAX) level = NIGHT_LEVEL_MAX;
  return (uint8_t)map(level, NIGHT_LEVEL_MIN, NIGHT_LEVEL_MAX, NIGHT_PWM_MIN, NIGHT_PWM_MAX);
}

void updateBacklight() {
  unsigned long now = millis();
  if (now - lastRampMillis < RAMP_INTERVAL_MS) return;
  lastRampMillis = now;

  int cur = currentBrightness;
  int tgt = targetBrightness;
  if (cur < tgt)      cur += min(RAMP_STEP, tgt - cur);
  else if (cur > tgt) cur -= min(RAMP_STEP, cur - tgt);

  currentBrightness = (uint8_t)cur;
  backlightWrite(currentBrightness);
}

void setup() {
  Serial.begin(115200);

  // Registers the web routes only; the Wi-Fi AP itself starts from loop().
  ota::begin(FW_VERSION);

  if (!LittleFS.begin(false)) {
    Serial.println("LittleFS init failed! Run: pio run -t uploadfs");
  } else {
    Serial.println("LittleFS available!");
  }

  const char* fontFiles[] = {
    "/" SPEED_FONT ".vlw", "/" GEAR_FONT ".vlw",
  };
  bool allFonts = true;
  for (const char* f : fontFiles) {
    if (!LittleFS.exists(f)) {
      Serial.print("Font missing: "); Serial.println(f);
      allFonts = false;
    }
  }
  if (allFonts) Serial.println("All fonts are there!");

  if (canBegin()) Serial.println("TWAI started (500 kbps)");
  else            Serial.println("Error starting TWAI...");

  tft.init();
  tft.setRotation(SCREEN_ROTATION);
  tft.fillScreen(TFT_BLACK);

  backlightBegin();

  speed_spr.setColorDepth(16);
  gear_spr.setColorDepth(16);

  speed_spr.loadFont(SPEED_FONT, LittleFS);
  gear_spr.loadFont(GEAR_FONT, LittleFS);

  speed_spr.setTextColor(TFT_WHITE, TFT_BLACK);
  gear_spr.setTextColor(TFT_WHITE, TFT_BLACK);

  backlightWrite(255);

  // No boot splash: loop() draws the first frame immediately.
}

void drawSpeedGear() {
  if (speed != prevSpeed) {
    bool nudgeLeft = (speed >= 100) || (speed >= 10 && speed < 20);
    tft.setCursor(nudgeLeft ? SPEED_NUDGE_X : SPEED_X, SPEED_Y);
    speed_spr.printToSprite(" " + (String)(speed) + "          ");
    prevSpeed = speed;
  }

  if (gear != prevGear || (gear == "D" && current_gear != prevCurrentGear)) {
    tft.setCursor(GEAR_X, GEAR_Y);
    // gear96 has no "D", so in drive we show just the gear NUMBER. Strings are
    // space-padded: single-char printToSprite renders nothing, and the padding is
    // what erases the previous glyph.
    if (gear == "D") {
      gear_spr.printToSprite(" " + (String)(current_gear) + "            ");
    } else {
      gear_spr.printToSprite(" " + gear + "            ");
    }
    prevGear = gear;
    prevCurrentGear = current_gear;
  }
}

// Decode the frame currently in rxId / rxBuf. Logic unchanged from the original.
static void handleFrame() {
  if (rxId == 0x202) {
    // Speed is raw * 0.01 km/h; hysteresis so a steady x.5 speed doesn't flicker.
    speedRaw = (rxBuf[2] << 8) | rxBuf[3];
    rpmRaw = (rxBuf[0] << 8) | rxBuf[1];
    updateGearEstimate();
    long hi = (long)speed * 100 + 50 + SPEED_HYST;
    long lo = (long)speed * 100 - 50 - SPEED_HYST;
    if ((long)speedRaw >= hi || (long)speedRaw < lo) speed = (speedRaw + 50) / 100;
  }

#if !GEAR_FROM_RPM
  if (rxId == 0x228) {
    gearRaw = rxBuf[0] & 0x07;
    switch (gearRaw) {
      case 0x01: gear = "P"; break;
      case 0x02: gear = "R"; break;
      case 0x03: gear = "N"; break;
      case 0x04:
        gear = "D";
        current_gearRaw = rxBuf[4] & 0x1E;
        {
          int cg = current_gearRaw >> 1;
          if (cg == current_gear)  { gearCand = cg; gearCandN = 0; }
          else if (cg == gearCand) { if (++gearCandN >= GEAR_DEBOUNCE) current_gear = cg; }
          else                     { gearCand = cg; gearCandN = 1; }
        }
        break;
    }
  }
#endif

  if (rxId == BRIGHT_ID) {
    dimmerLevel = rxBuf[BRIGHT_LEVEL_IDX];
    dayMode = (rxBuf[DAY_MODE_IDX] & DAY_MODE_MASK) != 0;
    targetBrightness = computeTargetBrightness();
  }

  if (rxId == LAMP_ID) {
    lampsOn = (rxBuf[LAMP_IDX] & LAMP_MASK) != 0;
    targetBrightness = computeTargetBrightness();
  }
}

void loop() {
  // Wi-Fi update AP: only while stationary, and only in the boot window (see ota_update.h).
  ota::service(otaUi, speed == 0);

  updateBacklight();
  canMaintain();

  // Drain pending frames (capped so a busy bus can't starve the display), then redraw.
  for (int i = 0; i < 64 && canRead(); ++i) handleFrame();

#if GEAR_FROM_RPM
  gear = estGear ? "D" : "";           // "D" = show the gear number only
  if (estGear) current_gear = estGear;
#endif
  drawSpeedGear();

  delay(1);   // yield to the Wi-Fi / idle tasks
}
