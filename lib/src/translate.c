#include "airmidi/translate.h"

#include <string.h>

uint32_t scale_up(uint32_t value, int src_bits, int dst_bits) {
    // M2-104 D.1.3, the reference algorithm
    int scale_bits = dst_bits - src_bits;
    uint32_t center = 1u << (src_bits - 1);
    uint32_t shifted = value << scale_bits;
    if (value <= center) return shifted;
    int repeat_bits = src_bits - 1;  // repeat everything below the top bit
    uint32_t repeat = value & ((1u << repeat_bits) - 1);
    if (scale_bits > repeat_bits)
        repeat <<= scale_bits - repeat_bits;
    else
        repeat >>= repeat_bits - scale_bits;
    while (repeat) {
        shifted |= repeat;
        repeat >>= repeat_bits;
    }
    return shifted;
}

// ---- MIDI 2.0 -> MIDI 1.0 -------------------------------------------------------------------------------------

int m2_to_m1(const ump_t *in, ump_t out[4]) {
    if (ump_mt(in) != UMP_MT_M2_CV) {
        out[0] = *in;
        return 1;
    }
    uint8_t g = ump_group(in), op = (in->w[0] >> 20) & 0xF, ch = (in->w[0] >> 16) & 0xF;
    uint8_t i1 = (in->w[0] >> 8) & 0x7F, i2 = in->w[0] & 0x7F;
    uint32_t data = in->w[1];
    uint8_t cc = 0xB0 | ch;
    switch (op) {
        case 0x8:  // Note Off: 16-bit velocity in the top half of the data word
            out[0] = ump_m1_cv(g, 0x80 | ch, i1, scale_down(data >> 16, 16, 7));
            return 1;
        case 0x9: {  // Note On: velocity 0 would mean Note Off in MIDI 1.0, so it becomes 1 (D.2.1)
            uint8_t v = scale_down(data >> 16, 16, 7);
            out[0] = ump_m1_cv(g, 0x90 | ch, i1, v ? v : 1);
            return 1;
        }
        case 0xA:
            out[0] = ump_m1_cv(g, 0xA0 | ch, i1, scale_down(data, 32, 7));
            return 1;
        case 0xB:
            out[0] = ump_m1_cv(g, cc, i1, scale_down(data, 32, 7));
            return 1;
        case 0xC: {  // Program Change; the Bank Valid option flag adds Bank Select MSB + LSB in front (D.2.4)
            int n = 0;
            if (in->w[0] & 0x01) {
                out[n++] = ump_m1_cv(g, cc, 0, (data >> 8) & 0x7F);
                out[n++] = ump_m1_cv(g, cc, 32, data & 0x7F);
            }
            out[n++] = ump_m1_cv(g, 0xC0 | ch, (data >> 24) & 0x7F, 0);
            return n;
        }
        case 0xD:
            out[0] = ump_m1_cv(g, 0xD0 | ch, scale_down(data, 32, 7), 0);
            return 1;
        case 0xE: {  // Pitch Bend: 14 bits, LSB first
            uint32_t v = scale_down(data, 32, 14);
            out[0] = ump_m1_cv(g, 0xE0 | ch, v & 0x7F, v >> 7);
            return 1;
        }
        case 0x2:    // Registered Controller (RPN)
        case 0x3: {  // Assignable Controller (NRPN): parameter number, then Data Entry MSB + LSB (D.2.3)
            uint32_t v = scale_down(data, 32, 14);
            out[0] = ump_m1_cv(g, cc, op == 0x2 ? 101 : 99, i1);
            out[1] = ump_m1_cv(g, cc, op == 0x2 ? 100 : 98, i2);
            out[2] = ump_m1_cv(g, cc, 6, v >> 7);
            out[3] = ump_m1_cv(g, cc, 38, v & 0x7F);
            return 4;
        }
        default:  // per-note controllers / pitch bend / management, relative RPN/NRPN (D.2.8)
            return 0;
    }
}

// ---- MIDI 1.0 -> MIDI 2.0 -------------------------------------------------------------------------------------

void m1_to_m2_init(m1_to_m2_t *t) { memset(t, 0, sizeof *t); }

// The RPN/NRPN for the channel's current parameter with a 14-bit value, unless it is the null function.
static int param_message(m1_to_m2_channel_t *c, uint8_t g, uint8_t ch, uint32_t v14, ump_t *out) {
    const uint8_t *p = c->kind == 0x2 ? c->rpn : c->nrpn;
    if (!c->kind || (p[0] == 0x7F && p[1] == 0x7F)) return 0;
    *out = ump_m2_cv(g, c->kind, ch, p[0], p[1], scale_up(v14, 14, 32));
    return 1;
}

