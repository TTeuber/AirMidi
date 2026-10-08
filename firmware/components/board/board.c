#include "board.h"

#include "driver/gpio.h"
#include "sdkconfig.h"

#if CONFIG_AIRMIDI_BOARD_XIAO_C6
static const board_t the_board = {
    .name = "receiver rev A + XIAO ESP32-C6",
    .midi_tx = GPIO_NUM_16,
    .midi_rx = GPIO_NUM_17,
    .led = GPIO_NUM_1,
    .button = GPIO_NUM_2,
    .batt_adc_unit = ADC_UNIT_1,
    .batt_adc_channel = ADC_CHANNEL_0,  // GPIO0
};
// Inside the XIAO C6 module: GPIO3 low powers the RF switch, GPIO14 low selects the ceramic antenna (high = the
// U.FL connector, measured ~20 dB weaker with nothing plugged in).
#define RF_SWITCH_EN GPIO_NUM_3
#define ANT_SELECT GPIO_NUM_14
#elif CONFIG_AIRMIDI_BOARD_XIAO_S3
static const board_t the_board = {
    .name = "receiver rev A + XIAO ESP32-S3",
    .midi_tx = GPIO_NUM_43,
    .midi_rx = GPIO_NUM_44,
    .led = GPIO_NUM_2,
    .button = GPIO_NUM_3,
    .batt_adc_unit = ADC_UNIT_1,
    .batt_adc_channel = ADC_CHANNEL_0,  // GPIO1
};
#else
#error "no board selected (CONFIG_AIRMIDI_BOARD)"
#endif

const board_t *board(void) { return &the_board; }

void board_init(void) {
#ifdef RF_SWITCH_EN
    gpio_config_t rf = {.pin_bit_mask = 1ULL << RF_SWITCH_EN | 1ULL << ANT_SELECT, .mode = GPIO_MODE_OUTPUT};
    gpio_config(&rf);
    gpio_set_level(RF_SWITCH_EN, 0);
    gpio_set_level(ANT_SELECT, 0);
#endif
    gpio_config_t led = {.pin_bit_mask = 1ULL << the_board.led, .mode = GPIO_MODE_OUTPUT};
    gpio_config(&led);
    gpio_set_level(the_board.led, 0);
    gpio_config_t btn = {
        .pin_bit_mask = 1ULL << the_board.button, .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE};
    gpio_config(&btn);
}
