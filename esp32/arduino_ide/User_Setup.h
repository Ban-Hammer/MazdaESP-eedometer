// TFT_eSPI User_Setup.h for Arduino IDE users (PlatformIO users: ignore this file,
// the same settings live in platformio.ini build_flags).
// Copy over Arduino/libraries/TFT_eSPI/User_Setup.h. Updating TFT_eSPI resets it.

#define USER_SETUP_INFO "MX5_ESP32_ST7789"

#define ST7789_DRIVER
#define TFT_WIDTH  240
#define TFT_HEIGHT 240
#define CGRAM_OFFSET

// If colours look inverted, try one of these:
//#define TFT_INVERSION_ON
//#define TFT_INVERSION_OFF

#define TFT_MISO -1
#define TFT_MOSI 23
#define TFT_SCLK 18
#define TFT_CS    5
#define TFT_DC   19
#define TFT_RST  21
// TFT_BL intentionally NOT defined: the firmware drives the backlight (GPIO 22).

#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4
#define LOAD_FONT6
#define LOAD_FONT7
#define LOAD_FONT8
#define LOAD_GFXFF
#define SMOOTH_FONT

#define SPI_FREQUENCY       40000000
#define SPI_READ_FREQUENCY  20000000
