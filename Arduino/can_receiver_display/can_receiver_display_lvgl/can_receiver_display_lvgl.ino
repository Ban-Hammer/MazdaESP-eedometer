// LVGL 9.x implementation for testing
// Requires: lvgl (9.x), TFT_eSPI, mcp_can libraries
// LVGL config: Enable LV_USE_TINY_TTF and LV_USE_TFT_ESPI in lv_conf.h
// Font: Place a .ttf font file in LittleFS (e.g., /font.ttf)

// Tell LVGL to look for lv_conf.h in the sketch folder
// #define LV_CONF_INCLUDE_SIMPLE
#include <lvgl.h>
#include <TFT_eSPI.h>
#include <mcp_can.h>
#include <SPI.h>
#include <FS.h>
#include <LittleFS.h>

#define TTF_FONT "/font.ttf"
#define SCREEN_WIDTH 240
#define SCREEN_HEIGHT 240
#define DRAW_BUF_SIZE (SCREEN_WIDTH * SCREEN_HEIGHT / 10 * (LV_COLOR_DEPTH / 8))

#define CS_PIN 19
#define CAN0_INT 22

long unsigned int rxId;
unsigned char len = 0;
unsigned char rxBuf[8];
char msgString[128];

unsigned int speedRaw = 0;
int speed = 0;
int prevSpeed = -1;

unsigned int gearRaw = 0;
String gear = "P";
String prevGear = "";

unsigned int current_gearRaw = 0;
int current_gear = 1;
int prevCurrentGear = -1;

bool manual_gear = false;
bool sport = false;

MCP_CAN CAN0(CS_PIN);

TFT_eSPI tft = TFT_eSPI();

// LVGL objects
lv_obj_t *speed_label;
lv_obj_t *gear_label;
lv_font_t *font_large = NULL;
lv_font_t *font_medium = NULL;

// Draw buffer
static uint8_t draw_buf[DRAW_BUF_SIZE];

// Display flush callback for LVGL 9.x
void my_disp_flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
  uint32_t w = lv_area_get_width(area);
  uint32_t h = lv_area_get_height(area);

  tft.startWrite();
  tft.setAddrWindow(area->x1, area->y1, w, h);
  tft.pushColors((uint16_t *)px_map, w * h, true);
  tft.endWrite();

  lv_display_flush_ready(disp);
}

// Tick callback for LVGL
uint32_t my_tick_get_cb(void) {
  return millis();
}

