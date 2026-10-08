#include <stdlib.h>
#include <string.h>

#include "airmidi/rtpmidi.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

// ---- a fake network between instances ------------------------------------------------------------------------
// Each peer has a control port `base` and a data port base + 1 on its own IP. What one sends waits in its outbox
// until pump() hands it over (or a test drops or reorders it).

typedef struct {
    int sock;
    rtpmidi_addr_t to;
    uint8_t b[RTPMIDI_MAX_PACKET];
    size_t n;
} dgram_t;

typedef struct {
    rtpmidi_t r;
    uint32_t ip;
    uint16_t base;
    dgram_t out[64];
    int n_out;
    ump_t got[2048];
    int n_got;
    int ups, downs;
    char peer[64];
} peer_t;

static peer_t box, mac, mac2, mac3;
static peer_t *const peers[] = {&box, &mac, &mac2, &mac3};
#define N_PEERS 4

static void on_send(void *ctx, int sock, const rtpmidi_addr_t *to, const uint8_t *data, size_t len) {
    peer_t *p = ctx;
    TEST_ASSERT_TRUE(p->n_out < 64);
    TEST_ASSERT_TRUE(len <= RTPMIDI_MAX_PACKET);
    dgram_t *d = &p->out[p->n_out++];
    d->sock = sock, d->to = *to, d->n = len;
    memcpy(d->b, data, len);
}

static void on_deliver(void *ctx, int session, const ump_t *u) {
    (void)session;
    peer_t *p = ctx;
    TEST_ASSERT_TRUE(p->n_got < 2048);
    p->got[p->n_got++] = *u;
}

static void on_session(void *ctx, int session, bool up, const char *peer) {
    (void)session;
    peer_t *p = ctx;
    up ? p->ups++ : p->downs++;
    strcpy(p->peer, peer);
}

static void peer_init(peer_t *p, const char *name, uint32_t ip, uint16_t base, uint32_t ssrc) {
    memset(p, 0, sizeof *p);
    p->ip = ip, p->base = base;
    rtpmidi_callbacks_t cb = {on_send, on_deliver, on_session};
    rtpmidi_init(&p->r, name, ssrc, ssrc * 7 + 1, &cb, p);
}

static void reset(void) {
    peer_init(&box, "airmidi-7530", 1, 5004, 0xB0B0B0B0);
    peer_init(&mac, "Mac", 2, 5100, 0x11111111);
    peer_init(&mac2, "Mac 2", 3, 5100, 0x22222222);
    peer_init(&mac3, "Mac 3", 4, 5100, 0x33333333);
}

static rtpmidi_addr_t ctrl_of(const peer_t *p) { return (rtpmidi_addr_t){p->ip, p->base}; }

static void hand_over(const peer_t *from, const dgram_t *d, uint32_t now) {
    for (int i = 0; i < N_PEERS; i++) {
        peer_t *to = peers[i];
        if (to->ip != d->to.ip) continue;
        rtpmidi_addr_t src = {from->ip, (uint16_t)(from->base + d->sock)};
        int sock = d->to.port == to->base ? RTPMIDI_CONTROL : RTPMIDI_DATA;
        rtpmidi_receive(&to->r, sock, &src, d->b, d->n, now);
    }
}

// Deliver everything in flight, and whatever that triggers, until the network is quiet.
static void pump(uint32_t now) {
    for (bool moved = true; moved;) {
        moved = false;
        for (int i = 0; i < N_PEERS; i++) {
            peer_t *p = peers[i];
            if (!p->n_out) continue;
            dgram_t q[64];
            int n = p->n_out;
            memcpy(q, p->out, sizeof q[0] * (size_t)n);
            p->n_out = 0;
            for (int k = 0; k < n; k++) hand_over(p, &q[k], now);
            moved = true;
        }
    }
}

