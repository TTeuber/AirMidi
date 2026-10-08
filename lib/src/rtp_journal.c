#include "airmidi/rtp_journal.h"

#include <string.h>

// Table of contents bits of a channel journal (RFC 6295 figure 9), in chapter order.
enum { TOC_P = 0x80, TOC_C = 0x40, TOC_M = 0x20, TOC_W = 0x10, TOC_N = 0x08, TOC_E = 0x04, TOC_T = 0x02, TOC_A = 0x01 };

#define CHAN_MAX 600  // largest channel journal we write: P 3 + C 257 + W 2 + N 274 + T 1, plus the header

static bool bit(const uint32_t *set, int n) { return set[n >> 5] >> (n & 31) & 1; }
static void set_bit(uint32_t *set, int n) { set[n >> 5] |= 1u << (n & 31); }
static void clear_bit(uint32_t *set, int n) { set[n >> 5] &= ~(1u << (n & 31)); }

// Controllers journalled with the count tool (they carry no value worth restoring, only "it happened"): All Sound
// Off, Reset All Controllers, All Notes Off and the mode messages that imply it.
static int counted(int num) {
    if (num == 120 || num == 121) return num - 120;
    if (num >= 123 && num <= 127) return num - 121;
    return -1;
}

// RPN/NRPN: they belong in chapter M, which we do not write (header comment).
static bool parameter_system(int num) { return num == 6 || num == 38 || (num >= 96 && num <= 101); }

static bool n_reset(int num) { return num == 120 || num >= 123; }  // ends notes: "N-active" (RFC 6295 A.1)

// ---- sending -------------------------------------------------------------------------------------------------

static void free_slot(rtpj_chan_t *c) {
    memset(c, 0, sizeof *c);
    c->ch = -1;
}

void rtpj_tx_init(rtpj_tx_t *t, uint16_t first_seq) {
    memset(t, 0, sizeof *t);
    for (int i = 0; i < RTPJ_SLOTS; i++) free_slot(&t->slot[i]);
    t->checkpoint = first_seq;
}

// Position of a packet in the checkpoint history: 0 for the checkpoint packet, growing towards the present.
static uint16_t age(const rtpj_tx_t *t, uint16_t seq) { return (uint16_t)(seq - t->checkpoint); }
static bool before_checkpoint(const rtpj_tx_t *t, uint16_t seq) { return (int16_t)(seq - t->checkpoint) < 0; }

static bool has_history(const rtpj_chan_t *c) {
    if (c->prog_in || c->bend_in || c->press_in) return true;
    for (int i = 0; i < 4; i++)
        if (c->ctl_in[i] || c->note_in[i]) return true;
    return false;
}

// Drop from the checkpoint history whatever went out before the checkpoint; slots left empty are free again.
static void sweep(rtpj_tx_t *t) {
    for (int i = 0; i < RTPJ_SLOTS; i++) {
        rtpj_chan_t *c = &t->slot[i];
        if (c->ch < 0) continue;
        if (c->prog_in && before_checkpoint(t, c->prog_seq)) c->prog_in = false;
        if (c->bend_in && before_checkpoint(t, c->bend_seq)) c->bend_in = false;
        if (c->press_in && before_checkpoint(t, c->press_seq)) c->press_in = false;
        for (int n = 0; n < 128; n++) {
            if (bit(c->ctl_in, n) && before_checkpoint(t, c->ctl_seq[n])) clear_bit(c->ctl_in, n);
            if (bit(c->note_in, n) && before_checkpoint(t, c->note_seq[n])) clear_bit(c->note_in, n);
        }
        if (!has_history(c)) free_slot(c);
    }
}

static void move_checkpoint(rtpj_tx_t *t, uint16_t to) {
    t->checkpoint = to;
    sweep(t);
}

static rtpj_chan_t *find_slot(rtpj_tx_t *t, int ch) {
    for (int i = 0; i < RTPJ_SLOTS; i++)
        if (t->slot[i].ch == ch) return &t->slot[i];
    return NULL;
}

// The slot for a channel's next command: its own, a free one, or (all taken) the one idle longest, whose
// unconfirmed history is given up (counted).
static rtpj_chan_t *slot_for(rtpj_tx_t *t, int ch) {
    rtpj_chan_t *c = find_slot(t, ch);
    if (c) return c;
    rtpj_chan_t *oldest = &t->slot[0];
    for (int i = 0; i < RTPJ_SLOTS; i++) {
        if (t->slot[i].ch < 0) {
            oldest = &t->slot[i];
            break;
        }
        if (age(t, t->slot[i].last_seq) < age(t, oldest->last_seq)) oldest = &t->slot[i];
    }
    if (oldest->ch >= 0) t->overflows++;
    free_slot(oldest);
    oldest->ch = (int8_t)ch;
    return oldest;
}

