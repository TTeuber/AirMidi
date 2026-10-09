#include "airmidi/rtpmidi.h"

#include <stdio.h>
#include <string.h>

// AppleMIDI session commands: 0xFFFF and two ASCII letters.
enum {
    CMD_IN = 0x494E,  // invitation
    CMD_OK = 0x4F4B,  // invitation accepted
    CMD_NO = 0x4E4F,  // invitation rejected
    CMD_BY = 0x4259,  // end of session
    CMD_CK = 0x434B,  // clock synchronisation
    CMD_RS = 0x5253,  // receiver feedback
};

#define VERSION 2
#define WAIT_DATA_MS 10000  // a client that accepted on the control port but never came to the data port
#define RECLAIM_MS 60000    // a session silent this long gives its slot to a new client
#define FEEDBACK_MS 250     // receiver feedback at most this often (it shortens the client's journal)

static const uint32_t guard_ms[] = {20, 100, 500};  // journal-only packets after the last MIDI

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8), p[1] = (uint8_t)v; }
static void put32(uint8_t *p, uint32_t v) { put16(p, (uint16_t)(v >> 16)), put16(p + 2, (uint16_t)v); }
static void put64(uint8_t *p, uint64_t v) { put32(p, (uint32_t)(v >> 32)), put32(p + 4, (uint32_t)v); }
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t get32(const uint8_t *p) { return (uint32_t)get16(p) << 16 | get16(p + 2); }
static uint64_t get64(const uint8_t *p) { return (uint64_t)get32(p) << 32 | get32(p + 4); }

static uint32_t rnd(rtpmidi_t *h) { return h->seed = h->seed * 1664525u + 1013904223u; }

// The session clock, in the 100 us units AppleMIDI uses for CK and RTP timestamps (see rtpmidi_set_clock).
static uint64_t clock_100us(const rtpmidi_t *h, uint32_t now_ms) { return h->clock_base + (uint64_t)now_ms * 10; }

void rtpmidi_init(rtpmidi_t *h, const char *name, uint32_t ssrc, uint32_t seed, const rtpmidi_callbacks_t *cb,
                  void *ctx) {
    memset(h, 0, sizeof *h);
    snprintf(h->name, sizeof h->name, "%s", name);
    h->ssrc = ssrc;
    h->seed = seed;
    h->cb = *cb;
    h->ctx = ctx;
}

static int find_by_ssrc(rtpmidi_t *h, uint32_t ssrc) {
    for (int i = 0; i < RTPMIDI_SESSIONS; i++)
        if (h->s[i].state != RTPMIDI_IDLE && h->s[i].state != RTPMIDI_INVITE_CTRL && h->s[i].peer_ssrc == ssrc)
            return i;  // (inviting the control port, the client's SSRC is not known yet)
    return -1;
}

static void set_state(rtpmidi_session_t *s, rtpmidi_state_t st, uint32_t now) {
    s->state = st;
    s->state_ms = now;
}

// Back to idle; tells the caller if the session had been up.
static void drop(rtpmidi_t *h, int i) {
    bool was_up = h->s[i].state == RTPMIDI_UP;
    h->s[i].state = RTPMIDI_IDLE;
    if (was_up) h->cb.session_changed(h->ctx, i, false, h->s[i].peer_name);
}

static int free_slot(rtpmidi_t *h, uint32_t now) {
    for (int i = 0; i < RTPMIDI_SESSIONS; i++)
        if (h->s[i].state == RTPMIDI_IDLE) return i;
    for (int i = 0; i < RTPMIDI_SESSIONS; i++) {
        if (h->s[i].state == RTPMIDI_UP && now - h->s[i].last_rx_ms >= RECLAIM_MS) {
            drop(h, i);
            return i;
        }
    }
    return -1;
}

// IN / OK / NO / BY: signature, command, version, token, SSRC, and (IN, OK) our name.
static void send_session_cmd(rtpmidi_t *h, int sock, const rtpmidi_addr_t *to, uint16_t cmd, uint32_t token) {
    uint8_t p[16 + sizeof h->name];
    size_t len = 16;
    put16(p, 0xFFFF), put16(p + 2, cmd), put32(p + 4, VERSION), put32(p + 8, token), put32(p + 12, h->ssrc);
    if (cmd == CMD_IN || cmd == CMD_OK) {
        size_t n = strlen(h->name) + 1;
        memcpy(p + 16, h->name, n);
        len += n;
    }
    h->cb.send(h->ctx, sock, to, p, len);
}

