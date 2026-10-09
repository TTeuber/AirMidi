// BLE MIDI (NimBLE), both roles. Peripheral: the BLE MIDI service, up to two centrals at once (PORT_BLE_0/1).
// Central: the box connects by itself to one BLE MIDI device, a keyboard or controller (PORT_BLE_CENTRAL), chosen
// in the settings ("*" for the first one found, its name, or its address). Each connection has its own parser (running
// status and SysEx continue across packets) and packetizer sized to its MTU.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "airmidi/settings.h"
#include "host/ble_hs.h"

// Start NimBLE, advertise as `name`, and look for `device` (settings_t.ble_device). Needs NVS initialised.
void ble_midi_start(const char *name, const char *device);

typedef struct {
    bool connected, subscribed;
    uint16_t interval_x125;  // connection interval in units of 1.25 ms
    uint16_t mtu;
    uint32_t rx_packets, rx_bad, tx_packets, tx_failed;
} ble_midi_slot_stats_t;

// Slots 0 and 1: centrals connected to us; slot 2: the device we connected to.
void ble_midi_get_stats(int slot, ble_midi_slot_stats_t *out);

// True once the NimBLE host has synced with the controller and advertising started (self-test).
bool ble_midi_ready(void);

// Advertise about once a second instead of every 100-150 ms (while Wi-Fi shares the radio).
void ble_midi_set_slow_advertising(bool slow);

// True while at least one connection is up (for the LED).
bool ble_midi_connected(void);

// ---- central -------------------------------------------------------------------------------------------------

typedef enum {
    BLE_CENTRAL_OFF,
    BLE_CENTRAL_SCANNING,
    BLE_CENTRAL_CONNECTING,
    BLE_CENTRAL_CONNECTED
} ble_midi_central_state_t;

#define BLE_MIDI_FOUND_MAX 8

typedef struct {
    ble_addr_t addr;
    char name[SETTINGS_DEVICE_MAX + 1];  // empty until its scan response arrives, or for good
    char address[18];                    // "aa:bb:cc:dd:ee:ff", usable as the target instead of the name
    int8_t rssi;
    bool midi;
    uint32_t seen_ms;
} ble_midi_found_t;

typedef struct {
    ble_midi_central_state_t state;
    char target[SETTINGS_DEVICE_MAX + 1];
    char name[SETTINGS_DEVICE_MAX + 1];  // the device connected (or last connected) to
    bool listing;                        // a scan for the web page is running
    ble_midi_slot_stats_t stats;
} ble_midi_central_info_t;

// Change the device to connect to (no restart): "" none, "*" any, a name or an address.
void ble_midi_set_device(const char *device);

// Scan for 5 s to list the BLE MIDI devices around (for the web page); results in ble_midi_found().
void ble_midi_scan_list(void);
int ble_midi_found(ble_midi_found_t *out, int max);

void ble_midi_central_info(ble_midi_central_info_t *out);
const char *ble_midi_central_state_name(ble_midi_central_state_t s);
