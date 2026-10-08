// Breaks MIDI-CI Discovery loops on one MIDI 1.0 output. macOS passes a Discovery that arrives on one of its MIDI
// ports on to its other MIDI-CI endpoints (and only Discovery, not other SysEx). With the box's TRS cabled back to
// the same Mac, every Discovery then went round Mac -> network -> box -> TRS -> Mac until the 31250-baud line was
// full (docs/design/midi-ci.md, "Loops"). An initiator sends Discovery once, seconds apart; a copy that went round
// the loop is back within ~100 ms. So a Discovery from a guarded source less than CI_GUARD_MS after the last
// Discovery written to the output is dropped, whole. Unguarded sources (the box's own initiator) always pass, and
// their Discovery counts as the last one.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "airmidi/ump.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CI_GUARD_MS 1000

typedef struct {
    bool seen;          // a Discovery has gone out
    uint32_t last_ms;   // when
    uint32_t dropping;  // bit per source (0-31): inside a dropped Discovery, drop up to its end
    uint32_t dropped;   // Discoveries dropped
} ci_guard_t;

void ci_guard_init(ci_guard_t *g);

// Whether u from src may go on to the output.
bool ci_guard_pass(ci_guard_t *g, int src, bool guarded, const ump_t *u, uint32_t now_ms);

#ifdef __cplusplus
}
#endif