static void connect(peer_t *client, uint32_t now) {
    rtpmidi_addr_t a = ctrl_of(&box);
    TEST_ASSERT_TRUE(rtpmidi_invite(&client->r, &a, now) >= 0);
    pump(now);
}

static void send_umps(peer_t *p, const ump_t *u, int n, uint32_t now) {
    TEST_ASSERT_TRUE(rtpmidi_send(&p->r, 0, u, n, now));
}

static void send_m1(peer_t *p, uint8_t s, uint8_t d1, uint8_t d2, uint32_t now) {
    ump_t u = ump_m1_cv(0, s, d1, d2);
    send_umps(p, &u, 1, now);
}

static uint16_t seq_of(const dgram_t *d) { return (uint16_t)(d->b[2] << 8 | d->b[3]); }

// ---- sessions ------------------------------------------------------------------------------------------------

static void test_client_invites_and_the_session_comes_up(void) {
    reset();
    rtpmidi_addr_t a = ctrl_of(&box);
    rtpmidi_invite(&mac.r, &a, 0);
    // IN on the control port: signature, "IN", version 2, token, SSRC, name.
    TEST_ASSERT_EQUAL(1, mac.n_out);
    TEST_ASSERT_EQUAL(RTPMIDI_CONTROL, mac.out[0].sock);
    const uint8_t head[] = {0xFF, 0xFF, 'I', 'N', 0, 0, 0, 2};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(head, mac.out[0].b, 8);
    TEST_ASSERT_EQUAL_HEX8_ARRAY("\x11\x11\x11\x11Mac", mac.out[0].b + 12, 8);
    pump(0);
    TEST_ASSERT_EQUAL(1, box.ups);
    TEST_ASSERT_EQUAL(1, mac.ups);
    TEST_ASSERT_EQUAL_STRING("Mac", box.peer);
    TEST_ASSERT_EQUAL_STRING("airmidi-7530", mac.peer);
    TEST_ASSERT_EQUAL(RTPMIDI_UP, box.r.s[0].state);
    TEST_ASSERT_EQUAL(5101, box.r.s[0].data.port);  // learnt from the data invitation's source
    TEST_ASSERT_FALSE(box.r.s[0].initiator);
}

static void test_third_client_is_turned_away(void) {
    reset();
    connect(&mac, 0);
    connect(&mac2, 0);
    rtpmidi_addr_t a = ctrl_of(&box);
    rtpmidi_invite(&mac3.r, &a, 0);
    pump(0);
    TEST_ASSERT_EQUAL(0, mac3.ups);
    TEST_ASSERT_EQUAL(RTPMIDI_IDLE, mac3.r.s[0].state);  // it got NO
    TEST_ASSERT_EQUAL(2, box.ups);
}

// A slot is never taken from a client that is still there; one silent for a minute makes room.
static void test_silent_session_gives_its_slot_away(void) {
    reset();
    connect(&mac, 0);
    connect(&mac2, 0);
    rtpmidi_addr_t a = ctrl_of(&box);
    rtpmidi_invite(&mac3.r, &a, 59000);
    pump(59000);
    TEST_ASSERT_EQUAL(0, mac3.ups);
    rtpmidi_invite(&mac3.r, &a, 60000);
    pump(60000);
    TEST_ASSERT_EQUAL(1, mac3.ups);
    TEST_ASSERT_EQUAL(1, box.downs);
}

