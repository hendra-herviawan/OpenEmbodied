#include "wifi_board.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "iot/thing_manager.h"
#include "audio/codecs/vb6824_audio_codec.h"
#include "power_manager.h"
#include "assets/lang_config.h"

#include "led/single_led.h"
#include "display/lcd_display.h"

#include <wifi_station.h>
#include "power_save_timer.h"
#include <esp_log.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include "driver/gpio.h"
#include "driver/spi_master.h"

#include <algorithm>

#define TAG "Mz01C3Lcd"

LV_FONT_DECLARE(font_puhui_16_4);
LV_FONT_DECLARE(font_awesome_16_4);

class Mz01Backlight : public PwmBacklight {
public:
    Mz01Backlight() : PwmBacklight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT) {}
};

class Mz01C3LcdBoard : public WifiBoard {
private:
    Button boot_button_;
    Button volume_up_button_;
    Button volume_down_button_;
    SpiLcdDisplay* display_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    VbAduioCodec* audio_codec_ = nullptr;
    PowerManager* power_manager_;
    PowerSaveTimer* power_save_timer_;
    int volume_ = 70;

    void InitializeLcdDisplay() {
        // Drive the backlight GPIO directly here: PwmBacklight construction
        // and SetBrightness call Board::GetInstance(), which deadlocks when
        // invoked from inside the board constructor (__cxa_guard).
        gpio_config_t bl_conf = {
            .pin_bit_mask = (1ULL << DISPLAY_BACKLIGHT_PIN),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&bl_conf);
        gpio_set_level(DISPLAY_BACKLIGHT_PIN, 1);

        spi_bus_config_t buscfg = {
            .mosi_io_num = DISPLAY_SPI_MOSI_PIN,
            .miso_io_num = -1,
            .sclk_io_num = DISPLAY_SPI_SCLK_PIN,
            .quadwp_io_num = -1,
            .quadhd_io_num = -1,
            .max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t),
        };
        ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));

        esp_lcd_panel_io_spi_config_t io_config = {
            .cs_gpio_num = DISPLAY_SPI_CS_PIN,
            .dc_gpio_num = DISPLAY_SPI_DC_PIN,
            .spi_mode = 3,
            .pclk_hz = DISPLAY_SPI_SCLK_HZ,
            .trans_queue_depth = 10,
            .on_color_trans_done = nullptr,
            .lcd_cmd_bits = 8,
            .lcd_param_bits = 8,
        };
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_config, &panel_io_));

        esp_lcd_panel_dev_config_t panel_config = {
            .reset_gpio_num = DISPLAY_SPI_RESET_PIN,
            .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
            .bits_per_pixel = 16,
        };
        ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io_, &panel_config, &panel_));
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_));
        ESP_ERROR_CHECK(esp_lcd_panel_init(panel_));
        esp_lcd_panel_invert_color(panel_, DISPLAY_INVERT_COLOR);

        // The panel stays in its NATIVE orientation: this ST7789 exposes a
        // 240x240 window inside 240x320 controller RAM, where MADCTL-based
        // rotation mirrors/wraps the framebuffer. Landscape is rendered by
        // LVGL software rotation instead (sw_rotation=ROTATION_90).
        display_ = new SpiLcdDisplay(panel_io_, panel_,
                                     DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                     DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
                                     DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y,
                                     DISPLAY_SWAP_XY,
                                     {
                                         .text_font = &font_puhui_16_4,
                                         .icon_font = &font_awesome_16_4,
                                         .emoji_font = font_emoji_32_init(),
                                     },
                                     LV_DISPLAY_ROTATION_90);
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting && !WifiStation::GetInstance().IsConnected()) {
                ResetWifiConfiguration();
                return;
            }
            app.ToggleChatState();
        });
        // Long press is deliberately inert: ResetWifiConfiguration() here is
        // too easy to trigger by accident and wipes the WiFi credentials.
        boot_button_.OnLongPress([this]() {
            ESP_LOGI(TAG, "BOOT long press (no-op by design)");
        });

        volume_up_button_.OnClick([this]() {
            if (audio_codec_) {
                volume_ = std::min<int>(100, audio_codec_->output_volume() + 10);
                audio_codec_->SetOutputVolume(volume_);
            }
            if (display_) {
                display_->ShowNotification(
                    std::string(Lang::Strings::VOLUME) + std::to_string(volume_), 1500);
            }
        });
        volume_down_button_.OnClick([this]() {
            if (audio_codec_) {
                volume_ = std::max<int>(0, audio_codec_->output_volume() - 10);
                audio_codec_->SetOutputVolume(volume_);
            }
            if (display_) {
                display_->ShowNotification(
                    std::string(Lang::Strings::VOLUME) + std::to_string(volume_), 1500);
            }
        });
    }

    void InitializeIot() {
        auto& thing_manager = iot::ThingManager::GetInstance();
        thing_manager.AddThing(iot::CreateThing("Speaker"));
        thing_manager.AddThing(iot::CreateThing("Screen"));
    }

    void InitializePowerSaveTimer() {
        power_save_timer_ = new PowerSaveTimer(-1, 60 * 20, 60 * 30);
        power_save_timer_->OnEnterSleepMode([this]() {
            ESP_LOGI(TAG, "Enabling sleep mode");
            Application::GetInstance().EnterSleepMode();
        });
        power_save_timer_->OnExitSleepMode([this]() {
            ESP_LOGI(TAG, "Exit sleep mode");
        });
        power_save_timer_->OnShutdownRequest([this]() {
            if (!power_manager_->IsCharging()) {
                PowerOff();
            }
        });
        power_save_timer_->SetEnabled(true);
    }