static void send_ck(rtpmidi_t *h, const rtpmidi_session_t *s, uint8_t count, const uint64_t ts[3]) {
    uint8_t p[36] = {0};
    put16(p, 0xFFFF), put16(p + 2, CMD_CK), put32(p + 4, h->ssrc);
    p[8] = count;
    for (int i = 0; i < 3; i++) put64(p + 12 + 8 * i, ts[i]);
    h->cb.send(h->ctx, RTPMIDI_DATA, &s->data, p, sizeof p);
}

static void start_session(rtpmidi_t *h, int i, uint32_t now, bool resumed) {
    rtpmidi_session_t *s = &h->s[i];
    set_state(s, RTPMIDI_UP, now);
    s->tx_seq = (uint16_t)rnd(h);
    rtpj_tx_init(&s->jtx, s->tx_seq);
    midi1_out_init(&s->m1, false);  // every command with its status byte: simpler than running status, same size
    s->sysex_cont = false;
    s->guard = sizeof guard_ms / sizeof guard_ms[0];  // nothing sent yet, nothing to guard
    s->rx_started = s->feedback_due = false;
    midi1_in_init(&s->parser, 0);
    rtpj_rx_init(&s->jrx);
    memset(&s->stats, 0, sizeof s->stats);
    s->last_rx_ms = s->feedback_ms = now;
    s->tries = 0;
    s->next_ck_ms = now;
    s->resumed = s->seq_unknown = resumed;
    h->cb.session_changed(h->ctx, i, true, s->peer_name);
}

static void send_packet(rtpmidi_t *h, rtpmidi_session_t *s, uint8_t *p, size_t n, uint32_t now);

// A clock sync or MIDI from a client we have no session with: one we had before a restart. Take it back.
static int resume(rtpmidi_t *h, uint32_t ssrc, const rtpmidi_addr_t *from, uint32_t now) {
    int i = free_slot(h, now);
    if (i < 0) return -1;
    rtpmidi_session_t *s = &h->s[i];
    s->initiator = false;
    s->data = *from;
    s->ctrl = (rtpmidi_addr_t){from->ip, (uint16_t)(from->port - 1)};  // AppleMIDI: data = control + 1
    s->peer_ssrc = ssrc;
    s->token = 0;
    s->peer_name[0] = 0;
    start_session(h, i, now, true);
    // The two probes (see rtpmidi.h): send_packet() numbers each with tx_seq and counts it up by one.
    s->probe[0] = s->tx_seq;
    send_packet(h, s, h->pkt, 0, now);
    s->tx_seq = s->probe[1] = s->probe[0] + 0x8000;
    send_packet(h, s, h->pkt, 0, now);
    s->state_ms = now;
    return i;
}

static void copy_name(char *dst, size_t cap, const uint8_t *p, size_t len) {
    size_t n = 0;
    while (n < len && n + 1 < cap && p[n]) n++;
    memcpy(dst, p, n);
    dst[n] = 0;
}

// ---- MIDI lists ----------------------------------------------------------------------------------------------

// One command of a MIDI list: a channel or system message with its status resolved (running status), or a SysEx
// segment (status F0 or F7 = its head, data between, tail = its last byte: F0 continues, F7 ends, F4 cancels).
typedef void (*command_fn)(void *ctx, uint8_t status, const uint8_t *data, size_t n, uint8_t tail);