// The inviter starts the clock sync at once: CK 0 with its time, the box answers CK 1 with the inviter's time
// echoed and its own added, the inviter closes with CK 2.
static void test_clock_sync(void) {
    reset();
    connect(&mac, 0);
    rtpmidi_tick(&mac.r, 7);
    TEST_ASSERT_EQUAL(1, mac.n_out);
    TEST_ASSERT_EQUAL(36, mac.out[0].n);
    TEST_ASSERT_EQUAL(RTPMIDI_DATA, mac.out[0].sock);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(
        "\xFF\xFF"
        "CK\x11\x11\x11\x11\x00",
        mac.out[0].b, 9);
    dgram_t ck0 = mac.out[0];
    mac.n_out = 0;
    hand_over(&mac, &ck0, 12);
    TEST_ASSERT_EQUAL(1, box.n_out);
    const uint8_t *b = box.out[0].b;
    TEST_ASSERT_EQUAL(1, b[8]);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(ck0.b + 12, b + 12, 8);  // ts1 echoed
    const uint8_t ts2[8] = {0, 0, 0, 0, 0, 0, 0, 120};    // 12 ms in 100 us units
    TEST_ASSERT_EQUAL_HEX8_ARRAY(ts2, b + 20, 8);
    pump(12);
    TEST_ASSERT_EQUAL(0, box.n_out);  // CK 2 needs no answer
    // six syncs 1.5 s apart (the first one above), then one every 10 s
    for (uint32_t t = 8; t < 30000; t++) rtpmidi_tick(&mac.r, t);
    TEST_ASSERT_EQUAL(5 + 2, mac.n_out);
}

static void test_bye_ends_the_session(void) {
    reset();
    connect(&mac, 0);
    rtpmidi_end(&mac.r, 0);
    pump(0);
    TEST_ASSERT_EQUAL(1, box.downs);
    TEST_ASSERT_EQUAL(1, mac.downs);
    TEST_ASSERT_EQUAL(RTPMIDI_IDLE, box.r.s[0].state);
}

// A client that invites again (it restarted its side) gets a fresh session in the same slot.
static void test_new_invitation_from_a_client_restarts_its_session(void) {
    reset();
    connect(&mac, 0);
    connect(&mac, 10);
    TEST_ASSERT_EQUAL(2, box.ups);
    TEST_ASSERT_EQUAL(1, box.downs);
    TEST_ASSERT_EQUAL(RTPMIDI_IDLE, box.r.s[1].state);
}

// The box invites a client it remembers (after a restart): one try a second, 12 in all, then it gives up quietly.
static void test_box_invites_and_retries(void) {
    reset();
    rtpmidi_addr_t m = ctrl_of(&mac);
    TEST_ASSERT_EQUAL(0, rtpmidi_invite(&box.r, &m, 0));
    for (uint32_t t = 0; t <= 15000; t += 10) rtpmidi_tick(&box.r, t);
    TEST_ASSERT_EQUAL(RTPMIDI_INVITE_TRIES, box.n_out);
    TEST_ASSERT_EQUAL(RTPMIDI_IDLE, box.r.s[0].state);
    TEST_ASSERT_EQUAL(0, box.ups);
    // With the client there, the session comes up and the box (the inviter now) starts the clock sync.
    box.n_out = 0;
    rtpmidi_invite(&box.r, &m, 20000);
    pump(20000);
    TEST_ASSERT_EQUAL(1, box.ups);
    TEST_ASSERT_EQUAL(1, mac.ups);
    TEST_ASSERT_EQUAL(5101, box.r.s[0].data.port);
    rtpmidi_tick(&box.r, 20001);
    TEST_ASSERT_EQUAL(1, box.n_out);
    TEST_ASSERT_EQUAL('C', box.out[0].b[2]);
}

// ---- MIDI ----------------------------------------------------------------------------------------------------