public:
    Mz01C3LcdBoard() :
        boot_button_(BOOT_BUTTON_GPIO),
        volume_up_button_(VOLUME_UP_BUTTON_GPIO),
        volume_down_button_(VOLUME_DOWN_BUTTON_GPIO) {
        // The factory firmware re-asserts the power-hold GPIO (LOW then HIGH)
        // before spi_bus_initialize. Keep that preamble: without it,
        // esp_lcd_new_panel_io_spi can fail with ESP_ERR_INVALID_STATE when
        // the GPIO matrix is left dirty by a previous reset stage.
        gpio_config_t power_hold_conf = {
            .pin_bit_mask = (1ULL << POWER_HOLD_PIN),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&power_hold_conf);
        gpio_set_level(POWER_HOLD_PIN, 0);
        gpio_set_level(POWER_HOLD_PIN, 1);

        const gpio_num_t lcd_pins[] = {
            DISPLAY_SPI_MOSI_PIN, DISPLAY_SPI_SCLK_PIN,
            DISPLAY_SPI_CS_PIN,   DISPLAY_SPI_DC_PIN,
            DISPLAY_SPI_RESET_PIN,
        };
        for (gpio_num_t p : lcd_pins) {
            gpio_reset_pin(p);
        }

        InitializeLcdDisplay();
        InitializeButtons();
        InitializeIot();

        power_manager_ = new PowerManager(POWER_CHARGE_PIN);
        InitializePowerSaveTimer();

        // VbAduioCodec over UART1. Set the volume directly via the driver
        // here: audio_codec_->SetOutputVolume calls Board::GetInstance(),
        // which deadlocks inside the board constructor (__cxa_guard).
        audio_codec_ = new VbAduioCodec(CODEC_TX_GPIO, CODEC_RX_GPIO);
        audio_codec_->Start();
        vb6824_audio_set_output_volume(100);

        audio_codec_->OnWakeUp([this](const std::string& command) {
            if ((command.find("hello baby") != std::string::npos ||
                 command.find("halo baby") != std::string::npos) &&
                Application::GetInstance().GetDeviceState() != kDeviceStateListening) {
                Application::GetInstance().WakeWordInvoke("hello baby");
            }
        });
    }

    virtual AudioCodec* GetAudioCodec() override {
        return audio_codec_;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }

    virtual Backlight* GetBacklight() override {
        static Mz01Backlight backlight;
        return &backlight;
    }

    virtual bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        static bool last_charging = false;
        charging = power_manager_->IsCharging();
        discharging = !charging;
        level = power_manager_->GetBatteryLevel();
        if (charging != last_charging) {
            last_charging = charging;
            ESP_LOGI(TAG, "Charging state: %d", charging);
        }
        return true;
    }

    virtual void PowerOff() override {
        power_manager_->PowerOff();
    }
};

DECLARE_BOARD(Mz01C3LcdBoard);
