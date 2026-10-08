#include "airmidi/midi1.h"
#include "airmidi/translate.h"

void midi1_out_init(midi1_out_t *o, bool use_running_status) {
    o->use_running_status = use_running_status;
    o->running = 0;
    o->in_sysex = false;
}

// A SysEx START went out but its END never came (lost upstream): close it before anything else is written, so the
// receiver does not swallow the next message as SysEx data.
static size_t close_sysex(midi1_out_t *o, uint8_t *buf) {
    if (!o->in_sysex) return 0;
    o->in_sysex = false;
    buf[0] = 0xF7;
    return 1;
}

static size_t channel_message(midi1_out_t *o, const ump_t *u, uint8_t *buf) {
    size_t n = close_sysex(o, buf);
    uint8_t st = ump_status(u);
    if (st < 0x80 || st >= 0xF0) return n;  // not a channel status: malformed MT 0x2
    if (!o->use_running_status || st != o->running) buf[n++] = st;
    o->running = st;
    buf[n++] = ump_byte3(u) & 0x7F;
    if (midi1_data_len(st) == 2) buf[n++] = ump_byte4(u) & 0x7F;
    return n;
}

static size_t system_message(midi1_out_t *o, const ump_t *u, uint8_t *buf) {
    uint8_t st = ump_status(u);
    int len = midi1_data_len(st);
    if (st < 0xF1 || st == 0xF7 || len < 0) return 0;  // F0/F7 travel as MT 0x3; undefined statuses dropped
    if (st >= 0xF8) {                                  // real-time: no effect on running status or SysEx
        buf[0] = st;
        return 1;
    }
    size_t n = close_sysex(o, buf);
    o->running = 0;  // system common cancels running status
    buf[n++] = st;
    if (len > 0) buf[n++] = ump_byte3(u) & 0x7F;
    if (len > 1) buf[n++] = ump_byte4(u) & 0x7F;
    return n;
}

static size_t sysex_packet(midi1_out_t *o, const ump_t *u, uint8_t *buf) {
    uint8_t data[6], st = ump_sysex7_status(u);
    int len = ump_sysex7_data(u, data);
    size_t n = 0;
    if (st == UMP_SYSEX_COMPLETE || st == UMP_SYSEX_START) {
        n += close_sysex(o, buf);
        buf[n++] = 0xF0;
        o->running = 0;
        o->in_sysex = true;
    } else if (!o->in_sysex) {
        return 0;  // CONTINUE or END without a START: the beginning was lost, drop the rest
    }
    for (int i = 0; i < len; i++) buf[n++] = data[i];
    if (st == UMP_SYSEX_COMPLETE || st == UMP_SYSEX_END) {
        buf[n++] = 0xF7;
        o->in_sysex = false;
    }
    return n;
}

size_t midi1_out_encode(midi1_out_t *o, const ump_t *u, uint8_t buf[MIDI1_OUT_MAX]) {
    switch (ump_mt(u)) {
        case UMP_MT_SYSTEM:
            return system_message(o, u, buf);
        case UMP_MT_M1_CV:
            return channel_message(o, u, buf);
        case UMP_MT_SYSEX7:
            return sysex_packet(o, u, buf);
        case UMP_MT_M2_CV: {
            ump_t m1[4];
            int count = m2_to_m1(u, m1);
            size_t n = 0;
            for (int i = 0; i < count; i++) n += channel_message(o, &m1[i], buf + n);
            return n;
        }
        default:
            return 0;
    }
}
