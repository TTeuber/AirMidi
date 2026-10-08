// airmidi firmware entry point. Phase 1: bring the board up, say who we are (version, board, why we reset), load
// the settings, and blink the LED as a heartbeat. Transports arrive in phase 2.
#include "airmidi/settings.h"
#include "board.h"
#include "driver/gpio.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_settings.h"

static const char *TAG = "airmidi";

static const char *reset_reason(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_POWERON:
            return "power on";
        case ESP_RST_EXT:
            return "external pin";
        case ESP_RST_SW:
            return "software restart";
        case ESP_RST_PANIC:
            return "panic";
        case ESP_RST_INT_WDT:
            return "interrupt watchdog";
        case ESP_RST_TASK_WDT:
            return "task watchdog";
        case ESP_RST_WDT:
            return "other watchdog";
        case ESP_RST_DEEPSLEEP:
            return "deep sleep wake";
        case ESP_RST_BROWNOUT:
            return "brownout";
        case ESP_RST_USB:
            return "USB";
        case ESP_RST_JTAG:
            return "JTAG";
        default:
            return "unknown";
    }
}

void app_main(void) {
    const esp_app_desc_t *app = esp_app_get_description();
    const board_t *b = board();
    ESP_LOGI(TAG, "airmidi %s, built %s %s with ESP-IDF %s", app->version, app->date, app->time, app->idf_ver);
    ESP_LOGI(TAG, "board: %s", b->name);
    ESP_LOGI(TAG, "reset reason: %s", reset_reason(esp_reset_reason()));

    board_init();
    ESP_ERROR_CHECK(nvs_settings_init());

    for (bool on = false;; on = !on) {
        gpio_set_level(b->led, on);
        vTaskDelay(pdMS_TO_TICKS(on ? 50 : 1950));
    }
}
