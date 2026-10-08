// TRS MIDI: J3 IN -> UART RX -> midi1_in -> bus (PORT_TRS), and bus -> midi1_out -> UART TX -> J2 OUT.
#pragma once

#include <stdint.h>

void uart_midi_start(void);

typedef struct {
    uint32_t rx_bytes, rx_dropped_bytes;  // dropped: stray data bytes / undefined statuses (midi1_in)
    uint32_t tx_bytes, tx_dropped;        // dropped: UART TX buffer full
} uart_midi_stats_t;

void uart_midi_get_stats(uart_midi_stats_t *out);
