#include "airmidi/blemidi.h"

int blemidi_decode(const uint8_t *pkt, size_t len, uint8_t *out) {
    if (len < 1 || (pkt[0] & 0xC0) != 0x80) return -1;  // header: bit 7 set, bit 6 reserved (0)
    int n = 0;
    bool after_ts = false;
    for (size_t i = 1; i < len; i++) {
        uint8_t b = pkt[i];
        if ((b & 0x80) && !after_ts) {
            after_ts = true;  // timestamp low byte
            continue;
        }
        after_ts = false;
        out[n++] = b;
    }
    return n;
}

void blemidi_tx_init(blemidi_tx_t *t, size_t max_payload, blemidi_emit_fn emit, void *ctx) {
    t->len = 0;
    t->emit = emit;
    t->ctx = ctx;
    midi1_out_init(&t->m1, false);
    t->max = BLEMIDI_MIN_PACKET;
    blemidi_tx_set_max(t, max_payload);
}

void blemidi_tx_flush(blemidi_tx_t *t) {
    if (t->len > 1) t->emit(t->ctx, t->pkt, t->len);
    t->len = 0;
}

void blemidi_tx_set_max(blemidi_tx_t *t, size_t max_payload) {
    if (max_payload > BLEMIDI_MAX_PACKET) max_payload = BLEMIDI_MAX_PACKET;
    if (max_payload < BLEMIDI_MIN_PACKET) max_payload = BLEMIDI_MIN_PACKET;
    if (t->len > max_payload) blemidi_tx_flush(t);
    t->max = max_payload;
}

// Make room for `need` more bytes (header included if a new packet starts) at time ts.
static void reserve(blemidi_tx_t *t, size_t need, uint16_t ts) {
    uint8_t hi = (ts >> 7) & 0x3F;
    // A receiver rebuilds each timestamp from the header's high bits plus the low byte, assuming a low byte smaller
    // than the previous one means the high bits rolled over once. Starting a new packet whenever the high bits
    // change keeps that true however long a packet stays open.
    if (t->len && (t->len + need > t->max || hi != t->pkt_hi)) blemidi_tx_flush(t);
    if (!t->len) {
        t->pkt[0] = 0x80 | hi;
        t->pkt_hi = hi;
        t->len = 1;
    }
}

// One message with its timestamp, kept in one packet.
static void put_message(blemidi_tx_t *t, const uint8_t *b, size_t n, uint16_t ts) {
    reserve(t, 1 + n, ts);
    t->pkt[t->len++] = 0x80 | (ts & 0x7F);
    for (size_t i = 0; i < n; i++) t->pkt[t->len++] = b[i];
}

void blemidi_tx_add(blemidi_tx_t *t, const ump_t *u, uint32_t now_ms) {
    uint8_t bytes[MIDI1_OUT_MAX];
    size_t n = midi1_out_encode(&t->m1, u, bytes);
    uint16_t ts = now_ms & 0x1FFF;
    for (size_t i = 0; i < n;) {
        uint8_t b = bytes[i];
        if (b == 0xF0 || b == 0xF7 || b >= 0xF8) {  // SysEx start/end and real-time: timestamp + the byte
            put_message(t, &b, 1, ts);
            i++;
        } else if (b & 0x80) {  // a whole channel / system common message
            int len = midi1_data_len(b);
            size_t m = 1 + (len > 0 ? (size_t)len : 0);
            if (i + m > n) m = n - i;
            put_message(t, bytes + i, m, ts);
            i += m;
        } else {  // SysEx data: may continue in the next packet, no timestamp needed
            reserve(t, 1, ts);
            t->pkt[t->len++] = b;
            i++;
        }
    }
}
