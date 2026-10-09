// Wi-Fi per mode: Play = off; Network = station, joining the stored networks in turn; Setup = an open access
// point named after the box (192.168.4.1) for the first join. mDNS announces <name>.local in Network and Setup.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "airmidi/settings.h"
#include "esp_err.h"

typedef enum { NET_OFF, NET_JOINING, NET_CONNECTED, NET_AP } net_state_t;

// Initialise (once) and apply a mode. `name` is the mDNS host name and the access point's SSID.
void net_start(airmidi_mode_t mode, const char *name);
void net_set_mode(airmidi_mode_t mode);

net_state_t net_state(void);
const char *net_state_name(net_state_t s);
// IPv4 address as text ("" while there is none) and the signal strength of the joined network (0 if none).
void net_ip(char out[16]);
// SSID of the joined network ("" if none).
void net_ssid(char out[33]);
int net_rssi(void);

// Stored networks (NVS namespace "wifi", up to NET_MAX_NETWORKS, newest first). Passwords never leave the box.
#define NET_MAX_NETWORKS 5
int net_networks(char ssids[][33], int max);
esp_err_t net_add_network(const char *ssid, const char *pass);
esp_err_t net_remove_network(const char *ssid);

// Announce an extra mDNS service (e.g. _http._tcp on port 80) for as long as Wi-Fi is up.
// Keep the Wi-Fi radio on in Play mode too, without joining anything (ESP-NOW). Call before net_start().
void net_keep_radio_on(bool on);

// The channel of the access point the box is joined to, 0 when not joined.
uint8_t net_ap_channel(void);

// True once SNTP has set the clock (wall-clock time).
bool net_time_valid(void);

void net_add_service(const char *type, const char *proto, uint16_t port);
