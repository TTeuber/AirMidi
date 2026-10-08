// The one place that knows which pin is what. The receiver rev A PCB takes either XIAO module on the same
// headers, so both boards share the signals and differ only in GPIO numbers (and the C6's internal RF switch).
#pragma once

#include "esp_adc/adc_oneshot.h"
#include "hal/gpio_types.h"

typedef struct {
    const char *name;
    gpio_num_t midi_tx;        // D6 -> J2 MIDI OUT (UART0's pin; driven by UART1 through the GPIO matrix)
    gpio_num_t midi_rx;        // D7 <- J3 MIDI IN (6N138 output, 1k pull-up: idles high)
    gpio_num_t led;            // D1, 3 mm LED + 1k to GND, active high
    gpio_num_t button;         // D2, to GND, internal pull-up: active low
    adc_unit_t batt_adc_unit;  // A0: half the switched cell voltage (1:1 divider)
    adc_channel_t batt_adc_channel;
} board_t;

// The board this firmware was built for (Kconfig AIRMIDI_BOARD).
const board_t *board(void);

// Board-specific setup that has to happen before the radio starts (C6: power the RF switch and pick the ceramic
// antenna). Leaves the LED off and the button an input with pull-up.
void board_init(void);
