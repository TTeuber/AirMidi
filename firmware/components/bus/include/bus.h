// The MIDI bus: every transport hands its incoming messages (as UMP) to bus_send(), one task routes them through
// the route matrix (lib/router) to the outputs that are up, merging per MIDI 1.0 output (lib/merge), and calls
// each output's write() from that one task. Transports never call each other.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "airmidi/router.h"
#include "airmidi/ump.h"

typedef struct {
    // Write one message to the transport. Called only from the bus task. Must not block: if the transport has no
    // room, drop and count.
    void (*write)(void *ctx, const ump_t *u);
    // End of a batch: e.g. send the BLE packet being filled. May be NULL.
    void (*flush)(void *ctx);
    // Bytes the transport can take right now without dropping. The bus stops draining this output's merge while
    // it is under MIDI1_OUT_MAX, so a burst waits in the merge queue (bounded, counted) instead of being dropped
    // at the transport. May be NULL (always room).
    size_t (*room)(void *ctx);
    // The port has just come up (called from the bus task, before any write): reset per-connection send state.
    // May be NULL.
    void (*on_up)(void *ctx);
    // A MIDI 1.0 byte wire (TRS, BLE MIDI): needs SysEx-aware merging of its sources.
    bool byte_stream;
} bus_output_ops_t;

typedef struct {
    uint32_t in, out, dropped;  // messages received from / written to the port, dropped on the way out
} bus_port_stats_t;

// Start the bus task. The route matrix starts as router_init()'s default (everything to everything else).
void bus_start(uint16_t sysex_timeout_ms);

// Register a port's output (once, after bus_start, before it comes up). Byte-stream outputs get their merge queue here.
void bus_register_output(int port, const bus_output_ops_t *ops, void *ctx);

// Mark an output usable or not (BLE: subscribed; TRS: always). Down outputs are skipped by routing.
void bus_set_up(int port, bool up);
bool bus_is_up(int port);

// Queue a message from a source port. Safe from any task, never blocks; returns false (and counts) if the queue
// is full.
bool bus_send(int src, const ump_t *u);

// Queue a message for every output that is up, bypassing the route matrix (panic, notes off before sleep).
bool bus_broadcast(const ump_t *u);

// Time (ms since boot) of the last message routed, for the LED's activity flicker.
uint32_t bus_last_activity_ms(void);

void bus_get_stats(int port, bus_port_stats_t *out);
uint32_t bus_queue_dropped(void);