// Sort numbers (controllers or notes) oldest first by the packet they went out in: the "oldest-first ordering
// rule" of RFC 6295 A.1. Insertion sort: the lists are short and mostly sorted already.
static void sort_by_age(const rtpj_tx_t *t, const uint16_t *seq_of, uint8_t *list, int n) {
    for (int i = 1; i < n; i++) {
        uint8_t v = list[i];
        int j = i;
        while (j > 0 && age(t, seq_of[list[j - 1]]) > age(t, seq_of[v])) list[j] = list[j - 1], j--;
        list[j] = v;
    }
}

// Was the NoteOn sent recently enough that playing it late beats not playing it? (the Y bit)
static bool recent(const rtpj_tx_t *t, uint16_t seq, uint32_t now_ms) {
    int i = seq % RTPJ_TIMES;
    return t->sent_seq[i] == seq && now_ms - t->sent_ms[i] < RTPJ_PLAY_MS;
}

// One channel journal into out (CHAN_MAX bytes). Returns its length, 0 if the channel has nothing to say; *single
// is false if it describes a command of the previous packet (the S bits, RFC 6295 A.1).
static size_t build_channel(const rtpj_tx_t *t, const rtpj_chan_t *c, uint16_t seq, uint32_t now_ms, uint8_t *out,
                            bool *single) {
    int ch = c->ch;
    uint16_t prev = seq - 1;
    uint8_t *p = out + 3, toc = 0;
    bool s_all = true;

    if (c->prog_in) {
        bool s = c->prog_seq != prev;
        s_all &= s;
        *p++ = (uint8_t)(s << 7 | c->program);
        *p++ = (uint8_t)(c->bank << 7 | c->bank_msb);
        *p++ = c->bank_lsb;
        toc |= TOC_P;
    }

    uint8_t list[128];
    int n = 0;
    for (int i = 0; i < 128; i++)
        if (bit(c->ctl_in, i)) list[n++] = (uint8_t)i;
    if (n) {
        sort_by_age(t, c->ctl_seq, list, n);
        uint8_t *head = p++;
        bool s_c = true;
        for (int i = 0; i < n; i++) {
            int num = list[i], k = counted(num);
            bool s = c->ctl_seq[num] != prev;
            s_c &= s;
            *p++ = (uint8_t)(s << 7 | num);
            // Count tool (A = 1, T = 1) for the counted controllers, value tool (A = 0) for the rest.
            *p++ = k >= 0 ? (uint8_t)(0xC0 | (t->chan[ch].counts[k] & 0x3F)) : c->ctl_val[num];
        }
        *head = (uint8_t)(s_c << 7 | (n - 1));
        s_all &= s_c;
        toc |= TOC_C;
    }

    if (c->bend_in) {
        bool s = c->bend_seq != prev;
        s_all &= s;
        *p++ = (uint8_t)(s << 7 | c->bend_lsb);
        *p++ = c->bend_msb;
        toc |= TOC_W;
    }

    // Chapter N: a note log for every note whose latest command was a NoteOn, a bit for every one whose latest was a
    // NoteOff.
    n = 0;
    int low = 16, high = -1;
    for (int i = 0; i < 128; i++) {
        if (!bit(c->note_in, i)) continue;
        if (c->note_vel[i])
            list[n++] = (uint8_t)i;
        else {
            if (i / 8 < low) low = i / 8;
            high = i / 8;
        }
    }
    if (n || high >= 0) {
        sort_by_age(t, c->note_seq, list, n);
        bool b = !(c->any_off && c->off_seq == prev);
        if (high < 0) low = 15, high = n == 128 ? 0 : 1;  // (15, 1): no NoteOff bits; (15, 0) with LEN 127: 128 logs
        *p++ = (uint8_t)(b << 7 | (n == 128 ? 127 : n));
        *p++ = (uint8_t)(low << 4 | high);
        bool s_n = b;
        for (int i = 0; i < n; i++) {
            int note = list[i];
            bool s = c->note_seq[note] != prev;
            s_n &= s;
            *p++ = (uint8_t)(s << 7 | note);
            *p++ = (uint8_t)(recent(t, c->note_seq[note], now_ms) << 7 | c->note_vel[note]);
        }
        if (low <= high) {
            for (int o = low; o <= high; o++) {
                uint8_t bits = 0;
                for (int k = 0; k < 8; k++) {
                    int note = o * 8 + k;
                    if (bit(c->note_in, note) && !c->note_vel[note]) bits |= 0x80 >> k;
                }
                *p++ = bits;
            }
        }
        s_all &= s_n;
        toc |= TOC_N;
    }

    if (c->press_in) {
        bool s = c->press_seq != prev;
        s_all &= s;
        *p++ = (uint8_t)(s << 7 | c->pressure);
        toc |= TOC_T;
    }

    if (!toc) return 0;
    size_t len = (size_t)(p - out);
    out[0] = (uint8_t)(s_all << 7 | ch << 3 | (len >> 8 & 0x03));  // H = 0: no enhanced chapter C
    out[1] = (uint8_t)len;
    out[2] = toc;
    *single = s_all;
    return len;
}

