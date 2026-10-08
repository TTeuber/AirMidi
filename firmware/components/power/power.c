#include "power.h"

#include "airmidi/ui.h"
#include "board.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "power";

#define SAMPLES 16

static adc_oneshot_unit_handle_t adc;
static adc_cali_handle_t cali;
static battery_t battery;

static void sample_task(void *arg) {
    const board_t *b = board();
    for (int n = 0;; n++) {
        int sum = 0, mv;
        for (int i = 0; i < SAMPLES; i++) {
            int raw;
            adc_oneshot_read(adc, b->batt_adc_channel, &raw);
            adc_cali_raw_to_voltage(cali, raw, &mv);
            sum += mv;
        }
        // A0 sees half the cell; the board's correction comes from a meter comparison.
        int cell = 2 * sum / SAMPLES + b->batt_offset_mv;
        battery_update(&battery, cell > 0 ? cell : 0);
        if (n % 60 == 0) {
            if (battery.absent)
                ESP_LOGI(TAG, "no cell voltage: power switch off, running from USB");
            else
                ESP_LOGI(TAG, "cell %lu mV (%d %%)%s", (unsigned long)battery.mv, battery_percent(battery.mv),
                         battery.low ? ", low" : "");
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void power_start(void) {
    const board_t *b = board();
    battery_init(&battery);
    adc_oneshot_unit_init_cfg_t uc = {.unit_id = b->batt_adc_unit};
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&uc, &adc));
    adc_oneshot_chan_cfg_t cc = {.atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT};
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc, b->batt_adc_channel, &cc));
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cf = {.unit_id = b->batt_adc_unit,
                                          .chan = b->batt_adc_channel,
                                          .atten = ADC_ATTEN_DB_12,
                                          .bitwidth = ADC_BITWIDTH_DEFAULT};
    ESP_ERROR_CHECK(adc_cali_create_scheme_curve_fitting(&cf, &cali));
#else
    adc_cali_line_fitting_config_t lf = {
        .unit_id = b->batt_adc_unit, .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT};
    ESP_ERROR_CHECK(adc_cali_create_scheme_line_fitting(&lf, &cali));
#endif
    xTaskCreate(sample_task, "battery", 3072, NULL, 3, NULL);
}

uint32_t power_cell_mv(void) { return battery.mv; }
int power_percent(void) { return battery_percent(battery.mv); }
bool power_low(void) { return battery.low; }
bool power_on_usb(void) { return battery.absent; }
bool power_empty(void) { return battery.empty; }

void power_shutdown(void) {
    ESP_LOGW(TAG, "battery empty (%lu mV): sleeping until the switch is cycled", (unsigned long)battery.mv);
    vTaskDelay(pdMS_TO_TICKS(100));  // let the log out
    esp_deep_sleep_start();
}
