// OpenFontRender implementation for testing
// Requires: TFT_eSPI, OpenFontRender, mcp_can libraries
// Font: Place a .ttf font file in LittleFS (e.g., /font.ttf)

#include <TFT_eSPI.h>
#include <mcp_can.h>
#include <SPI.h>
#include <FS.h>
#include <LittleFS.h>
#include "OpenFontRender.h"
#include "LittleFS_Preset.h"  // LittleFS filesystem support for OFR

#define TTF_FONT "/font.ttf"

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
TFT_eSprite spr = TFT_eSprite(&tft);
OpenFontRender ofr;

// Sprite dimensions for flicker-free rendering
#define SPR_WIDTH 240
#define SPR_HEIGHT 130

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

  tft.init();
  tft.setRotation(0);
  tft.fillScreen(TFT_BLACK);

  // Create sprite for flicker-free rendering
  spr.setColorDepth(16);
  spr.createSprite(SPR_WIDTH, SPR_HEIGHT);
  spr.fillSprite(TFT_BLACK);

  // Initialize OpenFontRender with TFT_eSPI sprite
  ofr.setDrawer(spr);

  // Load TTF font from LittleFS
  // Note: LittleFS_Preset.h provides the filesystem functions
  if (ofr.loadFont(TTF_FONT) == FT_Err_Ok) {
    Serial.println("Font loaded successfully");
  } else {
    Serial.println("Font load failed!");
  }

  ofr.setFontColor(TFT_WHITE, TFT_BLACK);
}

void drawSpeed(int spd) {
  spr.fillSprite(TFT_BLACK);

  ofr.setFontSize(112);
  ofr.setCursor(10, 10);
  ofr.printf("%d", spd);

  spr.pushSprite(0, 0);
}

void drawGear(String g, int currentGear) {
  spr.fillSprite(TFT_BLACK);

  ofr.setFontSize(96);
  ofr.setCursor(10, 10);

  if (g == "D") {
    ofr.printf("%s %d", g.c_str(), currentGear);
  } else {
    ofr.printf("%s", g.c_str());
  }

  spr.pushSprite(0, 130);
}

void loop() {
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

    // draw speed (only if changed)
    if (speed != prevSpeed) {
      drawSpeed(speed);
      prevSpeed = speed;
    }

    // draw gear (only if changed)
    if (gear != prevGear || (gear == "D" && current_gear != prevCurrentGear)) {
      drawGear(gear, current_gear);
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
