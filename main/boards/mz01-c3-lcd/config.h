#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

// Zuowei MZ01-C3-LCD (EZTECH X1, board id: zuowei-c3-realtime-lcd-id)
// ESP32-C3, ST7789 1.54" 240x240 SPI, VB6824 voice coprocessor over UART.

#include <driver/gpio.h>
#include <esp_adc/adc_oneshot.h>

// Audio: VB6824 voice module over UART (mic + speaker + wake word)
#define AUDIO_INPUT_SAMPLE_RATE  16000
#define AUDIO_OUTPUT_SAMPLE_RATE 16000
#define CODEC_TX_GPIO   GPIO_NUM_20   // ESP32-C3 TX -> VB6824 RX
#define CODEC_RX_GPIO   GPIO_NUM_10   // VB6824 TX -> ESP32-C3 RX

// Buttons
#define BOOT_BUTTON_GPIO        GPIO_NUM_9
#define VOLUME_UP_BUTTON_GPIO   GPIO_NUM_8
#define VOLUME_DOWN_BUTTON_GPIO GPIO_NUM_7

// Display: ST7789 1.54" 240x240 SPI.
// The panel controller has 240x320 RAM with a 240x240 visible window, so
// MADCTL-based rotation renders mirrored/wrapped output. The panel runs in
// its NATIVE orientation and the landscape rotation is done by LVGL software
// rotation (SpiLcdDisplay sw_rotate=true).
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

// Power
#define POWER_CHARGE_PIN GPIO_NUM_21   // charging detect
#define POWER_HOLD_PIN   GPIO_NUM_13   // power latch (hold high while on)
#define BAT_ADC_CHANNEL  ADC_CHANNEL_4 // GPIO4
#define BAT_ADC_ATTEN    ADC_ATTEN_DB_11
#define BAT_ADC_UNIT     ADC_UNIT_1
#define BAT_FULL_ADC_MV  680
#define BAT_EMPTY_ADC_MV 557

#endif // _BOARD_CONFIG_H_
