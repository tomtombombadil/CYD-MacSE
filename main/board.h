// Freenove 5.0" ESP32-S3 IPS display (FNK0115Q; also sold as Hosyond 5"):
// 800x480 IPS on a 16-bit RGB bus, GT911 capacitive touch, NS4168 I2S amp,
// SD card on SPI. Pins and panel timing as in cyd-classic-arcade's
// main/boards/freenove_s3_rgb50.hpp (from Freenove's schematic).
#pragma once

#define BOARD_W 800
#define BOARD_H 480

// RGB bus
#define PIN_DE      40
#define PIN_VSYNC   41
#define PIN_HSYNC   39
#define PIN_PCLK    42
#define RGB_PCLK_HZ 13000000   // Freenove's IPS ("Q") timing
#define RGB_BOUNCE_LINES 4     // 2 x 4 lines in internal RAM, refilled from the canvas
// B0-B4, G0-G5, R0-R4 (data line i on signal i ^ 8: bytes swapped, as
// LovyanGFX's RGB565 order; black and white look the same either way)
#define RGB_DATA_PINS {8, 3, 46, 9, 1, 5, 6, 7, 15, 16, 4, 45, 48, 47, 21, 14}
#define PIN_BACKLIGHT 2

// GT911 touch (I2C). INT is the amp's LRCLK too: only used at reset.
#define PIN_I2C_SDA 19
#define PIN_I2C_SCL 20
#define PIN_TOUCH_INT 18
#define PIN_TOUCH_RST 38

// NS4168 amp (I2S, no MCLK)
#define PIN_I2S_BCLK 0
#define PIN_I2S_WS   18
#define PIN_I2S_DOUT 17

// SD card (SPI)
#define PIN_SD_CS   10
#define PIN_SD_MOSI 11
#define PIN_SD_SCK  12
#define PIN_SD_MISO 13
