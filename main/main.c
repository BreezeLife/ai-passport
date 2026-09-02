#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "workbuddy_app.h"

static const char *TAG = "main";

void app_main(void)
{
    ESP_LOGI(TAG, "WorkBuddy AI Passport starting (%s mode)",
             WB_APP_DEMO_MODE ? "demo" : "live");
    esp_sleep_wakeup_cause_t wakeup = esp_sleep_get_wakeup_cause();
    if (wakeup != ESP_SLEEP_WAKEUP_UNDEFINED) {
        ESP_LOGI(TAG, "wake cause: %d", (int)wakeup);
    }

    esp_err_t i2c_error = bsp_i2c_init();
    if (i2c_error != ESP_OK) {
        ESP_LOGW(TAG, "shared I2C unavailable: %s", esp_err_to_name(i2c_error));
    } else {
        bsp_i2c_scan();
    }

    if (bsp_display_init() != ESP_OK || bsp_lvgl_init() == NULL) {
        ESP_LOGE(TAG,
                 "display/LVGL unavailable; check MOSI=%d SCLK=%d CS=%d DC=%d BL=%d",
                 BSP_LCD_MOSI, BSP_LCD_SCLK, BSP_LCD_CS, BSP_LCD_DC, BSP_LCD_BL);
        return;
    }
    bsp_display_backlight(80U);

    bool battery_available = bsp_battery_init() == ESP_OK;
    if (!battery_available) {
        ESP_LOGW(TAG, "battery gauge unavailable; continuing without a percentage");
    }

    if (!bsp_lvgl_lock(2000)) {
        ESP_LOGE(TAG, "unable to acquire LVGL lock for application startup");
        return;
    }
    esp_err_t app_error = wb_app_start(battery_available);
    bsp_lvgl_unlock();
    if (app_error != ESP_OK) {
        ESP_LOGE(TAG, "WorkBuddy application startup failed: %s",
                 esp_err_to_name(app_error));
        return;
    }

    esp_err_t button_error = bsp_button_init(wb_app_button_callback, NULL);
    if (button_error != ESP_OK) {
        ESP_LOGE(TAG, "buttons unavailable: %s", esp_err_to_name(button_error));
        return;
    }

    ESP_LOGI(TAG, "WorkBuddy ready: privacy cover, messages, tasks, outputs, voice review");
}
