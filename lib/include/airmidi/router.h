// The route matrix: which source port's messages go to which destination port, with a channel filter, a message
// type filter and a channel remap per route. Pure function of (settings, source, message); the firmware's router
// task owns the queues and the transports.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "airmidi/ump.h"

#ifdef __cplusplus
extern "C" {
#endif

// Ports. Fixed numbers so routes can be stored in settings and shown in the web UI.
enum {
    PORT_TRS,          // J2 OUT / J3 IN
    PORT_BLE_0,        // BLE MIDI peripheral, first connection
    PORT_BLE_1,        // second connection
    PORT_BLE_CENTRAL,  // a BLE MIDI device the box connected to itself (keyboard, controller)
    PORT_NET_0,        // Network MIDI 2.0 session
    PORT_NET_1,
    PORT_RTP_0,  // RTP-MIDI (AppleMIDI) session
    PORT_RTP_1,
    PORT_WEB,     // the web UI's control surface
    PORT_USB,     // S3 only
    PORT_ESPNOW,  // box-to-box
    PORT_CI,      // the box's own MIDI-CI initiator: SysEx to and from TRS only
    PORT_COUNT
};

// Message type classes for the type filter (bits).
enum {
    MSG_NOTE = 1 << 0,  // note on / off
    MSG_POLY_PRESSURE = 1 << 1,
    MSG_CONTROL = 1 << 2,  // control change, and MIDI 2.0 RPN / NRPN
    MSG_PROGRAM = 1 << 3,
    MSG_CHANNEL_PRESSURE = 1 << 4,
    MSG_PITCH_BEND = 1 << 5,
    MSG_PER_NOTE = 1 << 6,  // MIDI 2.0 per-note controllers, pitch bend, management
    MSG_SYSEX = 1 << 7,     // SysEx7 and SysEx8 / mixed data set
    MSG_CLOCK = 1 << 8,     // timing clock, start, continue, stop
    MSG_SYSTEM = 1 << 9,    // the other system messages (MTC, song position/select, tune request, ...)
    MSG_OTHER = 1 << 10,    // flex data
    MSG_ALL = (1 << 11) - 1
};

typedef struct {
    bool enabled;
    uint16_t channels;        // bit n = channel n+1 passes (channel voice only)
    uint16_t types;           // MSG_* bits that pass
    uint8_t channel_map[16];  // channel n is sent as channel_map[n]
} route_t;

typedef struct {
    route_t r[PORT_COUNT][PORT_COUNT];  // [source][destination]
} router_t;

// Everything to everything, never back to the port it came from, all channels and types, no remap. PORT_CI is
// the exception: it only exchanges SysEx with PORT_TRS (the box asks the device on its jacks, nobody else).
void router_init(router_t *rt);

// The MSG_* class of a message; 0 for messages that are never routed: utility (JR timestamps, NOOP) and UMP
// stream messages belong to the link they arrived on, not to the MIDI data.
uint16_t router_class(const ump_t *u);

// Where a message from src goes: fills dst[] with destination ports and out[] with the message as it should be
// sent there (remapped channel), and returns how many.
int router_route(const router_t *rt, int src, const ump_t *u, int dst[PORT_COUNT], ump_t out[PORT_COUNT]);

#ifdef __cplusplus
}
#endif
