#include "wifi_board.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "iot/thing_manager.h"
#include "audio/codecs/vb6824_audio_codec.h"
#include "power_manager.h"
#include "assets/lang_config.h"
#include "font_awesome_symbols.h"

#include "led/single_led.h"
#include "display/lcd_display.h"
#include "audio/codecs/dummy_audio_codec.h"

#include <wifi_station.h>
#include "power_save_timer.h"
#include <esp_log.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_timer.h"
#include <esp_http_client.h>
#include <cJSON.h>
#include "settings.h"
#include "mcp_server.h"
#include <web_socket.h>

#include <algorithm>
#include <cmath>

#define TAG "Mz01C3Lcd"

LV_FONT_DECLARE(font_puhui_16_4);
LV_FONT_DECLARE(font_awesome_16_4);

class Mz01Backlight : public PwmBacklight {
public:
    Mz01Backlight() : PwmBacklight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT) {
        SetBrightnessImpl(85);
        ESP_LOGI("Mz01Backlight", "LEDC Backlight initialized to 85%% duty on GPIO %d", DISPLAY_BACKLIGHT_PIN);
    }
};

static void PlayTone(int freq_hz, int duration_ms, int amplitude = 10000) {
    int total_samples = (16000 * duration_ms) / 1000;
    std::vector<int16_t> chunk(320);
    int sample_idx = 0;
    while (sample_idx < total_samples) {
        int n = std::min(320, total_samples - sample_idx);
        for (int i = 0; i < n; i++) {
            chunk[i] = (int16_t)(amplitude * sin(2.0 * M_PI * freq_hz * (sample_idx + i) / 16000.0));
        }
        vb6824_audio_write((uint8_t*)chunk.data(), n * sizeof(int16_t));
        sample_idx += n;
    }
}

class Mz01C3LcdBoard : public WifiBoard {
private:
    Button boot_button_;
    Button volume_up_button_;
    Button volume_down_button_;
    // Official OpenEmbodied LVGL UI (SpiLcdDisplay); panel handles kept for
    // power-management/diagnostics.
    SpiLcdDisplay* display_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    VbAduioCodec* audio_codec_ = nullptr;
    PowerManager* power_manager_;
    PowerSaveTimer* power_save_timer_;
    int volume_ = 70;