static void test_note_goes_out_in_one_packet_with_its_journal(void) {
    reset();
    connect(&mac, 0);
    send_m1(&box, 0x90, 60, 100, 5);
    TEST_ASSERT_EQUAL(1, box.n_out);
    const dgram_t *d = &box.out[0];
    TEST_ASSERT_EQUAL(RTPMIDI_DATA, d->sock);
    TEST_ASSERT_EQUAL(5101, d->to.port);
    TEST_ASSERT_EQUAL(12 + 2 + 3 + 3, d->n);
    TEST_ASSERT_EQUAL_HEX8(0x80, d->b[0]);  // version 2
    TEST_ASSERT_EQUAL_HEX8(0xE1, d->b[1]);  // M + payload type 97
    const uint8_t ts[4] = {0, 0, 0, 50}, ssrc[4] = {0xB0, 0xB0, 0xB0, 0xB0};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(ts, d->b + 4, 4);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(ssrc, d->b + 8, 4);
    const uint8_t rest[] = {0xC0, 0x03, 0x90, 60, 100, 0x80, d->b[2], d->b[3]};  // B J, 3 bytes; empty journal
    TEST_ASSERT_EQUAL_HEX8_ARRAY(rest, d->b + 12, sizeof rest);
    pump(5);
    TEST_ASSERT_EQUAL(1, mac.n_got);
    TEST_ASSERT_EQUAL_HEX32(0x20903C64, mac.got[0].w[0]);
}

// What a client may send: delta times (1 and 2 bytes), running status, a real-time message between commands.
static void test_client_packet_with_running_status_and_delta_times(void) {
    reset();
    connect(&mac, 0);
    const uint8_t pkt[] = {
        0x80, 0xE1, 0x12, 0x34, 0,    0,    0,  0,  0x11, 0x11, 0x11, 0x11,  // RTP header, mac's SSRC
        0x2E,                                                                // short header: Z, 14 bytes (no journal)
        0x00, 0x90, 60,   100,  0x81, 0x00, 62, 90, 0x00, 0xF8, 0x05, 0x80, 60, 64, 0x00};
    rtpmidi_addr_t from = {mac.ip, 5101};
    rtpmidi_receive(&box.r, RTPMIDI_DATA, &from, pkt, sizeof pkt, 1);
    TEST_ASSERT_EQUAL(4, box.n_got);
    TEST_ASSERT_EQUAL_HEX32(0x20903C64, box.got[0].w[0]);
    TEST_ASSERT_EQUAL_HEX32(0x20903E5A, box.got[1].w[0]);
    TEST_ASSERT_EQUAL_HEX32(0x10F80000, box.got[2].w[0]);
    TEST_ASSERT_EQUAL_HEX32(0x20803C40, box.got[3].w[0]);
    TEST_ASSERT_EQUAL(1, box.r.s[0].stats.rx_packets);
    TEST_ASSERT_EQUAL(0, box.r.s[0].stats.rx_bad);
}