size_t rtpj_tx_build(rtpj_tx_t *t, uint16_t seq, uint32_t now_ms, uint8_t *out, size_t cap) {
    // A receiver that never answers would keep the checkpoint where it is forever; 16-bit sequence numbers can only
    // be compared within half their range, so the history is cut at a quarter.
    if (age(t, seq) > 0x4000) move_checkpoint(t, seq - 0x4000);
    size_t pos = 3;
    int nch = 0;
    bool s_all = true;
    for (int ch = 0; ch < 16; ch++) {  // channel journals in ascending channel order (RFC 6295 5)
        const rtpj_chan_t *c = find_slot(t, ch);
        if (!c) continue;
        uint8_t buf[CHAN_MAX];
        bool single = true;
        size_t n = build_channel(t, c, seq, now_ms, buf, &single);
        if (!n) continue;
        if (pos + n > cap) {
            // Too much history for one packet: give up the part the receiver has not confirmed (it may miss a
            // repair) rather than send a packet over the MTU.
            move_checkpoint(t, seq);
            t->overflows++;
            pos = 3, nch = 0, s_all = true;
            break;
        }
        memcpy(out + pos, buf, n);
        pos += n, nch++;
        s_all &= single;
    }
    // Y = 0: no system journal; H = 0.
    out[0] = (uint8_t)(s_all << 7 | (nch ? 0x20 | (nch - 1) : 0));
    out[1] = (uint8_t)(t->checkpoint >> 8);
    out[2] = (uint8_t)t->checkpoint;
    return pos;
}

void rtpj_tx_record(rtpj_tx_t *t, uint16_t seq, uint32_t now_ms, uint8_t status, uint8_t d1, uint8_t d2) {
    t->sent_seq[seq % RTPJ_TIMES] = seq;
    t->sent_ms[seq % RTPJ_TIMES] = now_ms;
    if (status == 0xFF) {  // System Reset: nothing before it is active any more, the counts start again
        for (int i = 0; i < RTPJ_SLOTS; i++) free_slot(&t->slot[i]);
        memset(t->chan, 0, sizeof t->chan);
        return;
    }
    if (status < 0x80 || status >= 0xF0) return;
    int ch = status & 0x0F, op = status & 0xF0;
    rtpj_session_chan_t *sc = &t->chan[ch];
    d1 &= 0x7F, d2 &= 0x7F;
    if (op == 0xA0 || (op == 0xB0 && parameter_system(d1))) return;  // chapters A and M are not written
    rtpj_chan_t *c = slot_for(t, ch);
    c->last_seq = seq;
    switch (op) {
        case 0x80:
        case 0x90:
            c->note_vel[d1] = op == 0x90 ? d2 : 0;  // NoteOn with velocity 0 is a NoteOff
            c->note_seq[d1] = seq;
            set_bit(c->note_in, d1);
            if (!c->note_vel[d1]) c->off_seq = seq, c->any_off = true;
            break;
        case 0xB0: {
            int k = counted(d1);
            if (k >= 0) sc->counts[k] = (sc->counts[k] + 1) & 0x3F;
            c->ctl_val[d1] = d2;
            c->ctl_seq[d1] = seq;
            set_bit(c->ctl_in, d1);
            if (d1 == 0) sc->cur_msb = d2, sc->have_msb = true, sc->have_lsb = false;
            if (d1 == 32) sc->cur_lsb = d2, sc->have_lsb = true;
            if (n_reset(d1)) memset(c->note_in, 0, sizeof c->note_in), c->press_in = false;
            if (d1 == 121) c->bend_in = c->press_in = false;  // Reset All Controllers: no longer "C-active"
            break;
        }
        case 0xC0:
            c->program = d1;
            c->bank = sc->have_msb;
            c->bank_msb = sc->have_msb ? sc->cur_msb : 0;
            c->bank_lsb = sc->have_msb && sc->have_lsb ? sc->cur_lsb : 0;
            c->prog_seq = seq, c->prog_in = true;
            break;
        case 0xD0:
            c->pressure = d1;
            c->press_seq = seq, c->press_in = true;
            break;
        default:  // 0xE0
            c->bend_lsb = d1, c->bend_msb = d2;
            c->bend_seq = seq, c->bend_in = true;
            break;
    }
}