// Walk a MIDI list (RFC 6295 section 3): delta times are skipped (commands are delivered on arrival). Returns false
// if it is malformed; the commands before the error have been passed on.
static bool walk_list(const uint8_t *l, size_t n, bool z, command_fn fn, void *ctx) {
    size_t pos = 0;
    bool first = true;
    uint8_t running = 0;
    while (pos < n) {
        if (!first || z) {  // delta time: 1-4 bytes, the last one with bit 7 clear
            int k = 0;
            while (pos < n && (l[pos] & 0x80)) pos++, k++;
            if (k > 3 || pos >= n) return false;
            pos++;  // its last byte; a delta time with no command after it is legal (the loop ends)
        }
        first = false;
        if (pos >= n) break;
        uint8_t b = l[pos];
        if (b == 0xF0 || b == 0xF7) {  // SysEx, whole or a segment: data up to the next status byte
            size_t e = pos + 1;
            while (e < n && l[e] < 0x80) e++;
            if (e >= n) return false;
            fn(ctx, b, l + pos + 1, e - pos - 1, l[e]);
            running = 0;
            pos = e + 1;
        } else if (b >= 0xF8) {  // real-time: does not touch running status
            fn(ctx, b, NULL, 0, 0);
            pos++;
        } else if (b >= 0xF1) {  // system common: cancels running status
            int dl = midi1_data_len(b);
            if (dl < 0) {  // F4 / F5: undefined, ended by F7 (RFC 6295 3.2); skipped
                while (pos < n && l[pos] != 0xF7) pos++;
                pos++;
            } else {
                if (pos + 1 + (size_t)dl > n) return false;
                fn(ctx, b, l + pos + 1, (size_t)dl, 0);
                pos += 1 + (size_t)dl;
            }
            running = 0;
        } else {
            uint8_t st = running;
            if (b >= 0x80) st = running = b, pos++;
            if (!st) return false;  // data with no status to belong to
            size_t dl = (size_t)midi1_data_len(st);
            if (pos + dl > n) return false;
            fn(ctx, st, l + pos, dl, 0);
            pos += dl;
        }
    }
    return true;
}

// ---- receiving -----------------------------------------------------------------------------------------------

typedef struct {
    rtpmidi_t *h;
    int i;
} rx_ctx_t;

static void feed(rtpmidi_t *h, int i, uint8_t byte) {
    rtpmidi_session_t *s = &h->s[i];
    ump_t u[2];
    int n = midi1_in_feed(&s->parser, byte, u);
    for (int k = 0; k < n; k++) {
        if (ump_mt(&u[k]) == UMP_MT_M1_CV || ump_status(&u[k]) == 0xFF)
            rtpj_rx_track(&s->jrx, ump_status(&u[k]), ump_byte3(&u[k]), ump_byte4(&u[k]));
        h->cb.deliver(h->ctx, i, &u[k]);
    }
}

static void rx_command(void *ctx, uint8_t status, const uint8_t *data, size_t n, uint8_t tail) {
    rx_ctx_t *c = ctx;
    rtpmidi_session_t *s = &c->h->s[c->i];
    if (status == 0xF0 || status == 0xF7) {
        // A continuation of a SysEx we are not in (its start was lost, or cancelled) is skipped.
        if (status == 0xF7 && !s->parser.in_sysex) return;
        if (status == 0xF0) feed(c->h, c->i, 0xF0);
        for (size_t k = 0; k < n; k++) feed(c->h, c->i, data[k]);
        // F7 ends it; F5 is an F7 the source dropped; F4 cancels, which a MIDI 1.0 receiver can only see as an
        // early end (RFC 6295 3.2).
        if (tail == 0xF7 || tail == 0xF5 || tail == 0xF4) feed(c->h, c->i, 0xF7);
        return;
    }
    feed(c->h, c->i, status);
    for (size_t k = 0; k < n; k++) feed(c->h, c->i, data[k]);
}

static void rx_repair(void *ctx, uint8_t status, uint8_t d1, uint8_t d2) {
    rx_ctx_t *c = ctx;
    ump_t u = ump_m1_cv(0, status, d1, d2);
    c->h->cb.deliver(c->h->ctx, c->i, &u);
}