void setup() {
  Serial.begin(115200);

  if (!LittleFS.begin()) {
    Serial.println("LittleFS init failed!");
  } else {
    Serial.println("LittleFS available!");
  }

  if (!LittleFS.exists(TTF_FONT)) {
    Serial.println("TTF font missing! Please upload font.ttf to LittleFS");
  } else {
    Serial.println("TTF font found!");
  }

  if (CAN0.begin(MCP_STDEXT, CAN_500KBPS, MCP_16MHZ) == CAN_OK) {
    Serial.println("MCP2515 Initialized Successfully!");
  } else {
    Serial.println("Error Initializing MCP2515...");
  }
  pinMode(CAN0_INT, INPUT_PULLUP);

  CAN0.init_Mask(0, 0, 0x02FF0000);
  CAN0.init_Filt(0, 0, 0x02020000);
  CAN0.init_Filt(1, 0, 0x02280000);

  CAN0.setMode(MCP_NORMAL);

  // Initialize TFT
  tft.init();
  tft.setRotation(0);
  tft.fillScreen(TFT_BLACK);

  // Initialize LVGL
  lv_init();

  // Set tick callback
  lv_tick_set_cb(my_tick_get_cb);

  // Create display (LVGL 9.x API)
  lv_display_t *disp = lv_display_create(SCREEN_WIDTH, SCREEN_HEIGHT);
  lv_display_set_flush_cb(disp, my_disp_flush);
  lv_display_set_buffers(disp, draw_buf, NULL, sizeof(draw_buf), LV_DISPLAY_RENDER_MODE_PARTIAL);

  // Load TTF fonts using Tiny TTF (requires LV_USE_TINY_TTF enabled)
#if LV_USE_TINY_TTF
  File fontFile = LittleFS.open(TTF_FONT, "r");
  if (fontFile) {
    size_t fontLen = fontFile.size();
    uint8_t *fontData = (uint8_t *)lv_malloc(fontLen);
    if (fontData) {
      fontFile.read(fontData, fontLen);
      fontFile.close();

      // Create fonts at different sizes
      font_large = lv_tiny_ttf_create_data(fontData, fontLen, 112);
      font_medium = lv_tiny_ttf_create_data(fontData, fontLen, 96);

      if (font_large && font_medium) {
        Serial.println("LVGL TTF fonts loaded");
      } else {
        Serial.println("LVGL TTF font create failed, using default");
        font_large = (lv_font_t *)LV_FONT_DEFAULT;
        font_medium = (lv_font_t *)LV_FONT_DEFAULT;
      }
      // Note: fontData should NOT be freed - LVGL keeps using it
    } else {
      Serial.println("Could not allocate font buffer");
      fontFile.close();
      font_large = (lv_font_t *)LV_FONT_DEFAULT;
      font_medium = (lv_font_t *)LV_FONT_DEFAULT;
    }
  } else {
    Serial.println("Could not open font file");
    font_large = (lv_font_t *)LV_FONT_DEFAULT;
    font_medium = (lv_font_t *)LV_FONT_DEFAULT;
  }
#else
  Serial.println("Tiny TTF not enabled, using default font");
  font_large = (lv_font_t *)LV_FONT_DEFAULT;
  font_medium = (lv_font_t *)LV_FONT_DEFAULT;
#endif

  // Set black background
  lv_obj_set_style_bg_color(lv_screen_active(), lv_color_black(), LV_PART_MAIN);

  // Create speed label
  speed_label = lv_label_create(lv_screen_active());
  lv_obj_set_style_text_font(speed_label, font_large, LV_PART_MAIN);
  lv_obj_set_style_text_color(speed_label, lv_color_white(), LV_PART_MAIN);
  lv_label_set_text(speed_label, "0");
  lv_obj_set_pos(speed_label, 10, 10);

  // Create gear label
  gear_label = lv_label_create(lv_screen_active());
  lv_obj_set_style_text_font(gear_label, font_medium, LV_PART_MAIN);
  lv_obj_set_style_text_color(gear_label, lv_color_white(), LV_PART_MAIN);
  lv_label_set_text(gear_label, "P");
  lv_obj_set_pos(gear_label, 10, 140);

  Serial.println("LVGL UI initialized");
}

void loop() {
  // Handle LVGL tasks
  lv_timer_handler();

  if (!digitalRead(CAN0_INT)) {
    CAN0.readMsgBuf(&rxId, &len, rxBuf);

    if (rxId == 0x202) {
      speedRaw = (rxBuf[2] << 8) | rxBuf[3];
      speed = (speedRaw + 50) / 100;
    }

    if (rxId == 0x228) {
      gearRaw = rxBuf[0] & 0x07;
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
          current_gear = current_gearRaw >> 1;
          break;
      }
    }

    // update speed label (only if changed)
    if (speed != prevSpeed) {
      lv_label_set_text_fmt(speed_label, "%d", speed);
      prevSpeed = speed;
    }

    // update gear label (only if changed)
    if (gear != prevGear || (gear == "D" && current_gear != prevCurrentGear)) {
      if (gear == "D") {
        lv_label_set_text_fmt(gear_label, "%s %d", gear.c_str(), current_gear);
      } else {
        lv_label_set_text(gear_label, gear.c_str());
      }
      prevGear = gear;
      prevCurrentGear = current_gear;
    }

    Serial.print("Gear: ");
    Serial.print(gear);
    Serial.print(", ");
    Serial.print(current_gear);

    sprintf(msgString, " Speed: %d", speed);
    Serial.println(msgString);
  }
}
