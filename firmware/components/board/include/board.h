// The one place that knows which pin is what. The receiver rev A PCB takes either XIAO module on the same
// headers, so both boards share the signals and differ only in GPIO numbers (and the C6's internal RF switch). A bare
// ESP32-S3-DevKitC-1 is a third board, for USB MIDI and ESP-NOW without a second receiver PCB. Rev B keeps rev A's
// MIDI and battery pins and swaps the one button and LED for a panel: a button per mode with an LED over each. Rev C
// has rev B's panel on a bare S3 module: other GPIOs, one USB-C.
#pragma once

#include "esp_adc/adc_oneshot.h"
#include "hal/gpio_types.h"

typedef struct {
    const char *name;
    gpio_num_t midi_tx;  // D6 -> J2 MIDI OUT (UART0's pin; driven by UART1 through the GPIO matrix)
    gpio_num_t midi_rx;  // D7 <- J3 MIDI IN (6N138 output, 1k pull-up: idles high)
    gpio_num_t led;      // D1, 3 mm LED + 1k to GND, active high (GPIO_NUM_NC: none, or a panel instead)
    gpio_num_t button;   // D2, to GND, internal pull-up: active low (GPIO_NUM_NC: a panel instead)
    // Rev B panel, in lib/ui's panel_key_t order (BLE, NET, STP, PWR): LEDs active high through their resistor,
    // buttons to GND with the internal pull-up. PWR's button is an RTC GPIO, so it can wake the chip from deep sleep.
    bool panel;
    gpio_num_t panel_led[4];
    gpio_num_t panel_button[4];
    // PWM duty at full brightness: the colours differ, and 3 mm LEDs are bright on a dark panel
    uint8_t panel_led_max[4];
    gpio_num_t opto_en;        // Rev C: held low to power the H11L1, which draws ~1 mA otherwise. 0 = always powered
    gpio_num_t vbus_sense;     // Rev C: high while USB is plugged in (self-powered USB device). 0 = none
    adc_unit_t batt_adc_unit;  // A0: half the switched cell voltage (1:1 divider)
    int batt_adc_channel;      // -1: no battery (a dev kit on USB)
    int batt_offset_mv;        // added to the cell reading, from comparing it with a multimeter on this board
    int8_t wifi_max_tx;        // Wi-Fi transmit power limit in 0.25 dBm, 0 = the chip's own (20 dBm)
} board_t;

// The board this firmware was built for (Kconfig AIRMIDI_BOARD).
const board_t *board(void);

// The box's id: six hex digits from the chip's base MAC. The same box shows it over BLE (scan response,
// manufacturer data FFFF "am" + 3 bytes), mDNS (TXT id=) and /api/status, so the app can tell that a box it sees
// over BLE and one it sees on Wi-Fi are the same. The name can be changed; this cannot.
const char *board_id(void);
void board_id_bytes(uint8_t out[3]);

// Board-specific setup that has to happen before the radio starts (C6: power the RF switch and pick the ceramic
// antenna). Leaves the LED off and the button an input with pull-up (the panel's: ui_io sets them up).
void board_init(void);