static void receive_rtp(rtpmidi_t *h, const rtpmidi_addr_t *from, const uint8_t *p, size_t len, uint32_t now) {
    if (len < 13 || (p[0] & 0xC0) != 0x80 || (p[1] & 0x7F) != RTPMIDI_PT) return;
    int i = find_by_ssrc(h, get32(p + 8));
    if (i < 0) i = resume(h, get32(p + 8), from, now);
    if (i < 0 || h->s[i].state != RTPMIDI_UP) return;
    rtpmidi_session_t *s = &h->s[i];
    s->last_rx_ms = now;
    size_t off = 12 + 4 * (size_t)(p[0] & 0x0F);  // CSRCs, unused by AppleMIDI
    if (p[0] & 0x10) {                            // header extension, likewise
        if (len < off + 4) goto bad;
        off += 4 + 4 * (size_t)get16(p + off + 2);
    }
    if (len < off + 1) goto bad;
    uint8_t hd = p[off];
    size_t n = hd & 0x0F;
    if (hd & 0x80) {
        if (len < off + 2) goto bad;
        n = n << 8 | p[off + 1];
        off += 2;
    } else {
        off += 1;
    }
    if (len < off + n) goto bad;

    uint16_t seq = get16(p + 2);
    bool loss = !s->rx_started;  // the first packet of a stream is treated as the end of a loss (RFC 6295 4)
    if (s->rx_started) {
        int16_t d = (int16_t)(seq - (uint16_t)(s->rx_seq + 1));
        if (d < 0) {  // late or duplicate: its content is in the journals of the packets that came first
            s->stats.rx_late++;
            return;
        }
        if (d > 0) s->stats.rx_lost += (uint32_t)d, loss = true;
    }
    s->rx_started = true;
    s->rx_seq = seq;
    s->feedback_due = true;
    s->stats.rx_packets++;

    rx_ctx_t c = {h, i};
    if (loss) {
        if (s->parser.in_sysex) feed(h, i, 0xF7);  // its middle went missing: end it here, skip the rest
        if (hd & 0x40) {
            uint32_t before = s->jrx.repairs;
            if (!rtpj_rx_recover(&s->jrx, p + off + n, len - off - n, rx_repair, &c)) s->stats.rx_bad++;
            s->stats.rx_repairs += s->jrx.repairs - before;
        }
    }
    if (!walk_list(p + off, n, hd & 0x20, rx_command, &c)) s->stats.rx_bad++;
    return;
bad:
    s->stats.rx_bad++;
}

