// The ESP-NOW cable (PORT_ESPNOW): MIDI between airmidi boxes in the same group, straight from radio to radio
// (lib/espnow_link: finding each other, channels, duplicates). This file owns the ESP-NOW driver, the task and the
// bridge to the bus. Needs the Wi-Fi radio on (net_keep_radio_on in Play mode).
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "airmidi/espnow_link.h"

// Start (once), after net_start(). `name` goes out in hellos, `group` picks the other boxes.
void espnow_midi_start(const char *name, const char *group);

typedef struct {
    bool running;
    uint8_t channel;
    bool on_ap_channel;
    int n_peers;
    char peers[ESPNOW_PEERS][32];
    espnow_stats_t stats;
    uint32_t tx_failed;             // esp_now_send() refused a frame
    int last_tx_error;              // its last error
    uint32_t tx_acked, tx_unacked;  // unicast frames the peer's radio acknowledged, or never did
} espnow_midi_info_t;

void espnow_midi_get_info(espnow_midi_info_t *out);
