#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

// Zuowei MZ01-C3-LCD (EZTECH X1, board id: zuowei-c3-realtime-lcd-id)
// Pins reverse-engineered from the vendor 1.9.3 firmware backup
// (see Personal_2026/esp/firmware-backup/analysis/EXTRACTION_REPORT.md)
// and cross-validated against iuridomingos/xiaotu-esp32-c3 (same hardware class).

#include <driver/gpio.h>

// Audio: VB6824 voice module over UART (mic + speaker + wake word)
#define AUDIO_INPUT_SAMPLE_RATE  16000
#define AUDIO_OUTPUT_SAMPLE_RATE 16000
#define CODEC_TX_GPIO   GPIO_NUM_20   // ESP32-C3 TX -> VB6824 RX (Index 3 in factory vtable @ 0x4201b8f2)
#define CODEC_RX_GPIO   GPIO_NUM_10   // VB6824 TX -> ESP32-C3 RX (Index 4 in factory vtable @ 0x4201b8f2)

// Buttons
#define BOOT_BUTTON_GPIO        GPIO_NUM_9
#define VOLUME_UP_BUTTON_GPIO   GPIO_NUM_8
#define VOLUME_DOWN_BUTTON_GPIO GPIO_NUM_7

// Display: ST7789 1.54" 240x240 SPI
// Orientation strategy ports the PROVEN dino-run mapping
// (M5STACK/pixelart-game-enggine/engine/platform/mz01/mz01_platform.cpp:170-201):
// the panel runs in NATIVE orientation (no MADCTL swap/mirror, no gap offset)
// and the landscape-with-buttons-on-RIGHT rotation is done in software at
// blit time: panel_x = content_y, panel_y = 239 - content_x.
// (MADCTL-based rotation was calibrated and abandoned: on this 240x240-in-
// 240x320-RAM panel it produced mirrored/wrapped output that no mirror-bit
// combination fully fixed.)
#define DISPLAY_WIDTH   240
#define DISPLAY_HEIGHT  240
#define DISPLAY_MIRROR_X  false
#define DISPLAY_MIRROR_Y  false
#define DISPLAY_SWAP_XY   false
#define DISPLAY_INVERT_COLOR true

#define DISPLAY_SPI_MOSI_PIN  GPIO_NUM_1
#define DISPLAY_SPI_SCLK_PIN  GPIO_NUM_3
#define DISPLAY_SPI_CS_PIN    GPIO_NUM_12
#define DISPLAY_SPI_DC_PIN    GPIO_NUM_0
#define DISPLAY_SPI_RESET_PIN GPIO_NUM_2
#define DISPLAY_SPI_SCLK_HZ   (40 * 1000 * 1000)

#define DISPLAY_OFFSET_X  0
#define DISPLAY_OFFSET_Y  0

// Backlight (PWM)
#define DISPLAY_BACKLIGHT_PIN GPIO_NUM_5
#define DISPLAY_BACKLIGHT_OUTPUT_INVERT false
#define DISPLAY_BACKLIGHT_BRIGHTNESS 75

// Power
#define POWER_CHARGE_PIN GPIO_NUM_21   // charging detect (Index 5 in factory vtable @ 0x4201b8f2)
#define POWER_HOLD_PIN   GPIO_NUM_13   // power latch (hold high while on)
#define BAT_ADC_CHANNEL  ADC_CHANNEL_4 // GPIO4
#define BAT_ADC_ATTEN    ADC_ATTEN_DB_11
#define BAT_ADC_UNIT     ADC_UNIT_1
#define BAT_FULL_ADC_MV  680  // vendor default float idx0
#define BAT_EMPTY_ADC_MV 557  // vendor default float idx1

#endif // _BOARD_CONFIG_H_
