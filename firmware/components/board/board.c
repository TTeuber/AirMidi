#include "board.h"

#include <stdio.h>

#include "driver/gpio.h"
#include "esp_mac.h"
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
    .batt_offset_mv = 14,               // board #1: calibrated ADC read 14 mV under the meter (2026-10-05)
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
#elif CONFIG_AIRMIDI_BOARD_DEVKITC_S3
static const board_t the_board = {
    .name = "ESP32-S3-DevKitC-1",
    .midi_tx = GPIO_NUM_17,  // free pins (43/44 go to the kit's USB-UART bridge)
    .midi_rx = GPIO_NUM_18,
    .led = GPIO_NUM_NC,    // only an addressable RGB LED on the kit
    .button = GPIO_NUM_0,  // BOOT, with the kit's pull-up
    .batt_adc_unit = ADC_UNIT_1,
    .batt_adc_channel = -1,  // no battery: always "on USB"
    // At the full 20 dBm none of the kit's Wi-Fi frames arrived (no ESP-NOW acknowledgement, no access point
    // answering its authentication) while BLE worked; at 10 dBm all did. Most likely its 3.3 V sagging at the
    // transmit peaks on a USB port.
    .wifi_max_tx = 60,
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
    if (the_board.led != GPIO_NUM_NC) {
        gpio_config_t led = {.pin_bit_mask = 1ULL << the_board.led, .mode = GPIO_MODE_OUTPUT};
        gpio_config(&led);
        gpio_set_level(the_board.led, 0);
    }
    gpio_config_t btn = {
        .pin_bit_mask = 1ULL << the_board.button, .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE};
    gpio_config(&btn);
}

// The Wi-Fi station MAC's last three bytes. (Not the default MAC: on the C6 that reads as the 8-byte EUI-64 of its
// 802.15.4 radio, ff:fe in the middle.) 8 bytes of buffer all the same, for any chip that writes more.
void board_id_bytes(uint8_t out[3]) {
    uint8_t mac[8] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    out[0] = mac[3], out[1] = mac[4], out[2] = mac[5];
}

const char *board_id(void) {
    static char id[7];
    if (!id[0]) {
        uint8_t b[3];
        board_id_bytes(b);
        snprintf(id, sizeof id, "%02x%02x%02x", b[0], b[1], b[2]);
    }
    return id;
}
