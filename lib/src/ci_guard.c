#include "airmidi/ci_guard.h"

#include <string.h>

void ci_guard_init(ci_guard_t *g) { memset(g, 0, sizeof *g); }

// The first packet of a Discovery: 7E <device id> 0D 70 (Universal SysEx, MIDI-CI, Discovery).
static bool starts_discovery(const ump_t *u) {
    uint8_t d[6];
    return ump_sysex7_data(u, d) >= 4 && d[0] == 0x7E && d[2] == 0x0D && d[3] == 0x70;
}

bool ci_guard_pass(ci_guard_t *g, int src, bool guarded, const ump_t *u, uint32_t now_ms) {
    if (ump_mt(u) != UMP_MT_SYSEX7) return true;
    uint32_t bit = 1u << src;
    uint8_t st = ump_sysex7_status(u);
    if (st == UMP_SYSEX_CONTINUE || st == UMP_SYSEX_END) return !(g->dropping & bit);
    g->dropping &= ~bit;  // a new message from this source: the dropped one is over
    if (!starts_discovery(u)) return true;
    if (guarded && g->seen && now_ms - g->last_ms < CI_GUARD_MS) {
        if (st == UMP_SYSEX_START) g->dropping |= bit;
        g->dropped++;
        return false;
    }
    g->seen = true;
    g->last_ms = now_ms;
    return true;
}
