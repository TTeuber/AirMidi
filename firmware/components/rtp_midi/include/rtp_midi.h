// RTP-MIDI (AppleMIDI) on UDP 5004/5005: up to two sessions (PORT_RTP_0/1), announced over mDNS as
// _apple-midi._udp so macOS's MIDI Network window and rtpMIDI on Windows list the box. The protocol is
// lib/rtpmidi (sessions, recovery journal); this file owns the sockets, the task and the bridge to the bus.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "airmidi/rtpmidi.h"

#define RTP_MIDI_PORT 5004  // control; data is 5005

// Start (once). `name` is the session name clients see.
void rtp_midi_start(const char *name);

typedef struct {
    bool active;
    char peer[64];
    char ip[16];
    bool resumed;  // taken back after a restart (lib/rtpmidi.h)
    bool syncing;  // taken back, MIDI to the client held until it says where our sequence numbers stood
    rtpmidi_stats_t stats;
} rtp_midi_session_info_t;

void rtp_midi_get_info(int session, rtp_midi_session_info_t *out);
