#include "airmidi/router.h"

void router_init(router_t *rt) {
    for (int s = 0; s < PORT_COUNT; s++) {
        for (int d = 0; d < PORT_COUNT; d++) {
            route_t *r = &rt->r[s][d];
            r->enabled = s != d;
            r->channels = 0xFFFF;
            r->types = MSG_ALL;
            for (int c = 0; c < 16; c++) r->channel_map[c] = c;
        }
    }
}

uint16_t router_class(const ump_t *u) {
    uint8_t st = ump_status(u);
    switch (ump_mt(u)) {
        case UMP_MT_SYSTEM:
            return st == 0xF8 || (st >= 0xFA && st <= 0xFC) ? MSG_CLOCK : MSG_SYSTEM;
        case UMP_MT_M1_CV:
            switch (st & 0xF0) {
                case 0x80:
                case 0x90:
                    return MSG_NOTE;
                case 0xA0:
                    return MSG_POLY_PRESSURE;
                case 0xB0:
                    return MSG_CONTROL;
                case 0xC0:
                    return MSG_PROGRAM;
                case 0xD0:
                    return MSG_CHANNEL_PRESSURE;
                case 0xE0:
                    return MSG_PITCH_BEND;
            }
            return 0;
        case UMP_MT_M2_CV:
            switch (st >> 4) {
                case 0x8:
                case 0x9:
                    return MSG_NOTE;
                case 0xA:
                    return MSG_POLY_PRESSURE;
                case 0xB:
                case 0x2:  // RPN
                case 0x3:  // NRPN
                case 0x4:  // relative RPN
                case 0x5:  // relative NRPN
                    return MSG_CONTROL;
                case 0xC:
                    return MSG_PROGRAM;
                case 0xD:
                    return MSG_CHANNEL_PRESSURE;
                case 0xE:
                    return MSG_PITCH_BEND;
                default:  // 0x0, 0x1 per-note controllers, 0x6 per-note pitch bend, 0xF per-note management
                    return MSG_PER_NOTE;
            }
        case UMP_MT_SYSEX7:
        case UMP_MT_DATA128:
            return MSG_SYSEX;
        case UMP_MT_FLEX:
            return MSG_OTHER;
        default:  // utility, stream, reserved
            return 0;
    }
}

static bool is_channel_voice(const ump_t *u) { return ump_mt(u) == UMP_MT_M1_CV || ump_mt(u) == UMP_MT_M2_CV; }

int router_route(const router_t *rt, int src, const ump_t *u, int dst[PORT_COUNT], ump_t out[PORT_COUNT]) {
    if (src < 0 || src >= PORT_COUNT) return 0;
    uint16_t cls = router_class(u);
    if (!cls) return 0;
    bool cv = is_channel_voice(u);
    uint8_t ch = ump_status(u) & 0x0F;
    int n = 0;
    for (int d = 0; d < PORT_COUNT; d++) {
        const route_t *r = &rt->r[src][d];
        if (!r->enabled || !(r->types & cls)) continue;
        if (cv && !(r->channels & (1u << ch))) continue;
        out[n] = *u;
        if (cv) out[n].w[0] = (out[n].w[0] & ~0x000F0000u) | (uint32_t)(r->channel_map[ch] & 0xF) << 16;
        dst[n++] = d;
    }
    return n;
}