static int flush_entry(m1_to_m2_channel_t *c, uint8_t g, uint8_t ch, ump_t *out) {
    if (!c->entry_pending) return 0;
    c->entry_pending = false;
    return param_message(c, g, ch, (uint32_t)c->entry_msb << 7, out);
}

static int control_change(m1_to_m2_channel_t *c, uint8_t g, uint8_t ch, uint8_t num, uint8_t v, ump_t out[2]) {
    int n = 0;
    switch (num) {
        case 0:  // Bank Select is only sent as part of a Program Change (D.3.3)
            c->bank_msb = v, c->bank_valid = true;
            return 0;
        case 32:
            c->bank_lsb = v;
            return 0;
        case 101:
        case 100:
        case 99:
        case 98: {  // a new parameter number ends the previous Data Entry
            n += flush_entry(c, g, ch, &out[n]);
            bool rpn = num >= 100;
            uint8_t *p = rpn ? c->rpn : c->nrpn;
            p[(num & 1) ? 0 : 1] = v;  // 101/99 are the MSBs
            c->kind = rpn ? 0x2 : 0x3;
            c->entry_valid = false;
            return n;
        }
        case 6:                      // Data Entry MSB: held until CC 38, another CC 6, or a new parameter number
            if (!c->kind) return 0;  // Data Entry with no parameter selected does not translate
            n += flush_entry(c, g, ch, &out[n]);
            c->entry_msb = v, c->entry_valid = c->entry_pending = true;
            return n;
        case 38:  // Data Entry LSB completes the message
            if (!c->kind || !c->entry_valid) return 0;
            c->entry_pending = false;
            return param_message(c, g, ch, (uint32_t)c->entry_msb << 7 | v, &out[0]);
        default:
            out[0] = ump_m2_cv(g, 0xB, ch, num, 0, scale_up(v, 7, 32));
            return 1;
    }
}

int m1_to_m2(m1_to_m2_t *t, const ump_t *in, ump_t out[2]) {
    if (ump_mt(in) != UMP_MT_M1_CV) {
        out[0] = *in;
        return 1;
    }
    uint8_t g = ump_group(in), st = ump_status(in), ch = st & 0xF;
    uint8_t d1 = ump_byte3(in) & 0x7F, d2 = ump_byte4(in) & 0x7F;
    m1_to_m2_channel_t *c = &t->ch[g][ch];
    switch (st & 0xF0) {
        case 0x80:
            out[0] = ump_m2_cv(g, 0x8, ch, d1, 0, scale_up(d2, 7, 16) << 16);
            return 1;
        case 0x90:  // Note On with velocity 0 is a Note Off with velocity 0x8000 (D.3.1)
            out[0] = d2 ? ump_m2_cv(g, 0x9, ch, d1, 0, scale_up(d2, 7, 16) << 16)
                        : ump_m2_cv(g, 0x8, ch, d1, 0, 0x8000u << 16);
            return 1;
        case 0xA0:
            out[0] = ump_m2_cv(g, 0xA, ch, d1, 0, scale_up(d2, 7, 32));
            return 1;
        case 0xB0:
            return control_change(c, g, ch, d1, d2, out);
        case 0xC0: {  // Program Change carries the current bank, if one was selected (D.3.4)
            uint32_t bank = c->bank_valid ? (uint32_t)c->bank_msb << 8 | c->bank_lsb : 0;
            out[0] = ump_m2_cv(g, 0xC, ch, 0, c->bank_valid ? 1 : 0, (uint32_t)d1 << 24 | bank);
            return 1;
        }
        case 0xD0:
            out[0] = ump_m2_cv(g, 0xD, ch, 0, 0, scale_up(d1, 7, 32));
            return 1;
        case 0xE0:
            out[0] = ump_m2_cv(g, 0xE, ch, 0, 0, scale_up((uint32_t)d2 << 7 | d1, 14, 32));
            return 1;
    }
    return 0;
}

int m1_to_m2_flush(m1_to_m2_t *t, uint8_t group, uint8_t channel, ump_t out[1]) {
    return flush_entry(&t->ch[group & 0xF][channel & 0xF], group & 0xF, channel & 0xF, out);
}
