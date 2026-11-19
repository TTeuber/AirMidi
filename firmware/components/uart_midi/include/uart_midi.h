// TRS MIDI: J3 IN -> UART RX -> midi1_in -> bus (PORT_TRS), and bus -> midi1_out -> UART TX -> J2 OUT.
// Rev D takes either TRS type on both jacks (board_t.trs_ab): IN listens on both optocouplers, and OUT sends on
// whichever line the type makes the data line, with nothing driven while no receiver is plugged in.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "airmidi/settings.h"

// out_type: the setting (TRS_TYPE_AUTO follows the type heard on IN). Ignored on boards wired for Type A only.
void uart_midi_start(trs_type_t out_type);

// Change the OUT type setting while running (rev D).
void uart_midi_set_out_type(trs_type_t out_type);

typedef struct {
    uint32_t rx_bytes, rx_dropped_bytes;  // dropped: stray data bytes / undefined statuses (midi1_in)
    uint32_t tx_bytes, tx_dropped;        // dropped: UART TX buffer full
    // Rev D only (both_types):
    bool both_types;
    char in_type;           // 'A' or 'B': the type last heard on IN; 0 before anything arrived
    char out_type;          // 'A' or 'B': the type OUT sends as
    bool out_plugged;       // a receiver in OUT: the lines are driven (otherwise parked low, messages not sent)
    uint32_t tx_unplugged;  // messages not sent because no receiver was in OUT
} uart_midi_stats_t;

void uart_midi_get_stats(uart_midi_stats_t *out);
