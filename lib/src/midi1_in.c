#include <string.h>

#include "airmidi/midi1.h"

void midi1_in_init(midi1_in_t *p, uint8_t group) {
    memset(p, 0, sizeof *p);
    p->group = group & 0xF;
}

// Close the SysEx in progress with the bytes still held: COMPLETE if nothing went out yet, END otherwise.
static int end_sysex(midi1_in_t *p, ump_t *out) {
    uint8_t st = p->sysex_started ? UMP_SYSEX_END : UMP_SYSEX_COMPLETE;
    *out = ump_sysex7(p->group, st, p->sx, p->sx_n);
    p->in_sysex = p->sysex_started = false;
    p->sx_n = 0;
    return 1;
}

static int sysex_byte(midi1_in_t *p, uint8_t b, ump_t *out) {
    int n = 0;
    if (p->sx_n == 6) {  // a 7th byte: the held 6 are not the end, send them
        *out = ump_sysex7(p->group, p->sysex_started ? UMP_SYSEX_CONTINUE : UMP_SYSEX_START, p->sx, 6);
        p->sysex_started = true;
        p->sx_n = 0;
        n = 1;
    }
    p->sx[p->sx_n++] = b;
    return n;
}

static ump_t complete_message(const midi1_in_t *p) {
    uint8_t d1 = p->have > 0 ? p->data[0] : 0, d2 = p->have > 1 ? p->data[1] : 0;
    return p->status >= 0xF0 ? ump_system(p->group, p->status, d1, d2) : ump_m1_cv(p->group, p->status, d1, d2);
}

int midi1_in_feed(midi1_in_t *p, uint8_t b, ump_t out[2]) {
    int n = 0;

    if (b >= 0xF8) {  // real-time: one byte, anywhere, changes nothing
        if (b == 0xF9 || b == 0xFD) {
            p->dropped++;  // undefined real-time
            return 0;
        }
        out[0] = ump_system(p->group, b, 0, 0);
        return 1;
    }

    if (b & 0x80) {  // any other status byte ends a SysEx in progress
        if (p->in_sysex) n += end_sysex(p, &out[n]);
        if (p->have) p->dropped++;  // a message cut short by this status byte
        p->have = 0;
        if (b == 0xF7) {  // the proper end of a SysEx, or a stray F7: either way it cancels running status
            p->status = 0, p->running = false;
            return n;
        }
        if (b == 0xF0) {
            p->in_sysex = true;
            p->status = 0, p->running = false;
            return n;
        }
        int len = midi1_data_len(b);
        if (len < 0) {  // F4, F5: undefined system common, cancels running status
            p->status = 0, p->running = false;
            p->dropped++;
            return n;
        }
        p->status = b;
        p->running = b < 0xF0;  // system common cancels running status
        if (len == 0) {         // F6 Tune Request
            out[n++] = complete_message(p);
            p->status = 0;
        }
        return n;
    }

    // data byte
    if (p->in_sysex) return sysex_byte(p, b, &out[0]);
    if (!p->status) {
        p->dropped++;
        return 0;
    }
    p->data[p->have++] = b;
    if (p->have == midi1_data_len(p->status)) {
        out[n++] = complete_message(p);
        p->have = 0;
        if (!p->running) p->status = 0;
    }
    return n;
}
