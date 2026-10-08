// Merging several sources onto one MIDI output. UMP packets are self-contained, but on a MIDI 1.0 wire (TRS, BLE
// MIDI) a SysEx is one long message: anything else written in the middle of it, except real-time, ends it. So, per
// output:
//   - real-time messages (clock, start/stop) go out at once, even in the middle of another source's SysEx;
//   - while a SysEx from one source is in progress, other messages wait in a queue (in order) until it ends;
//   - if it does not end within the timeout, it is closed (an empty END, which the encoder turns into F7), the
//     rest of that SysEx is dropped when it comes, and the queue moves on.
// Bounded queues; overflow is dropped and counted, never blocks the caller.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "airmidi/ump.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MERGE_QUEUE 64

typedef struct {
    ump_t u;
    uint8_t src;
} merge_item_t;

typedef struct {
    merge_item_t ready[MERGE_QUEUE];  // may be written to the output now, in this order
    int n_ready;
    merge_item_t held[MERGE_QUEUE];  // waiting for the SysEx in progress to end
    int n_held;
    int active;            // source whose SysEx is in progress, -1 = none
    uint8_t active_group;  // its UMP group, for the END the timeout writes
    uint32_t active_since;
    uint32_t aborted;  // bit per source: its SysEx was cut by the timeout, drop the rest of it
    uint16_t timeout_ms;
    uint32_t dropped;  // queue overflow + rest of timed-out SysEx
    uint32_t timeouts;
} merge_t;

void merge_init(merge_t *m, uint16_t timeout_ms);

// Queue one message from source src (0-31).
void merge_push(merge_t *m, int src, const ump_t *u, uint32_t now_ms);

// The next message to write to the output, if any. Applies the SysEx timeout as of now_ms.
bool merge_pop(merge_t *m, ump_t *out, uint32_t now_ms);

#ifdef __cplusplus
}
#endif
