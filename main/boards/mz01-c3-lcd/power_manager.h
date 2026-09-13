#ifndef __POWER_MANAGER_H__
#define __POWER_MANAGER_H__

#include <driver/gpio.h>
#include "config.h"
#include <esp_adc/adc_oneshot.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_sleep.h>

// Minimal power manager for the MZ01 board:
// - battery gauge via ADC (GPIO4 / ADC1_CH4), vendor thresholds 557..680 mV
// - charging detection on POWER_CHARGE_PIN
// - power latch on POWER_HOLD_PIN (keep high while running)
class PowerManager {
private:
    static constexpr size_t ADC_VALUES_COUNT = 10;

    esp_timer_handle_t timer_handle_ = nullptr;
    gpio_num_t charging_pin_;
    adc_oneshot_unit_handle_t adc_handle_ = nullptr;
    uint16_t adc_values_[ADC_VALUES_COUNT] = {0};
    size_t adc_values_index_ = 0;
    size_t adc_values_count_ = 0;
    uint8_t battery_level_ = 100;
    bool is_charging_ = false;

    void ReadAdc() {
        int mv = 0;
        if (adc_oneshot_read(adc_handle_, BAT_ADC_CHANNEL, &mv) == ESP_OK) {
            adc_values_[adc_values_index_] = mv;
            adc_values_index_ = (adc_values_index_ + 1) % ADC_VALUES_COUNT;
            if (adc_values_count_ < ADC_VALUES_COUNT) adc_values_count_++;
        }
        static uint32_t tick_count = 0;
        if (++tick_count % 5 == 0) {
            ESP_LOGI("PowerManager", "BATTERY: %d%%, Charging=%d, ADC=%d mV",
                     GetBatteryLevel(), IsCharging(), mv);
        }
    }

public:
    PowerManager(gpio_num_t charging_pin) : charging_pin_(charging_pin) {
        // charging detect input
        gpio_config_t io_conf = {
            .pin_bit_mask = (1ULL << charging_pin_),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&io_conf);

        // power latch: hold high
        gpio_config_t hold_conf = {
            .pin_bit_mask = (1ULL << POWER_HOLD_PIN),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&hold_conf);
        gpio_set_level(POWER_HOLD_PIN, 1);

        // adc
        adc_oneshot_unit_init_cfg_t init_config = {
            .unit_id = BAT_ADC_UNIT,
        };
        adc_oneshot_new_unit(&init_config, &adc_handle_);
        adc_oneshot_chan_cfg_t chan_config = {
            .atten = BAT_ADC_ATTEN,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        adc_oneshot_config_channel(adc_handle_, BAT_ADC_CHANNEL, &chan_config);

        esp_timer_create_args_t timer_args = {
            .callback = [](void* arg) { static_cast<PowerManager*>(arg)->ReadAdc(); },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "bat_adc_timer",
        };
        esp_timer_create(&timer_args, &timer_handle_);
        esp_timer_start_periodic(timer_handle_, 1000000);
    }

    uint8_t GetBatteryLevel() {
        if (adc_values_count_ == 0) return 100;
        uint32_t sum = 0;
        for (size_t i = 0; i < adc_values_count_; i++) sum += adc_values_[i];
        uint32_t avg = sum / adc_values_count_;
        if (avg >= BAT_FULL_ADC_MV) return 100;
        if (avg <= BAT_EMPTY_ADC_MV) return 0;
        return (uint8_t)((avg - BAT_EMPTY_ADC_MV) * 100 / (BAT_FULL_ADC_MV - BAT_EMPTY_ADC_MV));
    }

    uint32_t GetAverageAdcMv() {
        uint32_t sum = 0;
        for (size_t i = 0; i < adc_values_count_; i++) sum += adc_values_[i];
        return adc_values_count_ ? sum / adc_values_count_ : 0;
    }

    bool IsCharging() {
        return gpio_get_level(charging_pin_) == 1;
    }

    void PowerOff() {
        ESP_LOGI("PowerManager", "Shutting down");
        gpio_set_level(POWER_HOLD_PIN, 0);
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_deep_sleep_start();
    }
};

#endif // __POWER_MANAGER_H__
