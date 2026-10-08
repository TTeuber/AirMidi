// User settings: what they are, their defaults, and what counts as valid. Storage is elsewhere (NVS on the ESP32,
// firmware/components/nvs_settings); keeping the rules here lets the host tests cover them.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AIRMIDI_MODE_PLAY,     // BLE + TRS, Wi-Fi off: lowest power and latency
    AIRMIDI_MODE_NETWORK,  // Wi-Fi station + network MIDI + web UI, BLE still on
    AIRMIDI_MODE_SETUP,    // own access point + web UI, for the first Wi-Fi join
    AIRMIDI_MODE_COUNT
} airmidi_mode_t;

#define SETTINGS_NAME_MAX 31  // BLE device name, mDNS host name and UMP endpoint name; ASCII letters, digits, '-'
#define SETTINGS_SYSEX_TIMEOUT_MIN 50
#define SETTINGS_SYSEX_TIMEOUT_MAX 5000

typedef struct {
    char name[SETTINGS_NAME_MAX + 1];
    airmidi_mode_t mode;
    uint16_t sysex_timeout_ms;  // how long a SysEx in progress may hold other sources off a merged output
    bool ble_in_network;        // keep BLE on in Network mode (costs network MIDI ~16 ms: the radio is shared)
} settings_t;

// Defaults; the name ends in the last two bytes of the MAC address so two boxes in one room differ.
void settings_defaults(settings_t *s, const uint8_t mac[6]);

// NULL if valid, else a short reason (for logs and the web UI).
const char *settings_check(const settings_t *s);

const char *airmidi_mode_name(airmidi_mode_t m);

#ifdef __cplusplus
}
#endif
