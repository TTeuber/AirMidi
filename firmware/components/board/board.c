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
#elif CONFIG_AIRMIDI_BOARD_XIAO_S3_B
// pcb/midi_receiver/circuit.py PANEL in the design workspace: LEDs A1 A3 SDA SCL, buttons SCK MISO MOSI A2.
static const board_t the_board = {
    .name = "receiver rev B + XIAO ESP32-S3",
    .midi_tx = GPIO_NUM_43,
    .midi_rx = GPIO_NUM_44,  // H11L1 output, 270R pull-up
    .led = GPIO_NUM_NC,
    .button = GPIO_NUM_NC,
    .panel = true,
    .panel_led = {GPIO_NUM_2, GPIO_NUM_4, GPIO_NUM_5, GPIO_NUM_6},  // as built: blue, green (2k), yellow, orange (1k)
    .panel_button = {GPIO_NUM_7, GPIO_NUM_8, GPIO_NUM_9, GPIO_NUM_3},
    .panel_led_max = {25, 25, 100, 35},  // all too bright at full duty, and yellow a little dim against the rest
    .batt_adc_unit = ADC_UNIT_1,
    .batt_adc_channel = ADC_CHANNEL_0,  // GPIO1
};
#elif CONFIG_AIRMIDI_BOARD_REV_C
// pcb/midi_receiver_c/circuit.py in the design workspace. MIDI TX is off UART0's pin, so the ROM's boot log stays out of
// the MIDI output, and off the pins Espressif lists with a power-up glitch; TX and RX stay off VDD_SPI's GPIO33-37.
static const board_t the_board = {
    .name = "receiver rev C",
    .midi_tx = GPIO_NUM_38,
    .midi_rx = GPIO_NUM_39,  // H11L1 output, 270R pull-up, on the switched supply
    .led = GPIO_NUM_NC,
    .button = GPIO_NUM_NC,
    .panel = true,
    .panel_led = {GPIO_NUM_11, GPIO_NUM_13, GPIO_NUM_15, GPIO_NUM_17},  // blue, green (10k), yellow (1.5k), orange (3.3k)
    .panel_button = {GPIO_NUM_12, GPIO_NUM_14, GPIO_NUM_16, GPIO_NUM_18},
    .panel_led_max = {100, 100, 150, 110},  // rev B's judged levels scaled to these resistors: confirm on the first board
    .batt_adc_unit = ADC_UNIT_1,
    .batt_adc_channel = ADC_CHANNEL_9,  // GPIO10
    .opto_en = GPIO_NUM_40,
    .vbus_sense = GPIO_NUM_21,
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
    if (the_board.opto_en) {  // before the UART looks at its line; the pin floats in deep sleep and a pull-up turns it off
        gpio_config_t en = {.pin_bit_mask = 1ULL << the_board.opto_en, .mode = GPIO_MODE_OUTPUT};
        gpio_config(&en);
        gpio_set_level(the_board.opto_en, 0);
    }
    if (the_board.led != GPIO_NUM_NC) {
        gpio_config_t led = {.pin_bit_mask = 1ULL << the_board.led, .mode = GPIO_MODE_OUTPUT};
        gpio_config(&led);
        gpio_set_level(the_board.led, 0);
    }
    if (the_board.button != GPIO_NUM_NC) {
        gpio_config_t btn = {
            .pin_bit_mask = 1ULL << the_board.button, .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE};
        gpio_config(&btn);
    }
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
