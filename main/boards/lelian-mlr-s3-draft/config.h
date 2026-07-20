#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>

#define AUDIO_INPUT_SAMPLE_RATE  16000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000

// Draft status from the board photo. Set these to 1 only after the real GPIO
// numbers are confirmed from the schematic, vendor pin table, or live tests.
#define LELIAN_LCD_PIN_MAP_READY 0
#define LELIAN_ML307_PIN_MAP_READY 0
#define LELIAN_AUDIO_PIN_MAP_READY 1

// The photo exposes GPIO1/2/3/4/5/6. These audio defaults are a wiring plan for
// external INMP441/ICS43434 + MAX98357A modules, not verified onboard routing.
#define AUDIO_I2S_METHOD_SIMPLEX
#define AUDIO_I2S_MIC_GPIO_WS   GPIO_NUM_1
#define AUDIO_I2S_MIC_GPIO_SCK  GPIO_NUM_2
#define AUDIO_I2S_MIC_GPIO_DIN  GPIO_NUM_3
#define AUDIO_I2S_SPK_GPIO_DOUT GPIO_NUM_4
#define AUDIO_I2S_SPK_GPIO_BCLK GPIO_NUM_5
#define AUDIO_I2S_SPK_GPIO_LRCK GPIO_NUM_6

#define BUILTIN_LED_GPIO        GPIO_NUM_NC
#define BOOT_BUTTON_GPIO        GPIO_NUM_0
#define TOUCH_BUTTON_GPIO       GPIO_NUM_NC
#define VOLUME_UP_BUTTON_GPIO   GPIO_NUM_NC
#define VOLUME_DOWN_BUTTON_GPIO GPIO_NUM_NC

// Photo silk mapping:
//   LCD_DIO  -> DISPLAY_MOSI_PIN
//   LCD_CLK  -> DISPLAY_CLK_PIN
//   LCD_SDC  -> DISPLAY_DC_PIN
//   LCD_CS   -> DISPLAY_CS_PIN
//   LCD_RST  -> DISPLAY_RST_PIN
//   LCD_FMARK is a TE/frame-mark signal and is unused by the SPI LCD driver.
//
// These GPIO values are unknown from the photo, so the draft board keeps LCD
// disabled until the exact mapping is filled in.
#define DISPLAY_BACKLIGHT_PIN GPIO_NUM_NC
#define DISPLAY_MOSI_PIN      GPIO_NUM_NC
#define DISPLAY_CLK_PIN       GPIO_NUM_NC
#define DISPLAY_DC_PIN        GPIO_NUM_NC
#define DISPLAY_RST_PIN       GPIO_NUM_NC
#define DISPLAY_CS_PIN        GPIO_NUM_NC
#define DISPLAY_FMARK_PIN     GPIO_NUM_NC

#define LCD_TYPE_ST7789_SERIAL
#define DISPLAY_WIDTH   240
#define DISPLAY_HEIGHT  240
#define DISPLAY_MIRROR_X false
#define DISPLAY_MIRROR_Y false
#define DISPLAY_SWAP_XY false
#define DISPLAY_INVERT_COLOR    true
#define DISPLAY_RGB_ORDER  LCD_RGB_ELEMENT_ORDER_RGB
#define DISPLAY_OFFSET_X  0
#define DISPLAY_OFFSET_Y  0
#define DISPLAY_BACKLIGHT_OUTPUT_INVERT false
#define DISPLAY_SPI_MODE 0

// Photo silk mapping for later 4G bring-up:
//   ESP TX -> module RX, ESP RX -> module TX.
// The exact GPIO numbers behind TX1/RX1 or TX_DBG/RX_DBG are not shown.
#define ML307_TX_PIN GPIO_NUM_NC
#define ML307_RX_PIN GPIO_NUM_NC
#define ML307_DTR_PIN GPIO_NUM_NC
#define ML307_BAUD_RATE 460800

#define LAMP_GPIO GPIO_NUM_NC

#endif // _BOARD_CONFIG_H_
