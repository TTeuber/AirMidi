#include "airmidi/nm2.h"

#include <stdio.h>
#include <string.h>

enum {
    CMD_INVITATION = 0x01,
    CMD_INVITATION_AUTH = 0x02,
    CMD_INVITATION_USER_AUTH = 0x03,
    CMD_ACCEPTED = 0x10,
    CMD_PING = 0x20,
    CMD_PING_REPLY = 0x21,
    CMD_RETRANSMIT_REQUEST = 0x80,
    CMD_RETRANSMIT_ERROR = 0x81,
    CMD_SESSION_RESET = 0x82,
    CMD_SESSION_RESET_REPLY = 0x83,
    CMD_NAK = 0x8F,
    CMD_BYE = 0xF0,
    CMD_BYE_REPLY = 0xF1,
    CMD_UMP_DATA = 0xFF,
};

enum { BYE_TIMEOUT = 0x04, BYE_NOT_ESTABLISHED = 0x05, BYE_TOO_MANY = 0x40 };
enum { NAK_NOT_SUPPORTED = 0x01 };

#define RETRANSMIT_AFTER_MS 10  // M2-124 7.2.3 suggests a short delay, so reordered packets can arrive first
#define RETRANSMIT_EVERY_MS 20
#define GIVE_UP_MS 150
#define PING_IDLE_MS 10000  // ping a client we have not heard from for this long
#define PING_EVERY_MS 10000
// A silent client is not dropped: macOS forgets a host whose session ended and never invites it again (nm2.h).
// Instead, a client silent for this long gives its slot to a new one when all slots are taken.
#define EVICT_SILENT_MS 60000
#define RESET_EVERY_MS 200  // Session Reset repeated until the reply (M2-124 6.11)
#define RESET_TRIES 5

// Zero-length UMP Data commands after the last data: two quick ones carry the last real command twice more (FEC),
// then the gaps grow, then they stop (M2-124 7.2.1).
static const uint32_t idle_after_ms[] = {20, 40, 300, 1000, 3000, 10000};
#define IDLE_STEPS (int)(sizeof idle_after_ms / sizeof idle_after_ms[0])

static void put32(uint8_t *p, uint32_t v) { p[0] = v >> 24, p[1] = v >> 16, p[2] = v >> 8, p[3] = v; }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | p[2] << 8 | p[3]; }

// A datagram being built: "MIDI" signature, then commands.
typedef struct {
    uint8_t b[NM2_MAX_PACKET];
    size_t n;
} packet_t;

static void packet_start(packet_t *p) {
    memcpy(p->b, "MIDI", 4);
    p->n = 4;
}

static bool packet_cmd(packet_t *p, uint8_t code, uint16_t csd, const uint8_t *payload, int words) {
    if (p->n + 4 + 4 * (size_t)words > sizeof p->b) return false;
    p->b[p->n] = code, p->b[p->n + 1] = (uint8_t)words, p->b[p->n + 2] = csd >> 8, p->b[p->n + 3] = csd & 0xFF;
    if (words) memcpy(p->b + p->n + 4, payload, 4 * (size_t)words);
    p->n += 4 + 4 * (size_t)words;
    return true;
}

static bool packet_data(packet_t *p, const nm2_cmd_t *c) {
    uint8_t w[4 * NM2_CMD_WORDS];
    for (int i = 0; i < c->n; i++) put32(w + 4 * i, c->w[i]);
    return packet_cmd(p, CMD_UMP_DATA, c->seq, w, c->n);
}

static void send_packet(nm2_host_t *h, const nm2_addr_t *to, const packet_t *p) {
    if (p->n > 4) h->cb.send(h->ctx, to, p->b, p->n);
}

// One command alone in a datagram.
static void send_one(nm2_host_t *h, const nm2_addr_t *to, uint8_t code, uint16_t csd, const uint8_t *pl, int words) {
    packet_t p;
    packet_start(&p);
    packet_cmd(&p, code, csd, pl, words);
    send_packet(h, to, &p);
}

static bool same_addr(const nm2_addr_t *a, const nm2_addr_t *b) { return a->ip == b->ip && a->port == b->port; }

static int find_session(nm2_host_t *h, const nm2_addr_t *a) {
    for (int i = 0; i < NM2_SESSIONS; i++)
        if (h->s[i].active && same_addr(&h->s[i].peer, a)) return i;
    return -1;
}