static void receive_cmd(rtpmidi_t *h, int sock, const rtpmidi_addr_t *from, const uint8_t *p, size_t len,
                        uint32_t now) {
    uint16_t cmd = get16(p + 2);
    if (cmd == CMD_IN || cmd == CMD_OK || cmd == CMD_NO || cmd == CMD_BY) {
        if (len < 16) return;
        uint32_t token = get32(p + 8), ssrc = get32(p + 12);
        int i = find_by_ssrc(h, ssrc);
        if (cmd == CMD_IN && sock == RTPMIDI_CONTROL) {
            // A client we have a session with inviting again has started over on its side.
            if (i >= 0) drop(h, i);
            if (i < 0) i = free_slot(h, now);
            if (i < 0) {
                send_session_cmd(h, sock, from, CMD_NO, token);
                return;
            }
            rtpmidi_session_t *s = &h->s[i];
            s->initiator = false;
            s->ctrl = *from;
            s->peer_ssrc = ssrc;
            s->token = token;
            copy_name(s->peer_name, sizeof s->peer_name, p + 16, len - 16);
            set_state(s, RTPMIDI_WAIT_DATA, now);
            send_session_cmd(h, sock, from, CMD_OK, token);
        } else if (cmd == CMD_IN) {  // data port
            if (i < 0 || h->s[i].token != token) {
                send_session_cmd(h, sock, from, CMD_NO, token);
                return;
            }
            rtpmidi_session_t *s = &h->s[i];
            s->data = *from;
            send_session_cmd(h, sock, from, CMD_OK, token);  // again if it is a repeat: our OK may have been lost
            if (s->state == RTPMIDI_WAIT_DATA) start_session(h, i, now, false);
        } else if (cmd == CMD_BY) {
            if (i >= 0) drop(h, i);
        } else {  // OK / NO to our invitation: matched by token, the client's SSRC is new to us
            for (i = 0; i < RTPMIDI_SESSIONS; i++) {
                rtpmidi_session_t *s = &h->s[i];
                bool ctrl_step = s->state == RTPMIDI_INVITE_CTRL && sock == RTPMIDI_CONTROL;
                bool data_step = s->state == RTPMIDI_INVITE_DATA && sock == RTPMIDI_DATA;
                if (!s->initiator || s->token != token || !(ctrl_step || data_step)) continue;
                if (cmd == CMD_NO) {
                    set_state(s, RTPMIDI_IDLE, now);
                } else if (ctrl_step) {
                    s->peer_ssrc = ssrc;
                    copy_name(s->peer_name, sizeof s->peer_name, p + 16, len - 16);
                    set_state(s, RTPMIDI_INVITE_DATA, now);
                    s->tries = 1;
                    send_session_cmd(h, RTPMIDI_DATA, &s->data, CMD_IN, token);
                } else {
                    s->data = *from;
                    start_session(h, i, now, false);
                }
                break;
            }
        }
        return;
    }
    if (cmd == CMD_CK && len >= 36) {
        int i = find_by_ssrc(h, get32(p + 4));
        if (i < 0 && sock == RTPMIDI_DATA && p[8] == 0) i = resume(h, get32(p + 4), from, now);
        if (i < 0 || h->s[i].state != RTPMIDI_UP) return;
        rtpmidi_session_t *s = &h->s[i];
        s->last_rx_ms = now;
        uint64_t ts[3] = {get64(p + 12), get64(p + 20), get64(p + 28)};
        // The inviter sends CK 0 with its time, the other side answers CK 1 with its own, the inviter closes with
        // CK 2; each side then knows the round trip and the offset between the clocks.
        if (p[8] == 0) {
            ts[1] = clock_100us(h, now), ts[2] = 0;
            send_ck(h, s, 1, ts);
        } else if (p[8] == 1) {
            ts[2] = clock_100us(h, now);
            send_ck(h, s, 2, ts);
        }
        return;
    }
    if (cmd == CMD_RS && len >= 12) {
        int i = find_by_ssrc(h, get32(p + 4));
        if (i < 0 || h->s[i].state != RTPMIDI_UP) return;
        rtpmidi_session_t *s = &h->s[i];
        s->last_rx_ms = now;
        uint16_t seen = get16(p + 8);
        if (s->seq_unknown) {
            // Taken back: carry on after the probe the client took. Feedback about neither (sent before the probes
            // arrived) is not trusted unless nothing else comes for 3 s.
            if (seen != s->probe[0] && seen != s->probe[1] && now - s->state_ms < 3000) return;
            s->seq_unknown = false;
            s->tx_seq = seen + 1;
            rtpj_tx_init(&s->jtx, s->tx_seq);
            return;
        }
        rtpj_tx_ack(&s->jtx, seen, s->tx_seq);
    }
}

void rtpmidi_set_clock(rtpmidi_t *h, uint64_t base_100us) { h->clock_base = base_100us; }

void rtpmidi_receive(rtpmidi_t *h, int sock, const rtpmidi_addr_t *from, const uint8_t *pkt, size_t len, uint32_t now) {
    if (len >= 4 && pkt[0] == 0xFF && pkt[1] == 0xFF)
        receive_cmd(h, sock, from, pkt, len, now);
    else if (sock == RTPMIDI_DATA)
        receive_rtp(h, from, pkt, len, now);
}

// ---- sending -------------------------------------------------------------------------------------------------

typedef struct {
    uint8_t *l;
    size_t n;
    bool first;  // no command yet: the first one goes without a delta time (Z = 0)
    bool open;   // a SysEx segment is open in this packet
} list_t;

static void command_start(list_t *L) {
    if (!L->first) L->l[L->n++] = 0x00;  // delta time 0: we do not schedule
    L->first = false;
}

// End a SysEx that will never be finished (a new one starts, or another message comes before its end).
static void end_sysex(rtpmidi_session_t *s, list_t *L) {
    if (L->open) {
        L->l[L->n++] = 0xF7;
        L->open = false;
    } else if (s->sysex_cont) {
        command_start(L);  // an empty "last" segment (RFC 6295 3.2)
        L->l[L->n++] = 0xF7, L->l[L->n++] = 0xF7;
    }
    s->sysex_cont = false;
}

