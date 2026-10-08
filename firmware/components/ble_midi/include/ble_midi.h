// BLE MIDI peripheral (NimBLE): the BLE MIDI service, up to two centrals at once (PORT_BLE_0, PORT_BLE_1), each
// with its own parser (running status and SysEx continue across packets) and packetizer sized to its MTU.
#pragma once

#include <stdbool.h>
#include <stdint.h>

// Start NimBLE and advertise as `name`. Needs NVS initialised.
void ble_midi_start(const char *name);

typedef struct {
    bool connected, subscribed;
    uint16_t interval_x125;  // connection interval in units of 1.25 ms
    uint16_t mtu;
    uint32_t rx_packets, rx_bad, tx_packets, tx_failed;
} ble_midi_slot_stats_t;

void ble_midi_get_stats(int slot, ble_midi_slot_stats_t *out);

// True while at least one central is connected (for the LED).
bool ble_midi_connected(void);