void nm2_host_init(nm2_host_t *h, const char *name, const char *piid, const nm2_callbacks_t *cb, void *ctx) {
    memset(h, 0, sizeof *h);
    snprintf(h->name, sizeof h->name, "%s", name);
    snprintf(h->piid, sizeof h->piid, "%s", piid);
    h->cb = *cb;
    h->ctx = ctx;
}

// ---- strings in commands: UTF-8, zero-padded to whole words ----------------------------------------------------

static int put_string(uint8_t *out, const char *s) {
    size_t len = strlen(s);
    int words = (int)((len + 3) / 4);
    memset(out, 0, 4 * (size_t)words);
    memcpy(out, s, len);
    return words;
}

// ---- sessions ------------------------------------------------------------------------------------------------

static void reset_streams(nm2_session_t *s, uint32_t now) {
    s->tx_seq = 0;
    s->hist_start = s->hist_count = 0;
    s->idle_step = -1;
    s->rx_next = 0;  // M2-124 7.1: a sender's first UMP Data command is number 0, so early losses show as a gap
    s->n_hold = 0;
    s->last_rx_ms = now;
    s->ping_sent_ms = 0;
    s->missed_pings = 0;
}

static void end_session(nm2_host_t *h, int i, uint8_t reason, bool send_bye) {
    nm2_session_t *s = &h->s[i];
    if (!s->active) return;
    if (send_bye) send_one(h, &s->peer, CMD_BYE, (uint16_t)reason << 8, NULL, 0);
    s->active = false;
    if (h->cb.session_changed) h->cb.session_changed(h->ctx, i, false, s->peer_name);
}

void nm2_host_end(nm2_host_t *h, int session, uint8_t reason) { end_session(h, session, reason, true); }

// A free slot for a new session, or the longest-silent one if silent long enough; -1 if none.
static int claim_slot(nm2_host_t *h, const nm2_addr_t *from, uint32_t now) {
    int i;
    for (i = 0; i < NM2_SESSIONS && h->s[i].active; i++) {
    }
    if (i == NM2_SESSIONS) {
        int oldest = 0;
        for (int k = 1; k < NM2_SESSIONS; k++)
            if (now - h->s[k].last_rx_ms > now - h->s[oldest].last_rx_ms) oldest = k;
        if (now - h->s[oldest].last_rx_ms < EVICT_SILENT_MS) return -1;
        end_session(h, oldest, BYE_TIMEOUT, true);
        i = oldest;
    }
    nm2_session_t *s = &h->s[i];
    memset(s, 0, sizeof *s);
    s->active = true;
    s->peer = *from;
    reset_streams(s, now);
    return i;
}

static void send_reset(nm2_host_t *h, nm2_session_t *s, uint32_t now) {
    send_one(h, &s->peer, CMD_SESSION_RESET, 0, NULL, 0);
    s->reset_sent_ms = now;
    s->reset_tries++;
}

// Take back a session the client still believes in (see nm2.h). Returns the session, or -1 if no slot is free.
static int resume(nm2_host_t *h, const nm2_addr_t *from, const char *name, uint32_t now) {
    int i = claim_slot(h, from, now);
    if (i < 0) return -1;
    nm2_session_t *s = &h->s[i];
    snprintf(s->peer_name, sizeof s->peer_name, "%s", name);
    s->resumed = true;
    s->reset_pending = true;
    send_reset(h, s, now);
    if (h->cb.session_changed) h->cb.session_changed(h->ctx, i, true, s->peer_name);
    return i;
}

void nm2_host_resume(nm2_host_t *h, const nm2_addr_t *peer, const char *name, uint32_t now) {
    if (find_session(h, peer) < 0) resume(h, peer, name, now);
}

static void invitation(nm2_host_t *h, const nm2_addr_t *from, const uint8_t *c, const uint8_t *pl, int words,
                       uint32_t now) {
    int i = find_session(h, from);
    // A resumed session that gets an Invitation is a client starting over: a new session.
    if (i >= 0 && h->s[i].resumed) end_session(h, i, 0, false), i = -1;
    bool fresh = i < 0;
    if (fresh) {
        i = claim_slot(h, from, now);
        if (i < 0) {  // all taken by clients heard from recently
            send_one(h, from, CMD_BYE, (uint16_t)BYE_TOO_MANY << 8, NULL, 0);
            return;
        }
        nm2_session_t *s = &h->s[i];
        int name_words = c[2] <= words ? c[2] : words;
        size_t len =
            4 * (size_t)name_words < sizeof s->peer_name - 1 ? 4 * (size_t)name_words : sizeof s->peer_name - 1;
        memcpy(s->peer_name, pl, len);
        s->peer_name[len] = 0;
    }
    // Accepted, also for a repeated invitation from a client already in a session (M2-124 6.4).
    uint8_t out[4 * 36];
    int nw = put_string(out, h->name);
    int pw = put_string(out + 4 * nw, h->piid);
    send_one(h, from, CMD_ACCEPTED, (uint16_t)nw << 8, out, nw + pw);
    if (fresh && h->cb.session_changed) h->cb.session_changed(h->ctx, i, true, h->s[i].peer_name);
}