static void add_ump(rtpmidi_session_t *s, list_t *L, const ump_t *u) {
    uint8_t mt = ump_mt(u);
    if (mt == UMP_MT_SYSEX7) {
        uint8_t st = ump_sysex7_status(u), d[6];
        int nd = ump_sysex7_data(u, d);
        if (st == UMP_SYSEX_COMPLETE || st == UMP_SYSEX_START) {
            end_sysex(s, L);
            command_start(L);
            L->l[L->n++] = 0xF0;
        } else {
            if (!L->open && !s->sysex_cont) return;               // the rest of a SysEx whose start we never had
            if (!L->open) command_start(L), L->l[L->n++] = 0xF7;  // a "middle" or "last" segment
        }
        memcpy(L->l + L->n, d, (size_t)nd);
        L->n += (size_t)nd;
        if (st == UMP_SYSEX_COMPLETE || st == UMP_SYSEX_END) {
            L->l[L->n++] = 0xF7;
            L->open = s->sysex_cont = false;
        } else {
            L->open = true;
        }
        return;
    }
    if (mt == UMP_MT_SYSTEM && ump_status(u) >= 0xF8) {
        // Real-time inside a SysEx: the SysEx is cut into segments around it (only real-time may go between them).
        if (L->open) L->l[L->n++] = 0xF0, L->open = false, s->sysex_cont = true;
        command_start(L);
        L->l[L->n++] = ump_status(u);
        return;
    }
    uint8_t b[MIDI1_OUT_MAX];
    size_t nb = midi1_out_encode(&s->m1, u, b);
    if (!nb) return;
    if (L->open || s->sysex_cont) end_sysex(s, L);
    for (size_t k = 0; k < nb;) {  // one UMP may become several messages (an RPN becomes four CCs)
        int dl = midi1_data_len(b[k]);
        if (dl < 0 || k + 1 + (size_t)dl > nb) break;
        command_start(L);
        memcpy(L->l + L->n, b + k, 1 + (size_t)dl);
        L->n += 1 + (size_t)dl;
        k += 1 + (size_t)dl;
    }
}

typedef struct {
    rtpmidi_session_t *s;
    uint16_t seq;
    uint32_t now;
} record_ctx_t;

static void record_command(void *ctx, uint8_t status, const uint8_t *data, size_t n, uint8_t tail) {
    (void)tail;
    record_ctx_t *c = ctx;
    if (status >= 0xF0 && status != 0xFF) return;
    rtpj_tx_record(&c->s->jtx, c->seq, c->now, status, n > 0 ? data[0] : 0, n > 1 ? data[1] : 0);
}

// Header, command section, journal; then the packet's commands go into the journal's history.
static void send_packet(rtpmidi_t *h, rtpmidi_session_t *s, uint8_t *p, size_t n, uint32_t now) {
    uint16_t seq = s->tx_seq;
    p[0] = 0x80;  // version 2
    // M = 0, against RFC 6295 2.1 (1 when the MIDI list is not empty): macOS 26 holds packets with M = 1 back by
    // ~25 ms and drops some (measured; 4-6 ms and none lost with M = 0). Apple's own packets have it 0.
    p[1] = RTPMIDI_PT;
    put16(p + 2, seq);
    put32(p + 4, (uint32_t)clock_100us(h, now));
    put32(p + 8, h->ssrc);
    p[12] = (uint8_t)(0x80 | 0x40 | (n >> 8 & 0x0F));  // long header (B), journal (J), Z = 0, P = 0
    p[13] = (uint8_t)n;
    size_t jl = rtpj_tx_build(&s->jtx, seq, now, p + 14 + n, RTPMIDI_MAX_PACKET - 14 - n);
    h->cb.send(h->ctx, RTPMIDI_DATA, &s->data, p, 14 + n + jl);
    record_ctx_t rc = {s, seq, now};
    walk_list(p + 14, n, false, record_command, &rc);
    if (!n) rtpj_tx_record(&s->jtx, seq, now, 0, 0, 0);  // still note the send time
    s->tx_seq++;
    s->stats.tx_packets++;
    s->stats.journal_overflows = s->jtx.overflows;
}