// A SysEx cut by the end of a batch goes on in the next packet as segments (RFC 6295 figure 5): F0 ... F0, then
// F7 ... F7. The other side puts it back together.
static void test_sysex_across_packets(void) {
    reset();
    connect(&mac, 0);
    uint8_t data[24];
    for (int i = 0; i < 24; i++) data[i] = (uint8_t)i;
    ump_t u[4] = {ump_sysex7(0, UMP_SYSEX_START, data, 6), ump_sysex7(0, UMP_SYSEX_CONTINUE, data + 6, 6),
                  ump_sysex7(0, UMP_SYSEX_CONTINUE, data + 12, 6), ump_sysex7(0, UMP_SYSEX_END, data + 18, 6)};
    send_umps(&box, u, 2, 1);
    send_umps(&box, u + 2, 2, 2);
    TEST_ASSERT_EQUAL(2, box.n_out);
    const dgram_t *a = &box.out[0], *b = &box.out[1];
    TEST_ASSERT_EQUAL(14, a->b[13]);  // F0 + 12 + F0
    TEST_ASSERT_EQUAL_HEX8(0xF0, a->b[14]);
    TEST_ASSERT_EQUAL_HEX8(0xF0, a->b[14 + 13]);
    TEST_ASSERT_EQUAL(14, b->b[13]);  // F7 + 12 + F7
    TEST_ASSERT_EQUAL_HEX8(0xF7, b->b[14]);
    TEST_ASSERT_EQUAL_HEX8(0xF7, b->b[14 + 13]);
    pump(2);
    uint8_t buf[64];
    sysex7_rx_t rx;
    sysex7_rx_init(&rx, buf, sizeof buf);
    bool done = false;
    for (int i = 0; i < mac.n_got; i++) done = sysex7_rx_push(&rx, &mac.got[i]);
    TEST_ASSERT_TRUE(done);
    TEST_ASSERT_EQUAL(24, rx.len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(data, buf, 24);
}

// A SysEx whose middle was lost cannot be repaired (no system journal): the receiver ends it where the loss is
// found and skips the rest of it, rather than passing on a message with a hole in it. The next SysEx is whole.
static void test_sysex_with_a_lost_middle_is_cut_short(void) {
    reset();
    connect(&mac, 0);
    uint8_t data[18];
    for (int i = 0; i < 18; i++) data[i] = (uint8_t)(i + 1);
    ump_t u[3] = {ump_sysex7(0, UMP_SYSEX_START, data, 6), ump_sysex7(0, UMP_SYSEX_CONTINUE, data + 6, 6),
                  ump_sysex7(0, UMP_SYSEX_END, data + 12, 6)};
    send_umps(&box, u, 1, 1);
    pump(1);
    send_umps(&box, u + 1, 1, 2);
    box.n_out = 0;  // lost
    send_umps(&box, u + 2, 1, 3);
    ump_t whole = ump_sysex7(0, UMP_SYSEX_COMPLETE, data, 4);
    send_umps(&box, &whole, 1, 4);
    pump(4);
    uint8_t buf[64];
    sysex7_rx_t rx;
    sysex7_rx_init(&rx, buf, sizeof buf);
    size_t lens[4];
    int done = 0;
    for (int i = 0; i < mac.n_got; i++)
        if (sysex7_rx_push(&rx, &mac.got[i]) && done < 4) lens[done++] = rx.len;
    TEST_ASSERT_EQUAL(2, done);
    TEST_ASSERT_EQUAL(6, lens[0]);  // what came before the loss
    TEST_ASSERT_EQUAL(4, lens[1]);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(data, buf, 4);
}

// Only real-time may sit between SysEx segments: a clock inside a SysEx splits it there.
static void test_real_time_inside_sysex_splits_it(void) {
    reset();
    connect(&mac, 0);
    const uint8_t d1[3] = {1, 2, 3}, d2[2] = {4, 5};
    ump_t u[3] = {ump_sysex7(0, UMP_SYSEX_START, d1, 3), ump_system(0, 0xF8, 0, 0),
                  ump_sysex7(0, UMP_SYSEX_END, d2, 2)};
    send_umps(&box, u, 3, 1);
    const uint8_t list[] = {0xF0, 1, 2, 3, 0xF0, 0x00, 0xF8, 0x00, 0xF7, 4, 5, 0xF7};
    TEST_ASSERT_EQUAL(sizeof list, box.out[0].b[13]);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(list, box.out[0].b + 14, sizeof list);
    pump(1);
    uint8_t buf[16];
    sysex7_rx_t rx;
    sysex7_rx_init(&rx, buf, sizeof buf);
    int clocks = 0;
    bool done = false;
    for (int i = 0; i < mac.n_got; i++) {
        if (ump_mt(&mac.got[i]) == UMP_MT_SYSTEM) clocks++;
        if (sysex7_rx_push(&rx, &mac.got[i])) done = true;
    }
    TEST_ASSERT_EQUAL(1, clocks);
    TEST_ASSERT_TRUE(done);
    TEST_ASSERT_EQUAL(5, rx.len);
}

// MIDI 2.0 from the bus goes out as MIDI 1.0 (translated down, M2-104).
static void test_midi2_is_translated_down(void) {
    reset();
    connect(&mac, 0);
    ump_t u = ump_m2_cv(0, 0x9, 3, 64, 0, 0x80000000);  // note on 64, velocity 0x8000 of 16 bits
    send_umps(&box, &u, 1, 1);
    const uint8_t list[] = {0x93, 64, 64};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(list, box.out[0].b + 14, 3);
}

// The receiver's side of a lost packet: the next one's journal plays the lost NoteOff before its own commands.
static void test_lost_note_off_is_repaired_by_the_next_packet(void) {
    reset();
    connect(&mac, 0);
    send_m1(&box, 0x90, 60, 100, 1);
    pump(1);
    send_m1(&box, 0x80, 60, 0, 2);
    box.n_out = 0;  // lost
    send_m1(&box, 0xB0, 7, 99, 3);
    pump(3);
    TEST_ASSERT_EQUAL(3, mac.n_got);
    TEST_ASSERT_EQUAL_HEX32(0x20903C64, mac.got[0].w[0]);
    TEST_ASSERT_EQUAL_HEX32(0x20803C40, mac.got[1].w[0]);  // repaired (release velocity 64)
    TEST_ASSERT_EQUAL_HEX32(0x20B00763, mac.got[2].w[0]);
    TEST_ASSERT_EQUAL(1, mac.r.s[0].stats.rx_lost);
    TEST_ASSERT_EQUAL(1, mac.r.s[0].stats.rx_repairs);
}

// Late or repeated packets are dropped: what they held was already repaired from the journal of the packet that
// overtook them, and delivering it again would play it twice.
static void test_late_and_duplicate_packets_are_dropped(void) {
    reset();
    connect(&mac, 0);
    send_m1(&box, 0x90, 60, 100, 1);
    send_m1(&box, 0x90, 61, 100, 1);
    send_m1(&box, 0x90, 62, 100, 1);
    dgram_t p1 = box.out[0], p2 = box.out[1], p3 = box.out[2];
    box.n_out = 0;
    hand_over(&box, &p1, 1);
    hand_over(&box, &p3, 1);
    hand_over(&box, &p2, 1);
    hand_over(&box, &p3, 1);
    TEST_ASSERT_EQUAL(3, mac.n_got);  // 60, 61 (repaired), 62
    TEST_ASSERT_EQUAL_HEX32(0x20903D64, mac.got[1].w[0]);
    TEST_ASSERT_EQUAL(2, mac.r.s[0].stats.rx_late);
    TEST_ASSERT_EQUAL(1, mac.r.s[0].stats.rx_lost);
    TEST_ASSERT_TRUE(seq_of(&p2) == (uint16_t)(seq_of(&p1) + 1));
}

// The receiver reports what it has (RS, at most every 250 ms); the sender's journal then starts after that.
static void test_feedback_empties_the_journal(void) {
    reset();
    connect(&mac, 0);
    send_m1(&box, 0x90, 60, 100, 1);
    pump(1);
    send_m1(&box, 0x80, 60, 0, 2);
    TEST_ASSERT_TRUE(box.out[0].n > 12 + 2 + 3 + 3);  // the journal has the note
    pump(2);
    rtpmidi_tick(&mac.r, 100);        // too soon after the session started
    TEST_ASSERT_EQUAL(1, mac.n_out);  // only its clock sync
    mac.n_out = 0;
    rtpmidi_tick(&mac.r, 260);
    TEST_ASSERT_EQUAL(1, mac.n_out);
    TEST_ASSERT_EQUAL('R', mac.out[0].b[2]);
    TEST_ASSERT_EQUAL(RTPMIDI_CONTROL, mac.out[0].sock);
    pump(260);
    send_m1(&box, 0xB0, 1, 1, 300);
    TEST_ASSERT_EQUAL(12 + 2 + 3 + 3, box.out[0].n);  // empty journal
}

// After the last MIDI, journal-only packets at 20, 100 and 500 ms: a lost last packet is repaired without waiting
// for the next note. None once the receiver has confirmed it all.
static void test_journal_only_packets_follow_the_last_data(void) {
    reset();
    connect(&mac, 0);
    send_m1(&box, 0x90, 60, 100, 1000);
    send_m1(&box, 0x80, 60, 0, 1000);
    box.n_out = 1;  // the note off is lost
    pump(1000);
    for (uint32_t t = 1000; t < 3000; t++) rtpmidi_tick(&box.r, t);
    TEST_ASSERT_EQUAL(3, box.n_out);
    for (int i = 0; i < 3; i++) {
        TEST_ASSERT_EQUAL_HEX8(0x61, box.out[i].b[1]);  // M = 0: no MIDI list
        TEST_ASSERT_EQUAL(0, box.out[i].b[13]);
    }
    pump(3000);
    TEST_ASSERT_EQUAL_HEX32(0x20803C40, mac.got[mac.n_got - 1].w[0]);  // repaired from the first of them

    // Confirmed in time: nothing to guard.
    send_m1(&box, 0x90, 61, 100, 5000);
    pump(5000);
    rtpmidi_tick(&mac.r, 5010);  // feedback
    pump(5010);
    for (uint32_t t = 5000; t < 7000; t++) rtpmidi_tick(&box.r, t);
    TEST_ASSERT_EQUAL(0, box.n_out);
}

// Two streams at once over a channel that loses 30% of the datagrams (MIDI, feedback, clock sync alike) and swaps
// some: at the end the box holds what the Mac sent and the Mac what the box sent (RFC 6295's mandate: no lasting
// damage from any loss). Notes, controllers, programs, bends, pressure; All Notes Off is left out (it makes some
// state stale on purpose, tested in test_rtp_journal).
static void random_message(uint8_t *s, uint8_t *d1, uint8_t *d2) {
    uint8_t ch = (uint8_t)(rand() % 3);
    switch (rand() % 6) {
        case 0:
        case 1:
            *s = 0x90 | ch, *d1 = (uint8_t)(48 + rand() % 12), *d2 = (uint8_t)(1 + rand() % 127);
            break;
        case 2:
            *s = 0x80 | ch, *d1 = (uint8_t)(48 + rand() % 12), *d2 = 0;
            break;
        case 3:
            *s = 0xB0 | ch, *d1 = (uint8_t)(1 + rand() % 5), *d2 = (uint8_t)(rand() % 128);
            break;
        case 4:
            *s = 0xE0 | ch, *d1 = (uint8_t)(rand() % 128), *d2 = (uint8_t)(rand() % 128);
            break;
        default:
            *s = 0xC0 | ch, *d1 = (uint8_t)(rand() % 128), *d2 = 0;
            break;
    }
}

static void lossy_pump(uint32_t now, int loss_pct) {
    static dgram_t held;
    static const peer_t *held_from;
    for (int i = 0; i < 2; i++) {
        peer_t *p = peers[i];
        dgram_t q[64];
        int n = p->n_out;
        memcpy(q, p->out, sizeof q[0] * (size_t)n);
        p->n_out = 0;
        for (int k = 0; k < n; k++) {
            int r = rand() % 100;
            if (r < loss_pct) continue;
            if (r < loss_pct + 5 && !held_from) {
                held = q[k], held_from = p;  // overtaken by the next datagram
                continue;
            }
            hand_over(p, &q[k], now);
            if (held_from) hand_over(held_from, &held, now), held_from = NULL;
        }
    }
}

static void test_lossy_network_both_ways(void) {
    reset();
    srand(11);
    connect(&mac, 0);
    rtpj_rx_t sent_by_box, sent_by_mac;
    rtpj_rx_init(&sent_by_box);
    rtpj_rx_init(&sent_by_mac);
    uint32_t t = 1;
    for (int i = 0; i < 4000; i++, t += 2) {
        peer_t *from = rand() % 2 ? &box : &mac;
        uint8_t s, d1, d2;
        random_message(&s, &d1, &d2);
        send_m1(from, s, d1, d2, t);
        rtpj_rx_track(from == &box ? &sent_by_box : &sent_by_mac, s, d1, d2);
        rtpmidi_tick(&box.r, t), rtpmidi_tick(&mac.r, t);
        lossy_pump(t, 30);
    }
    for (uint32_t end = t + 2000; t < end; t++) {  // the network heals; the journal-only packets get through
        rtpmidi_tick(&box.r, t), rtpmidi_tick(&mac.r, t);
        pump(t);
    }
    TEST_ASSERT_EQUAL(1, box.ups);  // nothing in the loss ended the session
    for (int ch = 0; ch < 3; ch++) {
        const rtpj_rx_chan_t *want[2] = {&sent_by_box.ch[ch], &sent_by_mac.ch[ch]};
        const rtpj_rx_chan_t *got[2] = {&mac.r.s[0].jrx.ch[ch], &box.r.s[0].jrx.ch[ch]};
        for (int k = 0; k < 2; k++) {
            TEST_ASSERT_EQUAL_HEX32_ARRAY(want[k]->notes, got[k]->notes, 4);
            TEST_ASSERT_EQUAL_HEX8_ARRAY(want[k]->ctl, got[k]->ctl, 128);
            TEST_ASSERT_EQUAL(want[k]->program, got[k]->program);
            TEST_ASSERT_EQUAL(want[k]->bend, got[k]->bend);
        }
    }
    TEST_ASSERT_TRUE(mac.r.s[0].stats.rx_lost > 100);
    TEST_ASSERT_TRUE(mac.r.s[0].stats.rx_repairs > 100);
    TEST_ASSERT_TRUE(box.r.s[0].stats.rx_repairs > 100);
    TEST_ASSERT_EQUAL(0, box.r.s[0].stats.journal_overflows);
}

// Anything at all on either port: no crash, no read past the datagram (ASan), no session from garbage.
static void test_random_datagrams(void) {
    reset();
    connect(&mac, 0);
    srand(5);
    rtpmidi_addr_t from = {mac.ip, 5101};
    for (int round = 0; round < 50000; round++) {
        size_t len = (size_t)(rand() % 80);
        uint8_t *b = malloc(len ? len : 1);
        for (size_t i = 0; i < len; i++) b[i] = (uint8_t)rand();
        if (len > 12 && rand() % 2) {  // half of them look like RTP from the Mac, to get past the header checks
            b[0] = 0x80, b[1] = 0x61;
            memcpy(b + 8, "\x11\x11\x11\x11", 4);
        }
        rtpmidi_receive(&box.r, rand() % 2, &from, b, len, 1);
        free(b);
        box.n_out = 0;
        box.n_got = 0;
    }
    TEST_ASSERT_EQUAL(RTPMIDI_UP, box.r.s[0].state);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_client_invites_and_the_session_comes_up);
    RUN_TEST(test_third_client_is_turned_away);
    RUN_TEST(test_silent_session_gives_its_slot_away);
    RUN_TEST(test_clock_sync);
    RUN_TEST(test_bye_ends_the_session);
    RUN_TEST(test_new_invitation_from_a_client_restarts_its_session);
    RUN_TEST(test_box_invites_and_retries);
    RUN_TEST(test_note_goes_out_in_one_packet_with_its_journal);
    RUN_TEST(test_client_packet_with_running_status_and_delta_times);
    RUN_TEST(test_sysex_across_packets);
    RUN_TEST(test_sysex_with_a_lost_middle_is_cut_short);
    RUN_TEST(test_real_time_inside_sysex_splits_it);
    RUN_TEST(test_midi2_is_translated_down);
    RUN_TEST(test_lost_note_off_is_repaired_by_the_next_packet);
    RUN_TEST(test_late_and_duplicate_packets_are_dropped);
    RUN_TEST(test_feedback_empties_the_journal);
    RUN_TEST(test_journal_only_packets_follow_the_last_data);
    RUN_TEST(test_lossy_network_both_ways);
    RUN_TEST(test_random_datagrams);
    return UNITY_END();
}