// ---- receiving data ------------------------------------------------------------------------------------------

static void deliver(nm2_host_t *h, int i, const nm2_rxcmd_t *c) {
    for (int k = 0; k < c->n;) {
        ump_t u = {{0}};
        int nw = ump_words_for_mt(c->w[k] >> 28);
        if (k + nw > c->n) break;  // a UMP cut short: malformed, drop the rest
        for (int j = 0; j < nw; j++) u.w[j] = c->w[k + j];
        h->cb.deliver(h->ctx, i, &u);
        k += nw;
    }
}

// Deliver held commands that are next in line.
static void drain_hold(nm2_host_t *h, int i) {
    nm2_session_t *s = &h->s[i];
    while (s->n_hold && s->hold[0].seq == s->rx_next) {
        deliver(h, i, &s->hold[0]);
        s->rx_next++;
        memmove(s->hold, s->hold + 1, --s->n_hold * sizeof s->hold[0]);
    }
}

// Stop waiting: what is missing is lost; deliver what is held, in order.
static void give_up(nm2_host_t *h, int i) {
    nm2_session_t *s = &h->s[i];
    while (s->n_hold) {
        s->stats.rx_lost += (uint16_t)(s->hold[0].seq - s->rx_next);
        s->rx_next = s->hold[0].seq;
        drain_hold(h, i);
    }
}

static void deliver_words(nm2_host_t *h, int i, const uint8_t *pl, int words) {
    for (int k = 0; k < words;) {
        ump_t u = {{0}};
        int nw = ump_words_for_mt(get32(pl + 4 * k) >> 28);
        if (k + nw > words) break;  // a UMP cut short: malformed, drop the rest
        for (int j = 0; j < nw; j++) u.w[j] = get32(pl + 4 * (k + j));
        h->cb.deliver(h->ctx, i, &u);
        k += nw;
    }
}

static void receive_data(nm2_host_t *h, int i, uint16_t seq, const uint8_t *pl, int words, uint32_t now) {
    nm2_session_t *s = &h->s[i];
    if ((int16_t)(seq - s->rx_next) < 0) {  // already delivered: an FEC repeat or a duplicate datagram
        s->stats.rx_dups++;
        return;
    }
    for (int k = 0; k < s->n_hold; k++) {
        if (s->hold[k].seq == seq) {
            s->stats.rx_dups++;
            return;
        }
    }
    s->stats.rx_cmds++;
    if (seq == s->rx_next) {  // in order: straight from the datagram, whatever its length
        bool had_gap = s->n_hold > 0;
        deliver_words(h, i, pl, words);
        s->rx_next++;
        drain_hold(h, i);
        if (had_gap) s->stats.rx_recovered++;
        return;
    }
    if (words > NM2_RX_WORDS) return;  // too long to hold: it stays missing, and counts as lost if not resent
    // ahead of a gap
    if (s->n_hold == NM2_HOLD) {
        give_up(h, i);  // waited for too much: stop waiting
        int16_t d = (int16_t)(seq - s->rx_next);
        if (d < 0) return;  // it belonged in a gap just given up on: delivering it now would reorder
        if (d == 0) {
            deliver_words(h, i, pl, words);
            s->rx_next++;
            return;
        }
    }
    if (s->n_hold == 0) s->gap_since_ms = now, s->next_request_ms = now + RETRANSMIT_AFTER_MS;
    int at = 0;
    while (at < s->n_hold && (int16_t)(s->hold[at].seq - seq) < 0) at++;
    memmove(s->hold + at + 1, s->hold + at, (s->n_hold - at) * sizeof s->hold[0]);
    nm2_rxcmd_t *c = &s->hold[at];
    c->seq = seq;
    c->n = (uint8_t)words;
    for (int k = 0; k < words; k++) c->w[k] = get32(pl + 4 * k);
    s->n_hold++;
}

// ---- sending data --------------------------------------------------------------------------------------------

static nm2_cmd_t *hist_at(nm2_session_t *s, int k) { return &s->hist[(s->hist_start + k) % NM2_HISTORY]; }

