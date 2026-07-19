//                            USER DEFINED SETTINGS
//
// Custom setup for: Adafruit Feather RP2040 + Generic ST7789 240x240 IPS
// Interface: SPI0 via RP2040 PIO. Pins: MOSI=GPIO3, SCLK=GPIO2, CS=GPIO5,
//            DC=GPIO6, RST=GPIO4.
// (This file was reset to library defaults after re-downloading TFT_eSPI and
//  has been reconfigured for this project's hardware.)

#define USER_SETUP_INFO "Mazda_RP2040_ST7789"

// ##################################################################################
// Section 1. Driver
// ##################################################################################

#define ST7789_DRIVER          // Full configuration option for ST7789

#define TFT_WIDTH  240
#define TFT_HEIGHT 240

#define CGRAM_OFFSET           // ST7789 240x240 needs the library to apply offsets

// If colours are inverted (e.g. RED shows as CYAN, white shows as black),
// uncomment ONE of the following. Most generic 240x240 IPS panels need this ON.
//#define TFT_INVERSION_ON
//#define TFT_INVERSION_OFF

// If blue and red are swapped, try one of these:
//#define TFT_RGB_ORDER TFT_RGB  // Colour order Red-Green-Blue
//#define TFT_RGB_ORDER TFT_BGR  // Colour order Blue-Green-Red

// ##################################################################################
// Section 2. Pins (RP2040)
// ##################################################################################

#define TFT_MOSI  3
#define TFT_SCLK  2
#define TFT_CS    5   // Chip select control pin
#define TFT_DC    6   // Data Command control pin
#define TFT_RST   4   // Reset pin
// No MISO / no backlight-control pin defined (backlight wired to always-on).

// ##################################################################################
// Section 3. Fonts
// ##################################################################################

#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4
#define LOAD_FONT6
#define LOAD_FONT7
#define LOAD_FONT8
#define LOAD_GFXFF
#define SMOOTH_FONT            // Required for the .vlw fonts loaded from LittleFS

// ##################################################################################
// Section 4. RP2040 PIO SPI + clock
// ##################################################################################

#define RP2040_PIO_SPI         // Use the RP2040 PIO interface for SPI
#define TFT_SPI_PORT 0         // SPI0 pins are used

#define SPI_FREQUENCY  62500000  // ~60MHz (clean divisor of the 125MHz system clock)
#define SPI_READ_FREQUENCY  20000000