void rtpj_tx_ack(rtpj_tx_t *t, uint16_t seq, uint16_t next_seq) {
    uint16_t to = seq + 1;
    // Only forward, and never past the next packet (a stale or bogus report must not empty the journal).
    if ((int16_t)(to - t->checkpoint) <= 0 || (int16_t)(next_seq - to) < 0) return;
    move_checkpoint(t, to);
}

bool rtpj_tx_pending(const rtpj_tx_t *t) {
    for (int i = 0; i < RTPJ_SLOTS; i++)
        if (t->slot[i].ch >= 0 && has_history(&t->slot[i])) return true;
    return false;
}

// ---- receiving -----------------------------------------------------------------------------------------------

static void rx_chan_init(rtpj_rx_chan_t *c) {
    memset(c, 0, sizeof *c);
    memset(c->ctl, RTPJ_UNKNOWN, sizeof c->ctl);
    c->program = c->pressure = RTPJ_UNKNOWN;
    c->bend = 0xFFFF;
}

void rtpj_rx_init(rtpj_rx_t *r) {
    for (int ch = 0; ch < 16; ch++) rx_chan_init(&r->ch[ch]);
    r->repairs = 0;
}

void rtpj_rx_track(rtpj_rx_t *r, uint8_t status, uint8_t d1, uint8_t d2) {
    if (status == 0xFF) {
        for (int ch = 0; ch < 16; ch++) rx_chan_init(&r->ch[ch]);
        return;
    }
    if (status < 0x80 || status >= 0xF0) return;
    rtpj_rx_chan_t *c = &r->ch[status & 0x0F];
    d1 &= 0x7F, d2 &= 0x7F;
    switch (status & 0xF0) {
        case 0x80:
            clear_bit(c->notes, d1);
            break;
        case 0x90:
            if (d2)
                set_bit(c->notes, d1);
            else
                clear_bit(c->notes, d1);
            break;
        case 0xB0: {
            c->ctl[d1] = d2;
            int k = counted(d1);
            if (k >= 0) c->counts[k] = (c->counts[k] + 1) & 0x3F;
            if (n_reset(d1)) memset(c->notes, 0, sizeof c->notes);
            if (d1 == 121) c->bend = 0x2000, c->pressure = 0;  // RP-015: centre, no pressure
            break;
        }
        case 0xC0:
            c->program = d1;
            break;
        case 0xD0:
            c->pressure = d1;
            break;
        case 0xE0:
            c->bend = (uint16_t)(d2 << 7 | d1);
            break;
        default:
            break;
    }
}

typedef struct {
    rtpj_rx_t *r;
    rtpj_emit_fn emit;
    void *ctx;
} repair_t;

static void play(repair_t *rp, uint8_t status, uint8_t d1, uint8_t d2) {
    rp->emit(rp->ctx, status, d1, d2);
    rtpj_rx_track(rp->r, status, d1, d2);
    rp->r->repairs++;
}