bool rtpmidi_send(rtpmidi_t *h, int i, const ump_t *u, int count, uint32_t now) {
    rtpmidi_session_t *s = &h->s[i];
    if (s->state != RTPMIDI_UP) return false;
    if (s->seq_unknown) {
        s->stats.tx_held += (uint32_t)count;
        return true;
    }
    int k = 0;
    while (k < count) {
        uint8_t *p = h->pkt;
        list_t L = {p + 14, 0, true, false};
        // 32 bytes of headroom: the most one UMP can add (four 3-byte messages with their delta times) plus the
        // byte that closes a SysEx segment.
        while (k < count && L.n + 32 < RTPMIDI_LIST_MAX) add_ump(s, &L, &u[k++]);
        if (L.open) L.l[L.n++] = 0xF0, s->sysex_cont = true;  // the SysEx goes on in the next packet
        if (!L.n) continue;
        send_packet(h, s, p, L.n, now);
        s->last_send_ms = now;
        s->guard = 0;
    }
    return true;
}

// ---- time ----------------------------------------------------------------------------------------------------

void rtpmidi_tick(rtpmidi_t *h, uint32_t now) {
    for (int i = 0; i < RTPMIDI_SESSIONS; i++) {
        rtpmidi_session_t *s = &h->s[i];
        switch (s->state) {
            case RTPMIDI_INVITE_CTRL:
            case RTPMIDI_INVITE_DATA:
                if (now - s->state_ms < 1000) break;
                if (s->tries >= RTPMIDI_INVITE_TRIES) {
                    set_state(s, RTPMIDI_IDLE, now);
                    break;
                }
                s->tries++;
                s->state_ms = now;
                if (s->state == RTPMIDI_INVITE_CTRL)
                    send_session_cmd(h, RTPMIDI_CONTROL, &s->ctrl, CMD_IN, s->token);
                else
                    send_session_cmd(h, RTPMIDI_DATA, &s->data, CMD_IN, s->token);
                break;
            case RTPMIDI_WAIT_DATA:
                if (now - s->state_ms >= WAIT_DATA_MS) set_state(s, RTPMIDI_IDLE, now);
                break;
            case RTPMIDI_UP:
                if (s->seq_unknown && now - s->state_ms >= 5000) s->seq_unknown = false;  // no feedback: carry on
                // The inviter keeps the clocks in step: six syncs 1.5 s apart, then one every 10 s (as macOS does).
                if (s->initiator && (int32_t)(now - s->next_ck_ms) >= 0) {
                    uint64_t ts[3] = {clock_100us(h, now), 0, 0};
                    send_ck(h, s, 0, ts);
                    s->next_ck_ms = now + (s->tries++ < 5 ? 1500 : 10000);
                }
                if (s->feedback_due && now - s->feedback_ms >= FEEDBACK_MS) {
                    uint8_t p[12] = {0};
                    put16(p, 0xFFFF), put16(p + 2, CMD_RS), put32(p + 4, h->ssrc), put16(p + 8, s->rx_seq);
                    h->cb.send(h->ctx, RTPMIDI_CONTROL, &s->ctrl, p, sizeof p);
                    s->feedback_due = false;
                    s->feedback_ms = now;
                }
                if (s->guard < (int)(sizeof guard_ms / sizeof guard_ms[0]) &&
                    now - s->last_send_ms >= guard_ms[s->guard]) {
                    s->guard++;
                    if (rtpj_tx_pending(&s->jtx)) send_packet(h, s, h->pkt, 0, now);
                }
                break;
            default:
                break;
        }
    }
}

int rtpmidi_invite(rtpmidi_t *h, const rtpmidi_addr_t *ctrl, uint32_t now) {
    int i = free_slot(h, now);
    if (i < 0) return -1;
    rtpmidi_session_t *s = &h->s[i];
    s->initiator = true;
    s->ctrl = *ctrl;
    s->data = (rtpmidi_addr_t){ctrl->ip, (uint16_t)(ctrl->port + 1)};
    s->token = rnd(h);
    s->peer_ssrc = 0;
    s->peer_name[0] = 0;
    s->tries = 1;
    set_state(s, RTPMIDI_INVITE_CTRL, now);
    send_session_cmd(h, RTPMIDI_CONTROL, ctrl, CMD_IN, s->token);
    return i;
}

void rtpmidi_end(rtpmidi_t *h, int i) {
    rtpmidi_session_t *s = &h->s[i];
    if (s->state == RTPMIDI_IDLE) return;
    send_session_cmd(h, RTPMIDI_CONTROL, &s->ctrl, CMD_BY, s->token);
    drop(h, i);
}
