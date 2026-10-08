// BLE MIDI 1.0 packets (Apple / MMA "MIDI over Bluetooth Low Energy"), both directions.
//
// A packet is a header byte (bit 7 set, bits 5-0 = timestamp bits 12-7) followed by MIDI messages, each status
// byte preceded by a timestamp byte (bit 7 set, bits 6-0 = timestamp bits 6-0). SysEx data may continue into the
// next packet (header, then data with no timestamp) and ends with a timestamp + F7. Timestamps are milliseconds,
// 13 bits, wrapping every 8.192 s.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "airmidi/midi1.h"
#include "airmidi/ump.h"

#ifdef __cplusplus
extern "C" {
#endif

// Largest payload we build: an ATT MTU of 247 (the BLE 4.2+ data length) minus the 3-byte ATT header.
#define BLEMIDI_MAX_PACKET 244
// What every central can take: the default ATT MTU of 23 minus 3.
#define BLEMIDI_MIN_PACKET 20

// ---- receive -------------------------------------------------------------------------------------------------

// Strip the header and timestamps from one packet, leaving the plain MIDI 1.0 bytes for a midi1_in parser (one
// parser per connection: running status and SysEx continue across packets). A byte with bit 7 set is a timestamp
// unless the byte before it was a timestamp, in which case it is a status byte. Returns the number of bytes
// written to out (at most len - 1), or -1 if pkt is not a BLE MIDI packet (empty, or a bad header byte).
// Timestamps are not used for scheduling: messages go out as soon as they arrive (see docs/design/transports.md).
int blemidi_decode(const uint8_t *pkt, size_t len, uint8_t *out);

// ---- send ----------------------------------------------------------------------------------------------------

typedef void (*blemidi_emit_fn)(void *ctx, const uint8_t *pkt, size_t len);

typedef struct {
    uint8_t pkt[BLEMIDI_MAX_PACKET];
    size_t len;      // bytes in pkt (0 = no packet started)
    size_t max;      // payload limit for this connection (ATT MTU - 3)
    uint8_t pkt_hi;  // timestamp bits 12-7 in this packet's header
    midi1_out_t m1;  // UMP -> MIDI 1.0 bytes, no running status (every message carries its status)
    blemidi_emit_fn emit;
    void *ctx;
} blemidi_tx_t;

void blemidi_tx_init(blemidi_tx_t *t, size_t max_payload, blemidi_emit_fn emit, void *ctx);

// After the MTU exchange. Flushes the packet in progress first if it would no longer fit.
void blemidi_tx_set_max(blemidi_tx_t *t, size_t max_payload);

// Append one UMP, timestamped now_ms. Emits finished packets through the callback when the next message does not
// fit; a message is never split across packets, only SysEx data is.
void blemidi_tx_add(blemidi_tx_t *t, const ump_t *u, uint32_t now_ms);

// Emit the packet in progress, if any. Call after a batch (e.g. once per router wake-up), so messages wait at most
// for the batch, not for the packet to fill.
void blemidi_tx_flush(blemidi_tx_t *t);

#ifdef __cplusplus
}
#endif