// The chapters of one channel journal, p[0..len) after its 3-byte header, in table-of-contents order.
static bool recover_channel(repair_t *rp, int ch, uint8_t toc, const uint8_t *p, size_t len) {
    rtpj_rx_chan_t *c = &rp->r->ch[ch];
    const uint8_t *end = p + len;
    if (toc & TOC_P) {
        if (end - p < 3) return false;
        uint8_t prog = p[0] & 0x7F, msb = p[1] & 0x7F, lsb = p[2] & 0x7F;
        bool bank = p[1] & 0x80;
        if (c->program != prog || (bank && (c->ctl[0] != msb || c->ctl[32] != lsb))) {
            if (bank) play(rp, 0xB0 | ch, 0, msb), play(rp, 0xB0 | ch, 32, lsb);
            play(rp, 0xC0 | ch, prog, 0);
        }
        p += 3;
    }
    if (toc & TOC_C) {
        if (end - p < 1) return false;
        int n = (p[0] & 0x7F) + 1;
        if (end - p < 1 + 2 * n) return false;
        for (int i = 0; i < n; i++) {
            uint8_t num = p[1 + 2 * i] & 0x7F, v = p[2 + 2 * i];
            if (!(v & 0x80)) {  // value tool
                if (c->ctl[num] != v) play(rp, 0xB0 | ch, num, v);
            } else if (v & 0x40) {  // count tool: the command happened more often than we saw it
                int k = counted(num);
                if (k >= 0 && c->counts[k] != (v & 0x3F)) {
                    play(rp, 0xB0 | ch, num, 0);
                    c->counts[k] = v & 0x3F;
                }
            } else {  // toggle tool: an odd count means on (for the usual off-by-default switches)
                bool on = v & 1;
                if (c->ctl[num] == RTPJ_UNKNOWN || (c->ctl[num] >= 64) != on) play(rp, 0xB0 | ch, num, on ? 127 : 0);
            }
        }
        p += 1 + 2 * n;
    }
    if (toc & TOC_M) {  // RPN/NRPN: not repaired, skipped by its LENGTH
        if (end - p < 2) return false;
        size_t m = (size_t)((p[0] & 0x03) << 8 | p[1]);
        if (m < 2 || (size_t)(end - p) < m) return false;
        p += m;
    }
    if (toc & TOC_W) {
        if (end - p < 2) return false;
        uint16_t bend = (uint16_t)((p[1] & 0x7F) << 7 | (p[0] & 0x7F));
        if (c->bend != bend) play(rp, 0xE0 | ch, p[0] & 0x7F, p[1] & 0x7F);
        p += 2;
    }
    if (toc & TOC_N) {
        if (end - p < 2) return false;
        int n = p[0] & 0x7F, low = p[1] >> 4, high = p[1] & 0x0F;
        if (n == 127 && low == 15 && high == 0) n = 128;
        int offs;
        if (low <= high)
            offs = high - low + 1;
        else if (low == 15 && high <= 1)
            offs = 0;
        else
            return false;
        if (end - p < 2 + 2 * n + offs) return false;
        const uint8_t *log = p + 2;
        for (int i = 0; i < n; i++) {
            uint8_t note = log[2 * i] & 0x7F, vel = log[2 * i + 1] & 0x7F;
            bool y = log[2 * i + 1] & 0x80;
            // A NoteOn we missed: play it if the sender says it is still fresh. One we have is left alone.
            if (vel && y && !bit(c->notes, note)) play(rp, 0x90 | ch, note, vel);
        }
        const uint8_t *off = log + 2 * n;
        for (int i = 0; i < offs; i++) {
            for (int k = 0; k < 8; k++) {
                int note = (low + i) * 8 + k;
                if ((off[i] & 0x80 >> k) && note < 128 && bit(c->notes, note)) play(rp, 0x80 | ch, (uint8_t)note, 64);
            }
        }
        p += 2 + 2 * n + offs;
    }
    if (toc & TOC_E) {  // note extras: skipped
        if (end - p < 1) return false;
        int n = (p[0] & 0x7F) + 1;
        if (end - p < 1 + 2 * n) return false;
        p += 1 + 2 * n;
    }
    if (toc & TOC_T) {
        if (end - p < 1) return false;
        uint8_t pr = p[0] & 0x7F;
        if (c->pressure != pr) play(rp, 0xD0 | ch, pr, 0);
        p += 1;
    }
    // Chapter A (poly pressure) would be last; nothing after it to reach, so it is not parsed.
    return true;
}

bool rtpj_rx_recover(rtpj_rx_t *r, const uint8_t *j, size_t len, rtpj_emit_fn emit, void *ctx) {
    if (len < 3) return false;
    repair_t rp = {r, emit, ctx};
    bool sys = j[0] & 0x40, chans = j[0] & 0x20;
    int totchan = (j[0] & 0x0F) + 1;
    size_t pos = 3;
    if (sys) {  // the system journal: skipped by its LENGTH
        if (len - pos < 2) return false;
        size_t n = (size_t)((j[pos] & 0x03) << 8 | j[pos + 1]);
        if (n < 2 || len - pos < n) return false;
        pos += n;
    }
    if (!chans) return true;
    for (int i = 0; i < totchan; i++) {
        if (len - pos < 3) return false;
        size_t n = (size_t)((j[pos] & 0x03) << 8 | j[pos + 1]);
        if (n < 3 || len - pos < n) return false;
        if (!recover_channel(&rp, j[pos] >> 3 & 0x0F, j[pos + 2], j + pos + 3, n - 3)) return false;
        pos += n;
    }
    return true;
}