    void InitializeSpi() {
        spi_bus_config_t buscfg = {
            .mosi_io_num = DISPLAY_SPI_MOSI_PIN,
            .miso_io_num = -1,
            .sclk_io_num = DISPLAY_SPI_SCLK_PIN,
            .quadwp_io_num = -1,
            .quadhd_io_num = -1,
            .max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t),
        };
        ESP_LOGI(TAG, "LCD: calling spi_bus_initialize");
        ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));
        ESP_LOGI(TAG, "LCD: spi_bus_initialize OK");
    }

    // Vendor firmware asserts on these calls in mz01-c3-lcd.h lines 417/430/436:
    // spi_bus_initialize(SPI2_HOST) -> esp_lcd_new_panel_io_spi(SPI2_HOST) -> esp_lcd_new_panel_st7789
    void InitializeLcdDisplay() {
        ESP_LOGI(TAG, "LCD: InitializeLcdDisplay ENTER");

        // Turn on backlight GPIO directly (avoid PwmBacklight calling Board::GetInstance() during ctor!)
        gpio_config_t bl_conf = {
            .pin_bit_mask = (1ULL << DISPLAY_BACKLIGHT_PIN),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&bl_conf);
        gpio_set_level(DISPLAY_BACKLIGHT_PIN, 1);
        ESP_LOGI(TAG, "LCD: backlight GPIO set to HIGH");

        InitializeSpi();

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
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        ESP_LOGI(TAG, "LCD: calling esp_lcd_new_panel_io_spi (pclk=%d)", DISPLAY_SPI_SCLK_HZ);
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_config, &panel_io));
        ESP_LOGI(TAG, "LCD: esp_lcd_new_panel_io_spi OK");

        esp_lcd_panel_dev_config_t panel_config = {
            .reset_gpio_num = DISPLAY_SPI_RESET_PIN,
            .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
            .bits_per_pixel = 16,
        };
        esp_lcd_panel_handle_t panel = nullptr;
        ESP_LOGI(TAG, "LCD: calling esp_lcd_new_panel_st7789");
        ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io, &panel_config, &panel));
        ESP_LOGI(TAG, "LCD: esp_lcd_new_panel_st7789 OK");
        ESP_LOGI(TAG, "LCD: calling esp_lcd_panel_reset");
        esp_lcd_panel_reset(panel);
        ESP_LOGI(TAG, "LCD: esp_lcd_panel_reset OK");
        ESP_LOGI(TAG, "LCD: calling esp_lcd_panel_init");
        esp_lcd_panel_init(panel);
        ESP_LOGI(TAG, "LCD: esp_lcd_panel_init OK");
        esp_lcd_panel_invert_color(panel, DISPLAY_INVERT_COLOR);
        // Panel stays in NATIVE orientation: landscape rotation is handled by
        // LVGL software rotation inside SpiLcdDisplay (see lcd_display.cc
        // MZ01_SW_ROTATE patch). esp_lcd swap/mirror calls are broken on this
        // 240x240-in-240x320 panel and must not be used.

        // Official OpenEmbodied LVGL UI (status bar, chat bubbles, emotions).
        display_ = new SpiLcdDisplay(panel_io, panel,
                                     DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                     DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
                                     DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y,
                                     DISPLAY_SWAP_XY,
                                     {
                                         .text_font = &font_puhui_16_4,
                                         .icon_font = &font_awesome_16_4,
                                         .emoji_font = font_emoji_32_init(),
                                     });
        ESP_LOGI(TAG, "Step 2: Official SpiLcdDisplay (LVGL) created");

        // Keep panel handle alive — store it in a member so we don't leak and
        // so the SPI device doesn't go away.
        panel_ = panel;
        panel_io_ = panel_io;
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            ESP_LOGI(TAG, "BUTTON: BOOT clicked");
            PlayTone(880, 50);
            auto& app = Application::GetInstance();
            // Standard xiaozhi flow: BOOT toggles the Application chat state,
            // which opens the audio channel via protocol_ (Mz01XiaozhiProtocol)
            // and starts mic -> opus -> SendAudio streaming.
            if (app.GetDeviceState() == kDeviceStateStarting && !WifiStation::GetInstance().IsConnected()) {
                ResetWifiConfiguration();
                return;
            }
            app.ToggleChatState();
        });
        boot_button_.OnLongPress([this]() {
            // Deliberately inert: ResetWifiConfiguration() here is too easy to
            // trigger by accident during voice tests and wipes WiFi creds.
            // WiFi reprovisioning stays reachable via factory reset flows.
            ESP_LOGI(TAG, "BUTTON: BOOT long press (no-op by design)");
        });

        volume_up_button_.OnClick([this]() {
            if (audio_codec_) {
                volume_ = audio_codec_->output_volume();
                volume_ = std::min<int>(100, volume_ + 10);
                audio_codec_->SetOutputVolume(volume_);
            }
            if (display_) {
                display_->ShowNotification(
                    std::string(Lang::Strings::VOLUME) + std::to_string(volume_), 1500);
            }
            ESP_LOGI(TAG, "BUTTON: Vol+ clicked -> volume=%d", volume_);
        });
        volume_down_button_.OnClick([this]() {
            if (audio_codec_) {
                volume_ = audio_codec_->output_volume();
                volume_ = std::max<int>(0, volume_ - 10);
                audio_codec_->SetOutputVolume(volume_);
            }
            if (display_) {
                display_->ShowNotification(
                    std::string(Lang::Strings::VOLUME) + std::to_string(volume_), 1500);
            }
            ESP_LOGI(TAG, "BUTTON: Vol- clicked -> volume=%d", volume_);
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
        ESP_LOGI(TAG, "ctor: BODY START (after member init)");
        // Step 1 fix: factory-derived defensive sequence (irom.asm @ 0x4201d500).
        // The factory binary re-asserts GPIO 13 LOW then HIGH BEFORE spi_bus_initialize,
        // which is the root-cause fix for the ESP_ERR_INVALID_STATE (0x103) crash on
        // esp_lcd_new_panel_io_spi(SPI2_HOST, ...).
        ESP_LOGI(TAG, "ctor: configuring power-hold GPIO");
        gpio_config_t power_hold_conf = {
            .pin_bit_mask = (1ULL << POWER_HOLD_PIN),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&power_hold_conf);
        ESP_LOGI(TAG, "ctor: power-hold GPIO configured");
        gpio_set_level(POWER_HOLD_PIN, 0);  // defensive re-latch LOW
        gpio_set_level(POWER_HOLD_PIN, 1);  // re-latch HIGH
        ESP_LOGI(TAG, "ctor: power-hold re-latched");

        // LCD pin matrix cleanup so IO MUX is clean before spi_bus_initialize.
        const gpio_num_t lcd_pins[] = {
            DISPLAY_SPI_MOSI_PIN, DISPLAY_SPI_SCLK_PIN,
            DISPLAY_SPI_CS_PIN,   DISPLAY_SPI_DC_PIN,
            DISPLAY_SPI_RESET_PIN,
        };
        for (gpio_num_t p : lcd_pins) {
            gpio_reset_pin(p);
        }
        ESP_LOGI(TAG, "ctor: LCD pins reset");

        ESP_LOGI(TAG, "ctor: calling InitializeLcdDisplay");
        InitializeLcdDisplay();
        ESP_LOGI(TAG, "Step 2: InitializeLcdDisplay done (DIAG: panel draw done)");
        ESP_LOGI(TAG, "ctor: calling InitializeButtons");
        InitializeButtons();
        ESP_LOGI(TAG, "ctor: InitializeButtons done");
        ESP_LOGI(TAG, "ctor: calling InitializeIot");
        InitializeIot();
        ESP_LOGI(TAG, "ctor: InitializeIot done");
        ESP_LOGI(TAG, "ctor: creating PowerManager");
        power_manager_ = new PowerManager(POWER_CHARGE_PIN);
        ESP_LOGI(TAG, "ctor: PowerManager created");
        InitializePowerSaveTimer();

        // Step 5: Initialize VbAduioCodec over UART1 (TX=20, RX=10)
        ESP_LOGI(TAG, "ctor: creating VbAduioCodec (TX=%d, RX=%d)", CODEC_TX_GPIO, CODEC_RX_GPIO);
        audio_codec_ = new VbAduioCodec(CODEC_TX_GPIO, CODEC_RX_GPIO);
        audio_codec_->Start();
        // Use vb6824_audio_set_output_volume directly here; audio_codec_->SetOutputVolume
        // calls Board::GetInstance() which deadlocks if called inside the Board constructor!
        vb6824_audio_set_output_volume(100);

        // Play an audible boot chime: 3 ascending musical notes (C5=523Hz, E5=659Hz, G5=784Hz)
        ESP_LOGI(TAG, "ctor: playing boot chime via VB6824...");
        PlayTone(523, 100); // C5
        PlayTone(659, 100); // E5
        PlayTone(784, 150); // G5

        audio_codec_->OnWakeUp([this](const std::string& command) {
            ESP_LOGI(TAG, "VB6824: Wake up command: '%s'", command.c_str());
            if ((command.find("hello baby") != std::string::npos ||
                 command.find("halo baby") != std::string::npos) &&
                Application::GetInstance().GetDeviceState() != kDeviceStateListening) {
                Application::GetInstance().WakeWordInvoke("hello baby");
            }
        });
        ESP_LOGI(TAG, "ctor: VbAduioCodec created and wake callback registered");

        // The Application's protocol (Mz01XiaozhiProtocol) reads its server
        // URL from NVS namespace "websocket"; keep the self-hosted repoint
        // explicit so a fresh NVS still reaches the local server.
        Settings ws_settings("websocket", true);
        ws_settings.SetString("url", "ws://192.168.31.220:8000");
        ESP_LOGI(TAG, "ctor: websocket url configured (NVS 'websocket'/'url')");

        // Register MCP tools from a deferred task: AddCommonTools() calls
        // Board::GetInstance(), which would deadlock inside this constructor
        // (same __cxa_guard re-entrancy as SetOutputVolume).
        xTaskCreate([](void* arg) {
            vTaskDelay(pdMS_TO_TICKS(3000));
            McpServer::GetInstance().AddCommonTools();
            ESP_LOGI("Mz01C3Lcd", "MCP common tools registered");
            vTaskDelete(NULL);
        }, "mz01_mcp_init", 4096, NULL, 1, NULL);

        ESP_LOGI(TAG, "ctor: complete (Step 8 build — xiaozhi protocol via Application)");
    }

    virtual AudioCodec* GetAudioCodec() override {
        // audio_codec_ is always created in the ctor; the old DummyAudioCodec
        // fallback is gone because it is abstract in EYE_STYLE opus mode.
        return audio_codec_;
    }

    virtual Display* GetDisplay() override {
        return display_;  // Official SpiLcdDisplay (LVGL UI)
    }

    virtual Backlight* GetBacklight() override {
        static Mz01Backlight backlight;
        return &backlight;
    }

    virtual bool NeedSilentStartup() override {
        return false;
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
