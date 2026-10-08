// MIDI 1.0 byte streams <-> UMP, for the byte-oriented transports (TRS at 31250 baud, BLE MIDI payloads).
//
// midi1_in: a byte-at-a-time parser that turns a MIDI 1.0 stream into UMP (MT 0x1 system, MT 0x2 channel voice,
// MT 0x3 SysEx7). It follows the MIDI 1.0 rules a real cable throws at you: running status, real-time bytes in the
// middle of any message (including SysEx), system common cancelling running status, a status byte cutting a SysEx
// short, and stray data bytes (dropped and counted).
//
// midi1_out: turns UMP back into bytes for one output, applying running status when asked to and translating
// MIDI 2.0 channel voice (MT 0x4) down with the M2-104 default translation.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "airmidi/ump.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t group;    // UMP group stamped on everything this source produces
    uint8_t status;   // status of the message being collected (or the running status), 0 = none
    uint8_t data[2];  // data bytes collected so far
    uint8_t have;     // how many of them
    bool running;     // status may be reused for the next message (channel voice only)
    bool in_sysex;
    bool sysex_started;  // a START packet has gone out for the SysEx in progress
    uint8_t sx[6];       // SysEx bytes not sent yet (a packet goes out once a 7th byte proves it is not the last)
    uint8_t sx_n;
    uint32_t dropped;  // data bytes with no status to belong to, undefined statuses
} midi1_in_t;

void midi1_in_init(midi1_in_t *p, uint8_t group);

// Feed one byte. Writes 0, 1 or 2 UMPs to out and returns how many. (2 when a status byte ends a SysEx and is a
// complete message itself, e.g. F6 Tune Request.)
int midi1_in_feed(midi1_in_t *p, uint8_t byte, ump_t out[2]);

typedef struct {
    bool use_running_status;  // omit repeated channel status bytes (saves a third of the bandwidth on TRS)
    uint8_t running;          // last channel status written, 0 = none
    bool in_sysex;            // a SysEx START went out without its END yet
} midi1_out_t;

#define MIDI1_OUT_MAX 13  // bytes one UMP can turn into: F7 to close a broken SysEx + an RPN as 4 full CCs

void midi1_out_init(midi1_out_t *o, bool use_running_status);

// Encode one UMP. Returns the number of bytes written to buf (0 for UMPs with no MIDI 1.0 meaning: utility,
// stream, flex data, SysEx8, and MIDI 2.0 messages without a MIDI 1.0 equivalent such as per-note controllers).
// Real-time messages never touch running status or SysEx state, so they can be sent in the middle of a SysEx.
size_t midi1_out_encode(midi1_out_t *o, const ump_t *u, uint8_t buf[MIDI1_OUT_MAX]);

// Forget running status, e.g. after the output has been idle (receivers that power up mid-stream need a status
// byte) or after another writer used the same wire.
static inline void midi1_out_reset_running(midi1_out_t *o) { o->running = 0; }

#ifdef __cplusplus
}
#endif