static nm2_cmd_t *hist_push(nm2_session_t *s) {
    if (s->hist_count == NM2_HISTORY) s->hist_start = (s->hist_start + 1) % NM2_HISTORY, s->hist_count--;
    nm2_cmd_t *c = hist_at(s, s->hist_count++);
    c->seq = s->tx_seq++;
    c->n = 0;
    s->stats.tx_cmds++;
    return c;
}

// A datagram with the newest `fresh` commands from the history, preceded by up to NM2_FEC older ones.
static void send_latest(nm2_host_t *h, nm2_session_t *s, int fresh) {
    int first = s->hist_count - fresh - NM2_FEC;
    if (first < 0) first = 0;
    packet_t p;
    packet_start(&p);
    for (int k = first; k < s->hist_count; k++) packet_data(&p, hist_at(s, k));
    send_packet(h, &s->peer, &p);
}

bool nm2_host_send(nm2_host_t *h, int session, const ump_t *u, int count, uint32_t now) {
    if (session < 0 || session >= NM2_SESSIONS || !h->s[session].active) return false;
    nm2_session_t *s = &h->s[session];
    if (s->reset_pending) {  // the numbering restarts with the reply; a few ms of MIDI are lost
        s->stats.tx_dropped += (uint32_t)count;
        return false;
    }
    // A datagram holds (FEC + new) commands of at most NM2_CMD_WORDS words each; 16 new ones is well within 1400.
    int fresh = 0;
    nm2_cmd_t *c = NULL;
    for (int i = 0; i < count; i++) {
        int nw = ump_words(&u[i]);
        if (!c || c->n + nw > NM2_CMD_WORDS) {
            if (fresh == 16) send_latest(h, s, fresh), fresh = 0;
            c = hist_push(s);
            fresh++;
        }
        for (int j = 0; j < nw; j++) c->w[c->n++] = u[i].w[j];
    }
    if (fresh) send_latest(h, s, fresh);
    s->last_data_ms = now;
    s->idle_step = 0;
    return true;
}

static void retransmit(nm2_host_t *h, nm2_session_t *s, uint16_t seq) {
    int k = 0;
    while (k < s->hist_count && hist_at(s, k)->seq != seq) k++;
    if (k == s->hist_count) {  // too old (or never sent): Retransmit Error, with the first one we still have
        uint8_t pl[4] = {0};
        uint16_t first = s->hist_count ? hist_at(s, 0)->seq : s->tx_seq;
        pl[0] = first >> 8, pl[1] = first & 0xFF;
        send_one(h, &s->peer, CMD_RETRANSMIT_ERROR, 0x01 << 8, pl, 1);
        return;
    }
    packet_t p;
    packet_start(&p);
    for (; k < s->hist_count; k++) {
        if (!packet_data(&p, hist_at(s, k))) {
            send_packet(h, &s->peer, &p);
            packet_start(&p);
            packet_data(&p, hist_at(s, k));
        }
        s->stats.tx_retransmitted++;
    }
    send_packet(h, &s->peer, &p);
}

// ---- datagrams in --------------------------------------------------------------------------------------------

// The sequence number of the last UMP Data command in a datagram (the newest; the ones before it are FEC repeats).
static bool last_data_seq(const uint8_t *pkt, size_t len, uint16_t *seq) {
    bool found = false;
    for (size_t at = 4; at + 4 <= len;) {
        const uint8_t *c = pkt + at;
        at += 4 + 4 * (size_t)c[1];
        if (at > len) break;
        if (c[0] == CMD_UMP_DATA) *seq = (uint16_t)(c[2] << 8 | c[3]), found = true;
    }
    return found;
}

