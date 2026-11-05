#include "power.h"

#include "airmidi/ui.h"
#include "board.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/soc_caps.h"

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
    if (b->batt_adc_channel < 0) {  // no battery on this board: always on USB
        battery_update(&battery, 0);
        power_set_mode(1);
        ESP_LOGI(TAG, "no battery on this board");
        return;
    }
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
    power_set_mode(1);  // frequency scaling: no MIDI lost (tools/hil/sleep_test.py), see docs/measurements.md
    xTaskCreate(sample_task, "battery", 3072, NULL, 3, NULL);
}

static int pm_mode;

void power_set_mode(int mode) {
    esp_pm_config_t c = {.max_freq_mhz = 160, .min_freq_mhz = mode >= 1 ? 40 : 160, .light_sleep_enable = mode >= 2};
    esp_err_t e = esp_pm_configure(&c);
    if (e == ESP_OK) pm_mode = mode;
    ESP_LOGI(TAG, "power mode %d: %d-%d MHz, light sleep %s (%s)", mode, c.min_freq_mhz, c.max_freq_mhz,
             c.light_sleep_enable ? "on" : "off", esp_err_to_name(e));
}

int power_mode(void) { return pm_mode; }

uint32_t power_cell_mv(void) { return battery.mv; }
int power_percent(void) { return battery_percent(battery.mv); }
bool power_low(void) { return battery.low; }
bool power_on_usb(void) { return battery.absent; }
bool power_empty(void) { return battery.empty; }

// Deep sleep with the button (active low, RTC GPIO) as the wake source. A press still under way would wake the
// chip at once, so wait for the release first. The RTC pull-up holds the pin high while asleep (ext0 keeps the RTC
// peripherals powered; chips without ext0, the C6, use ext1 and keep them on by hand).
static void __attribute__((noreturn)) sleep_until_pressed(gpio_num_t button) {
    while (gpio_get_level(button) == 0) vTaskDelay(pdMS_TO_TICKS(10));
    vTaskDelay(pdMS_TO_TICKS(50));  // release bounce
#if SOC_PM_SUPPORT_EXT0_WAKEUP
    ESP_ERROR_CHECK(esp_sleep_enable_ext0_wakeup(button, 0));
#else
    ESP_ERROR_CHECK(esp_sleep_enable_ext1_wakeup_io(1ULL << button, ESP_EXT1_WAKEUP_ANY_LOW));
#if SOC_PM_SUPPORT_RTC_PERIPH_PD
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
#endif
#endif
    rtc_gpio_pullup_en(button);
    rtc_gpio_pulldown_dis(button);
    esp_deep_sleep_start();
}

void power_shutdown(void) {
    ESP_LOGW(TAG, "battery empty (%lu mV): sleeping until PWR or the switch", (unsigned long)battery.mv);
    vTaskDelay(pdMS_TO_TICKS(100));  // let the log out
    if (board()->panel) sleep_until_pressed(board()->panel_button[PANEL_PWR]);  // rev C has no switch to cycle
    esp_deep_sleep_start();
}

void power_off(gpio_num_t button) {
    ESP_LOGI(TAG, "power off: hold PWR %d ms to start again", POWER_ON_HOLD_MS);
    vTaskDelay(pdMS_TO_TICKS(100));  // let the log out
    sleep_until_pressed(button);
}

void power_confirm_wake(gpio_num_t button) {
    esp_sleep_wakeup_cause_t why = esp_sleep_get_wakeup_cause();
    if (why != ESP_SLEEP_WAKEUP_EXT0 && why != ESP_SLEEP_WAKEUP_EXT1) return;
    rtc_gpio_deinit(button);  // back to a digital pin
    gpio_config_t in = {.pin_bit_mask = 1ULL << button, .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE};
    gpio_config(&in);
    // Uptime counts from shortly after the press that woke the chip (boot takes a few hundred ms of it).
    while (esp_timer_get_time() < POWER_ON_HOLD_MS * 1000LL) {
        if (gpio_get_level(button) != 0) {
            ESP_LOGI(TAG, "PWR let go before %d ms: back to sleep", POWER_ON_HOLD_MS);
            sleep_until_pressed(button);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    ESP_LOGI(TAG, "woken by PWR");
}
