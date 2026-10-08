// Network MIDI 2.0 (UDP) host on port 5673: up to two clients (PORT_NET_0/1), announced over mDNS as _midi2._udp.
// The session protocol is lib/nm2 (FEC, retransmit, reordering), endpoint discovery lib/ump_endpoint; this file
// owns the socket, the task and the bridge to the bus.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "airmidi/nm2.h"

#define NET_MIDI_PORT 5673

// Start the host (once). `name` is the UMP Endpoint Name; the product instance id is the MAC address.
void net_midi_start(const char *name);

typedef struct {
    bool active;
    char peer[64];
    char ip[16];
    uint8_t protocol;  // 1 = MIDI 1.0, 2 = MIDI 2.0
    nm2_stats_t stats;
} net_midi_session_info_t;

void net_midi_get_info(int session, net_midi_session_info_t *out);