void nm2_host_receive(nm2_host_t *h, const nm2_addr_t *from, const uint8_t *pkt, size_t len, uint32_t now) {
    if (len < 8 || memcmp(pkt, "MIDI", 4)) return;
    bool told_not_established = false;
    for (size_t at = 4; at + 4 <= len;) {
        const uint8_t *c = pkt + at;
        uint8_t code = c[0];
        int words = c[1];
        uint16_t csd = (uint16_t)(c[2] << 8 | c[3]);
        const uint8_t *pl = c + 4;
        if (at + 4 + 4 * (size_t)words > len) break;  // truncated command
        at += 4 + 4 * (size_t)words;

        int i = find_session(h, from);
        if (i < 0 && (code == CMD_PING || code == CMD_UMP_DATA)) {
            // A client that still believes in a session from before a restart: take it back (see nm2.h).
            i = resume(h, from, "", now);
            uint16_t newest;
            // Only the newest command is delivered: the older ones in the datagram are repeats that may be stale.
            if (i >= 0 && last_data_seq(pkt, len, &newest)) h->s[i].rx_next = newest;
        }
        if (i >= 0) {
            h->s[i].last_rx_ms = now;
            h->s[i].missed_pings = 0;
            h->s[i].ping_sent_ms = 0;
        }
        bool needs_session = code == CMD_UMP_DATA || code == CMD_RETRANSMIT_REQUEST || code == CMD_RETRANSMIT_ERROR ||
                             code == CMD_SESSION_RESET;
        if (needs_session && i < 0) {
            if (!told_not_established) send_one(h, from, CMD_BYE, (uint16_t)BYE_NOT_ESTABLISHED << 8, NULL, 0);
            told_not_established = true;
            continue;
        }
        switch (code) {
            case CMD_INVITATION:
                invitation(h, from, c, pl, words, now);
                break;
            case CMD_PING:  // always answered, session or not (M2-124 8)
                send_one(h, from, CMD_PING_REPLY, 0, pl, words >= 1 ? 1 : 0);
                break;
            case CMD_PING_REPLY:
                break;  // any datagram from the client already counts as alive (above)
            case CMD_UMP_DATA:
                receive_data(h, i, csd, pl, words, now);
                break;
            case CMD_RETRANSMIT_REQUEST:
                retransmit(h, &h->s[i], csd);
                break;
            case CMD_RETRANSMIT_ERROR:  // what we wait for is gone
                give_up(h, i);
                break;
            case CMD_SESSION_RESET:
                reset_streams(&h->s[i], now);
                send_one(h, from, CMD_SESSION_RESET_REPLY, 0, NULL, 0);
                break;
            case CMD_BYE:
                send_one(h, from, CMD_BYE_REPLY, 0, NULL, 0);  // even with no session: the Bye may be a repeat
                if (i >= 0) end_session(h, i, 0, false);
                break;
            case CMD_SESSION_RESET_REPLY:
                if (i >= 0 && h->s[i].reset_pending) {
                    reset_streams(&h->s[i], now);  // both sides number from 0 again
                    h->s[i].reset_pending = false;
                }
                break;
            case CMD_BYE_REPLY:
            case CMD_NAK:
                break;
            default: {  // includes the authenticated invitations: we never ask for authentication
                uint8_t pl_nak[4];
                memcpy(pl_nak, c, 4);
                send_one(h, from, CMD_NAK, (uint16_t)NAK_NOT_SUPPORTED << 8, pl_nak, 1);
            }
        }
    }
}

// ---- time ----------------------------------------------------------------------------------------------------

void nm2_host_tick(nm2_host_t *h, uint32_t now) {
    for (int i = 0; i < NM2_SESSIONS; i++) {
        nm2_session_t *s = &h->s[i];
        if (!s->active) continue;

        if (s->reset_pending && now - s->reset_sent_ms >= RESET_EVERY_MS) {
            if (s->reset_tries >= RESET_TRIES) {
                end_session(h, i, 0, false);  // the client did not have a session after all: nothing to say
                continue;
            }
            send_reset(h, s, now);
        }

        if (s->n_hold) {
            if (now - s->gap_since_ms >= GIVE_UP_MS) {
                give_up(h, i);
            } else if ((int32_t)(now - s->next_request_ms) >= 0) {
                uint8_t pl[4] = {0};  // number of commands 0 = everything from rx_next on
                send_one(h, &s->peer, CMD_RETRANSMIT_REQUEST, s->rx_next, pl, 1);
                s->stats.retransmit_requests++;
                s->next_request_ms = now + RETRANSMIT_EVERY_MS;
            }
        }

        if (s->idle_step >= 0 && now - s->last_data_ms >= idle_after_ms[s->idle_step]) {
            hist_push(s);  // a zero-length UMP Data command, with the FEC repeats in front
            send_latest(h, s, 1);
            s->idle_step = s->idle_step + 1 < IDLE_STEPS ? s->idle_step + 1 : -1;
        }

        if (now - s->last_rx_ms >= PING_IDLE_MS && (!s->ping_sent_ms || now - s->ping_sent_ms >= PING_EVERY_MS)) {
            uint8_t pl[4];
            put32(pl, ++s->ping_id);
            send_one(h, &s->peer, CMD_PING, 0, pl, 1);
            s->ping_sent_ms = now ? now : 1;
            s->missed_pings++;
        }
    }
}
