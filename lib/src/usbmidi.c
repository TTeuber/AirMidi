#include "airmidi/usbmidi.h"

#include <string.h>

// MIDI bytes per Code Index Number (USB MIDI 1.0, table 4-1). 0 and 1 are reserved.
static const uint8_t cin_bytes[16] = {0, 0, 2, 3, 3, 1, 2, 3, 3, 3, 3, 3, 2, 2, 3, 1};

int usbmidi_packet_bytes(const uint8_t pkt[4], uint8_t out[3]) {
    int n = cin_bytes[pkt[0] & 0x0F];
    memcpy(out, pkt + 1, (size_t)n);
    return n;
}

void usbmidi_tx_init(usbmidi_tx_t *t, uint8_t cable) {
    memset(t, 0, sizeof *t);
    t->cable = cable & 0x0F;
    midi1_out_init(&t->m1, false);
}

typedef struct {
    uint8_t (*p)[4];
    int n;
} packets_t;

static void put(usbmidi_tx_t *t, packets_t *o, uint8_t cin, const uint8_t *b, int len) {
    uint8_t *p = o->p[o->n++];
    p[0] = (uint8_t)(t->cable << 4 | cin);
    memset(p + 1, 0, 3);
    memcpy(p + 1, b, (size_t)len);
}

// The CIN of a non-SysEx message by its status byte.
static uint8_t cin_of(uint8_t status) {
    if (status >= 0xF8) return 0xF;         // real-time: single byte
    if (status < 0xF0) return status >> 4;  // channel voice: the status nibble
    if (status == 0xF2) return 0x3;         // song position: three-byte system common
    if (status == 0xF6) return 0x5;         // tune request: single-byte system common
    return 0x2;                             // MTC quarter frame, song select: two-byte system common
}

static void sysex_byte(usbmidi_tx_t *t, packets_t *o, uint8_t b) {
    t->sx[t->n_sx++] = b;
    if (t->n_sx == 3) put(t, o, 0x4, t->sx, 3), t->n_sx = 0;  // SysEx starts or continues
}

// F7, and the last packet: CIN 5, 6 or 7 for 1, 2 or 3 bytes.
static void sysex_end(usbmidi_tx_t *t, packets_t *o) {
    t->sx[t->n_sx++] = 0xF7;
    put(t, o, (uint8_t)(0x4 + t->n_sx), t->sx, t->n_sx);
    t->n_sx = 0;
    t->in_sysex = false;
}

int usbmidi_tx_encode(usbmidi_tx_t *t, const ump_t *u, uint8_t out[USBMIDI_MAX_PACKETS][4]) {
    packets_t o = {out, 0};
    uint8_t mt = ump_mt(u);
    if (mt == UMP_MT_SYSEX7) {
        uint8_t st = ump_sysex7_status(u), d[6];
        int nd = ump_sysex7_data(u, d);
        if (st == UMP_SYSEX_COMPLETE || st == UMP_SYSEX_START) {
            if (t->in_sysex) sysex_end(t, &o);  // the previous one never ended: end it here
            t->in_sysex = true;
            sysex_byte(t, &o, 0xF0);
        } else if (!t->in_sysex) {
            return 0;  // the rest of a SysEx whose start we never had
        }
        for (int i = 0; i < nd; i++) sysex_byte(t, &o, d[i]);
        if (st == UMP_SYSEX_COMPLETE || st == UMP_SYSEX_END) sysex_end(t, &o);
        return o.n;
    }
    if (mt == UMP_MT_SYSTEM && ump_status(u) >= 0xF8) {  // real-time: its own packet, even inside a SysEx
        uint8_t b = ump_status(u);
        put(t, &o, 0xF, &b, 1);
        return o.n;
    }
    uint8_t b[MIDI1_OUT_MAX];
    size_t nb = midi1_out_encode(&t->m1, u, b);
    if (!nb) return 0;
    if (t->in_sysex) sysex_end(t, &o);
    for (size_t k = 0; k < nb && o.n < USBMIDI_MAX_PACKETS;) {  // an RPN translated down is four CCs
        int dl = midi1_data_len(b[k]);
        if (dl < 0 || k + 1 + (size_t)dl > nb) break;
        put(t, &o, cin_of(b[k]), b + k, 1 + dl);
        k += 1 + (size_t)dl;
    }
    return o.n;
}
