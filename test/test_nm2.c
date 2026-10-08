#include <stdlib.h>
#include <string.h>

#include "airmidi/nm2.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

// ---- a fake network ------------------------------------------------------------------------------------------

typedef struct {
    nm2_addr_t to;
    uint8_t b[NM2_MAX_PACKET];
    size_t n;
} sent_t;

static sent_t sent[256];
static int n_sent;
static uint32_t delivered[4096];  // word 0 of each delivered UMP
static int n_delivered;
static int ups, downs;
static nm2_host_t host;
static const nm2_addr_t A = {0x0A000001, 5000}, B = {0x0A000002, 5000}, C = {0x0A000003, 5000};

static void on_send(void *ctx, const nm2_addr_t *to, const uint8_t *data, size_t len) {
    (void)ctx;
    TEST_ASSERT_TRUE(n_sent < 256);
    TEST_ASSERT_TRUE(len <= NM2_MAX_PACKET);
    sent[n_sent].to = *to;
    memcpy(sent[n_sent].b, data, len);
    sent[n_sent++].n = len;
}

static void on_deliver(void *ctx, int session, const ump_t *u) {
    (void)ctx, (void)session;
    TEST_ASSERT_TRUE(n_delivered < 4096);
    delivered[n_delivered++] = u->w[0];
}

static void on_session(void *ctx, int session, bool up, const char *peer) {
    (void)ctx, (void)session, (void)peer;
    up ? ups++ : downs++;
}

static void reset(void) {
    nm2_callbacks_t cb = {on_send, on_deliver, on_session};
    nm2_host_init(&host, "airmidi-7530", "10BDA38F7530", &cb, NULL);
    n_sent = n_delivered = ups = downs = 0;
}

// ---- building client datagrams -------------------------------------------------------------------------------

typedef struct {
    uint8_t b[NM2_MAX_PACKET];
    size_t n;
} dgram_t;

static void start(dgram_t *d) {
    memcpy(d->b, "MIDI", 4);
    d->n = 4;
}

static void cmd(dgram_t *d, uint8_t code, uint16_t csd, const uint32_t *words, int nw) {
    d->b[d->n] = code, d->b[d->n + 1] = (uint8_t)nw, d->b[d->n + 2] = csd >> 8, d->b[d->n + 3] = csd & 0xFF;
    d->n += 4;
    for (int i = 0; i < nw; i++) {
        d->b[d->n++] = words[i] >> 24, d->b[d->n++] = words[i] >> 16, d->b[d->n++] = words[i] >> 8;
        d->b[d->n++] = words[i];
    }
}

static void deliver_to_host(const nm2_addr_t *from, const dgram_t *d, uint32_t now) {
    nm2_host_receive(&host, from, d->b, d->n, now);
}

static void invite(const nm2_addr_t *from, uint32_t now) {
    dgram_t d;
    start(&d);
    uint32_t name[2] = {0x4D616300, 0};  // "Mac"
    cmd(&d, 0x01, 1 << 8, name, 2);      // name 1 word, then an (empty) product instance id word
    deliver_to_host(from, &d, now);
}

// A note on whose data byte carries the sequence number, so order is visible in delivered[].
static uint32_t note(int i) { return 0x20900000u | (uint32_t)(i % 128) << 8 | (uint32_t)(i / 128 % 128); }

static void data(const nm2_addr_t *from, uint16_t seq, int i, uint32_t now) {
    dgram_t d;
    start(&d);
    uint32_t w = note(i);
    cmd(&d, 0xFF, seq, &w, 1);
    deliver_to_host(from, &d, now);
}

static uint8_t code_of(const sent_t *s, int k) {  // code of the k-th command in a sent datagram
    size_t at = 4;
    for (int i = 0; i < k; i++) at += 4 + 4 * (size_t)s->b[at + 1];
    return s->b[at];
}

static int commands_in(const sent_t *s) {
    int n = 0;
    for (size_t at = 4; at + 4 <= s->n; at += 4 + 4 * (size_t)s->b[at + 1]) n++;
    return n;
}

static uint16_t seq_of(const sent_t *s, int k) {
    size_t at = 4;
    for (int i = 0; i < k; i++) at += 4 + 4 * (size_t)s->b[at + 1];
    return (uint16_t)(s->b[at + 2] << 8 | s->b[at + 3]);
}

// ---- sessions ------------------------------------------------------------------------------------------------

static void test_invitation_is_accepted_with_name_and_id(void) {
    reset();
    invite(&A, 0);
    TEST_ASSERT_EQUAL(1, ups);
    TEST_ASSERT_EQUAL(1, n_sent);
    TEST_ASSERT_EQUAL_HEX8(0x10, code_of(&sent[0], 0));
    TEST_ASSERT_EQUAL(3, sent[0].b[6]);      // name: "airmidi-7530" = 3 words
    TEST_ASSERT_EQUAL(3 + 3, sent[0].b[5]);  // + product instance id, 3 words
    TEST_ASSERT_EQUAL_MEMORY("airmidi-7530", sent[0].b + 8, 12);
    TEST_ASSERT_EQUAL_MEMORY("10BDA38F7530", sent[0].b + 20, 12);
    TEST_ASSERT_EQUAL_STRING("Mac", host.s[0].peer_name);
    invite(&A, 10);  // repeated: accepted again, still one session
    TEST_ASSERT_EQUAL(1, ups);
    TEST_ASSERT_EQUAL(2, n_sent);
    TEST_ASSERT_FALSE(host.s[1].active);
}

static void test_third_client_is_turned_away(void) {
    reset();
    invite(&A, 0), invite(&B, 0), invite(&C, 0);
    TEST_ASSERT_EQUAL(2, ups);
    TEST_ASSERT_EQUAL_HEX8(0xF0, code_of(&sent[2], 0));
    TEST_ASSERT_EQUAL_HEX8(0x40, sent[2].b[6]);  // too many sessions
}

// ---- resuming after a restart (nm2.h): macOS never invites again, so a client that still believes in a session
// from before the restart is taken back with Session Reset instead of being told Bye

static int count_code(uint8_t code) {
    int n = 0;
    for (int i = 0; i < n_sent; i++)
        for (int k = 0; k < commands_in(&sent[i]); k++) n += code_of(&sent[i], k) == code;
    return n;
}

static void reset_reply(const nm2_addr_t *from, uint32_t now) {
    dgram_t d;
    start(&d);
    cmd(&d, 0x83, 0, NULL, 0);
    deliver_to_host(from, &d, now);
}

static void test_ping_without_a_session_resumes_it(void) {
    reset();
    dgram_t d;
    start(&d);
    uint32_t id = 0x12345678;
    cmd(&d, 0x20, 0, &id, 1);
    deliver_to_host(&C, &d, 0);
    TEST_ASSERT_EQUAL(1, ups);
    TEST_ASSERT_TRUE(host.s[0].resumed);
    TEST_ASSERT_EQUAL(2, n_sent);
    TEST_ASSERT_EQUAL_HEX8(0x82, code_of(&sent[0], 0));  // Session Reset
    TEST_ASSERT_EQUAL_HEX8(0x21, code_of(&sent[1], 0));  // and the ping is answered
    TEST_ASSERT_EQUAL_HEX8(0x12, sent[1].b[8]);
    TEST_ASSERT_EQUAL(0, count_code(0xF0));  // never a Bye
}

static void test_data_without_a_session_resumes_it_with_only_the_newest_command(void) {
    reset();
    dgram_t d;
    start(&d);
    uint32_t w1 = note(1), w2 = note(2), w3 = note(3);
    cmd(&d, 0xFF, 700, &w1, 1);  // FEC repeats from before the restart: may be stale
    cmd(&d, 0xFF, 701, &w2, 1);
    cmd(&d, 0xFF, 702, &w3, 1);  // the new one
    deliver_to_host(&A, &d, 0);
    TEST_ASSERT_EQUAL(1, ups);
    TEST_ASSERT_EQUAL(1, n_delivered);
    TEST_ASSERT_EQUAL_HEX32(note(3), delivered[0]);
    TEST_ASSERT_EQUAL(1, count_code(0x82));
    data(&A, 703, 4, 5);  // still the old numbering until the client has seen the reset
    TEST_ASSERT_EQUAL(2, n_delivered);
    reset_reply(&A, 10);
    data(&A, 0, 5, 20);  // both sides start again at 0
    TEST_ASSERT_EQUAL(3, n_delivered);
    TEST_ASSERT_EQUAL_HEX32(note(5), delivered[2]);
    TEST_ASSERT_EQUAL(0, host.s[0].stats.rx_lost);
}

static void test_nothing_is_sent_until_the_reset_is_answered(void) {
    reset();
    nm2_host_resume(&host, &A, "Mac", 0);
    TEST_ASSERT_EQUAL_STRING("Mac", host.s[0].peer_name);
    TEST_ASSERT_EQUAL(1, ups);
    ump_t u = {{note(1)}};
    TEST_ASSERT_FALSE(nm2_host_send(&host, 0, &u, 1, 5));
    TEST_ASSERT_EQUAL(1, host.s[0].stats.tx_dropped);
    TEST_ASSERT_EQUAL(0, count_code(0xFF));
    reset_reply(&A, 10);
    TEST_ASSERT_TRUE(nm2_host_send(&host, 0, &u, 1, 15));
    TEST_ASSERT_EQUAL_HEX8(0xFF, code_of(&sent[n_sent - 1], 0));
    TEST_ASSERT_EQUAL(0, seq_of(&sent[n_sent - 1], 0));  // numbering from 0
}

static void test_unanswered_reset_ends_the_session_quietly(void) {
    reset();
    nm2_host_resume(&host, &A, "Mac", 0);  // e.g. the Mac rebooted meanwhile and forgot us
    for (uint32_t t = 0; t <= 2000; t += 10) nm2_host_tick(&host, t);
    TEST_ASSERT_FALSE(host.s[0].active);
    TEST_ASSERT_EQUAL(1, downs);
    TEST_ASSERT_EQUAL(5, count_code(0x82));  // repeated, then given up
    TEST_ASSERT_EQUAL(0, count_code(0xF0));  // no Bye: it never had the session
}

static void test_bye_in_answer_to_a_reset_ends_it(void) {
    reset();
    nm2_host_resume(&host, &A, "Mac", 0);
    dgram_t d;
    start(&d);
    cmd(&d, 0xF0, 0x05 << 8, NULL, 0);  // Session Not Established
    deliver_to_host(&A, &d, 10);
    TEST_ASSERT_FALSE(host.s[0].active);
    TEST_ASSERT_EQUAL_HEX8(0xF1, code_of(&sent[n_sent - 1], 0));
}

static void test_invitation_after_a_resume_is_a_new_session(void) {
    reset();
    data(&A, 900, 1, 0);
    TEST_ASSERT_TRUE(host.s[0].resumed);
    invite(&A, 10);
    TEST_ASSERT_TRUE(host.s[0].active);
    TEST_ASSERT_FALSE(host.s[0].resumed);
    TEST_ASSERT_FALSE(host.s[0].reset_pending);
    TEST_ASSERT_EQUAL_STRING("Mac", host.s[0].peer_name);
    TEST_ASSERT_EQUAL(2, ups);
    TEST_ASSERT_EQUAL(1, downs);
    data(&A, 0, 2, 20);  // a new session numbers from 0
    TEST_ASSERT_EQUAL_HEX32(note(2), delivered[n_delivered - 1]);
}

static void test_no_free_slot_still_gets_bye(void) {
    reset();
    invite(&A, 0), invite(&B, 0);
    data(&C, 0, 0, 10);  // both slots heard from recently: nothing to take back into
    TEST_ASSERT_EQUAL(2, ups);
    TEST_ASSERT_EQUAL(0, n_delivered);
    TEST_ASSERT_EQUAL_HEX8(0xF0, code_of(&sent[n_sent - 1], 0));
    TEST_ASSERT_EQUAL_HEX8(0x05, sent[n_sent - 1].b[6]);
}

static void test_unknown_and_authenticated_commands_are_naked(void) {
    reset();
    invite(&A, 0);
    dgram_t d;
    start(&d);
    cmd(&d, 0x02, 0, NULL, 0);
    cmd(&d, 0x55, 0, NULL, 0);
    deliver_to_host(&A, &d, 0);
    TEST_ASSERT_EQUAL(3, n_sent);
    TEST_ASSERT_EQUAL_HEX8(0x8F, code_of(&sent[1], 0));
    TEST_ASSERT_EQUAL_HEX8(0x01, sent[1].b[6]);
    TEST_ASSERT_EQUAL_HEX8(0x02, sent[1].b[8]);  // the NAKed header comes back
    TEST_ASSERT_EQUAL_HEX8(0x55, sent[2].b[8]);
}

static void test_bye_ends_the_session(void) {
    reset();
    invite(&A, 0);
    dgram_t d;
    start(&d);
    cmd(&d, 0xF0, 0x01 << 8, NULL, 0);
    deliver_to_host(&A, &d, 0);
    TEST_ASSERT_EQUAL(1, downs);
    TEST_ASSERT_FALSE(host.s[0].active);
    TEST_ASSERT_EQUAL_HEX8(0xF1, code_of(&sent[1], 0));
}

static void test_silent_client_is_pinged_but_kept(void) {
    reset();
    invite(&A, 0);
    for (uint32_t t = 0; t < 100000; t += 10) nm2_host_tick(&host, t);
    TEST_ASSERT_EQUAL(0, downs);  // unanswered pings are not a reason to drop (macOS does not answer them)
    int pings = 0;
    for (int i = 0; i < n_sent; i++) pings += code_of(&sent[i], 0) == 0x20;
    TEST_ASSERT_EQUAL(9, pings);  // 10, 20, ... 90 s
}

static void test_silent_client_gives_way_to_a_new_one(void) {
    reset();
    invite(&A, 0), invite(&B, 0);
    invite(&C, 30000);  // both slots busy, nobody silent for a minute yet
    TEST_ASSERT_EQUAL(2, ups);
    TEST_ASSERT_EQUAL_HEX8(0x40, sent[n_sent - 1].b[6]);
    data(&B, 0, 0, 50000);  // B keeps talking; A has been silent since 0
    invite(&C, 61000);
    TEST_ASSERT_EQUAL(3, ups);
    TEST_ASSERT_EQUAL(1, downs);
    TEST_ASSERT_TRUE(host.s[0].active);
    TEST_ASSERT_EQUAL(C.ip, host.s[0].peer.ip);  // A's slot
}

static void test_ping_reply_keeps_the_session(void) {
    reset();
    invite(&A, 0);
    for (uint32_t t = 0; t < 60000; t += 10) {
        nm2_host_tick(&host, t);
        if (n_sent && code_of(&sent[n_sent - 1], 0) == 0x20) {
            dgram_t d;
            start(&d);
            uint32_t id = host.s[0].ping_id;
            cmd(&d, 0x21, 0, &id, 1);
            deliver_to_host(&A, &d, t + 5);
            n_sent = 0;
        }
    }
    TEST_ASSERT_EQUAL(0, downs);
}

// ---- receiving -----------------------------------------------------------------------------------------------

static void test_in_order_and_fec_duplicates(void) {
    reset();
    invite(&A, 0);
    dgram_t d;
    for (int i = 0; i < 5; i++) {  // like a real sender: each datagram repeats the two previous commands
        start(&d);
        for (int k = i - 2; k <= i; k++) {
            if (k < 0) continue;
            uint32_t w = note(k);
            cmd(&d, 0xFF, (uint16_t)k, &w, 1);
        }
        deliver_to_host(&A, &d, i);
    }
    TEST_ASSERT_EQUAL(5, n_delivered);
    for (int i = 0; i < 5; i++) TEST_ASSERT_EQUAL_HEX32(note(i), delivered[i]);
    TEST_ASSERT_EQUAL(7, host.s[0].stats.rx_dups);
}

static void test_fec_fills_a_lost_datagram(void) {
    reset();
    invite(&A, 0);
    data(&A, 0, 0, 0);
    // datagram with seq 1 is lost; the next one carries it as FEC
    dgram_t d;
    start(&d);
    uint32_t w1 = note(1), w2 = note(2);
    cmd(&d, 0xFF, 1, &w1, 1);
    cmd(&d, 0xFF, 2, &w2, 1);
    deliver_to_host(&A, &d, 5);
    TEST_ASSERT_EQUAL(3, n_delivered);
    TEST_ASSERT_EQUAL(0, host.s[0].stats.retransmit_requests);
}

static void test_gap_is_held_then_filled_by_retransmit(void) {
    reset();
    invite(&A, 0);
    n_sent = 0;
    data(&A, 0, 0, 0);
    data(&A, 2, 2, 1);  // 1 is missing: 2 must wait
    TEST_ASSERT_EQUAL(1, n_delivered);
    nm2_host_tick(&host, 5);
    TEST_ASSERT_EQUAL(0, n_sent);  // not yet: reordered datagrams get 10 ms
    nm2_host_tick(&host, 11);
    TEST_ASSERT_EQUAL(1, n_sent);
    TEST_ASSERT_EQUAL_HEX8(0x80, code_of(&sent[0], 0));
    TEST_ASSERT_EQUAL(1, seq_of(&sent[0], 0));
    data(&A, 1, 1, 20);
    TEST_ASSERT_EQUAL(3, n_delivered);
    TEST_ASSERT_EQUAL_HEX32(note(1), delivered[1]);
    TEST_ASSERT_EQUAL_HEX32(note(2), delivered[2]);
    TEST_ASSERT_EQUAL(1, host.s[0].stats.rx_recovered);
    TEST_ASSERT_EQUAL(0, host.s[0].stats.rx_lost);
}

static void test_gap_given_up_after_150_ms(void) {
    reset();
    invite(&A, 0);
    data(&A, 0, 0, 0);
    data(&A, 2, 2, 1);
    for (uint32_t t = 2; t <= 150; t++) nm2_host_tick(&host, t);
    TEST_ASSERT_EQUAL(1, n_delivered);  // still waiting
    nm2_host_tick(&host, 151);
    TEST_ASSERT_EQUAL(2, n_delivered);
    TEST_ASSERT_EQUAL_HEX32(note(2), delivered[1]);
    TEST_ASSERT_EQUAL(1, host.s[0].stats.rx_lost);
    data(&A, 1, 1, 170);  // too late: delivering it now would put it after 2
    TEST_ASSERT_EQUAL(2, n_delivered);
}

static void test_retransmit_error_ends_the_wait(void) {
    reset();
    invite(&A, 0);
    data(&A, 0, 0, 0);
    data(&A, 2, 2, 1);
    dgram_t d;
    start(&d);
    uint32_t pl = 0;
    cmd(&d, 0x81, 0x01 << 8, &pl, 1);
    deliver_to_host(&A, &d, 3);
    TEST_ASSERT_EQUAL(2, n_delivered);
}

static void test_hold_overflow_gives_up(void) {
    reset();
    invite(&A, 0);
    data(&A, 0, 0, 0);
    for (int i = 2; i <= 2 + NM2_HOLD; i++) data(&A, (uint16_t)i, i, 1);
    TEST_ASSERT_EQUAL(1 + NM2_HOLD + 1, n_delivered);
    for (int i = 1; i < n_delivered; i++) TEST_ASSERT_EQUAL_HEX32(note(i + 1), delivered[i]);
    TEST_ASSERT_EQUAL(1, host.s[0].stats.rx_lost);
}

static void test_sequence_wraps(void) {
    reset();
    invite(&A, 0);
    host.s[0].rx_next = 0xFFFE;  // as if 65534 commands had come before
    data(&A, 0xFFFE, 0, 0);
    data(&A, 0xFFFF, 1, 0);
    data(&A, 0x0000, 2, 0);
    data(&A, 0xFFFF, 1, 0);  // late duplicate across the wrap
    TEST_ASSERT_EQUAL(3, n_delivered);
    TEST_ASSERT_EQUAL(1, host.s[0].stats.rx_dups);
}

static void test_session_reset_restarts_sequences(void) {
    reset();
    invite(&A, 0);
    data(&A, 0, 0, 0);
    dgram_t d;
    start(&d);
    cmd(&d, 0x82, 0, NULL, 0);
    deliver_to_host(&A, &d, 1);
    TEST_ASSERT_EQUAL_HEX8(0x83, code_of(&sent[n_sent - 1], 0));
    data(&A, 0, 1, 2);  // a duplicate without the reset
    TEST_ASSERT_EQUAL(2, n_delivered);
}

static void test_long_command_in_order_is_delivered_whole(void) {
    reset();
    invite(&A, 0);
    dgram_t d;
    start(&d);
    uint32_t w[64];
    for (int i = 0; i < 64; i++) w[i] = 0x20B00000u | (uint32_t)i << 8;  // 64 one-word UMPs
    cmd(&d, 0xFF, 0, w, 64);
    deliver_to_host(&A, &d, 0);
    TEST_ASSERT_EQUAL(64, n_delivered);
    TEST_ASSERT_EQUAL_HEX32(0x20B03F00, delivered[63]);
}

static void test_multiword_umps_are_split_correctly(void) {
    reset();
    invite(&A, 0);
    dgram_t d;
    start(&d);
    uint32_t w[7] = {0x40903C00, 0xFFFF0000, 0x20B00740, 0xF0000101, 0x1F, 0, 0};  // MT4 (2) + MT2 (1) + MTF (4)
    cmd(&d, 0xFF, 0, w, 7);
    deliver_to_host(&A, &d, 0);
    TEST_ASSERT_EQUAL(3, n_delivered);
    TEST_ASSERT_EQUAL_HEX32(0x40903C00, delivered[0]);
    TEST_ASSERT_EQUAL_HEX32(0x20B00740, delivered[1]);
    TEST_ASSERT_EQUAL_HEX32(0xF0000101, delivered[2]);
}

// ---- sending -------------------------------------------------------------------------------------------------

static void test_send_carries_two_fec_repeats(void) {
    reset();
    invite(&A, 0);
    n_sent = 0;
    for (int i = 0; i < 4; i++) {
        ump_t u = ump_m1_cv(0, 0x90, (uint8_t)i, 100);
        TEST_ASSERT_TRUE(nm2_host_send(&host, 0, &u, 1, (uint32_t)i));
    }
    TEST_ASSERT_EQUAL(4, n_sent);
    TEST_ASSERT_EQUAL(1, commands_in(&sent[0]));
    TEST_ASSERT_EQUAL(2, commands_in(&sent[1]));
    TEST_ASSERT_EQUAL(3, commands_in(&sent[3]));
    TEST_ASSERT_EQUAL(1, seq_of(&sent[3], 0));  // previous two, oldest first, then the new one
    TEST_ASSERT_EQUAL(2, seq_of(&sent[3], 1));
    TEST_ASSERT_EQUAL(3, seq_of(&sent[3], 2));
    TEST_ASSERT_FALSE(nm2_host_send(&host, 1, NULL, 0, 0));  // no session there
}

static void test_many_umps_split_into_commands(void) {
    reset();
    invite(&A, 0);
    n_sent = 0;
    ump_t u[40];
    for (int i = 0; i < 40; i++) u[i] = ump_m2_cv(0, 0x9, 0, (uint8_t)i, 0, 0xFFFF0000);  // 2 words each
    nm2_host_send(&host, 0, u, 40, 0);
    TEST_ASSERT_EQUAL(1, n_sent);
    TEST_ASSERT_EQUAL(5, commands_in(&sent[0]));  // 80 words in commands of 16
}

static void test_idle_zero_length_commands(void) {
    reset();
    invite(&A, 0);
    ump_t u = ump_m1_cv(0, 0x90, 60, 100);
    nm2_host_send(&host, 0, &u, 1, 1000);
    n_sent = 0;
    for (uint32_t t = 1000; t <= 1045; t++) nm2_host_tick(&host, t);
    TEST_ASSERT_EQUAL(2, n_sent);                    // at +20 and +40 ms
    TEST_ASSERT_EQUAL(2, commands_in(&sent[0]));     // the real command again, then the empty one
    TEST_ASSERT_EQUAL(0, sent[0].b[4 + 4 + 4 + 1]);  // second command: zero words
    TEST_ASSERT_EQUAL(3, commands_in(&sent[1]));
    for (uint32_t t = 1046; t <= 1000 + 10000; t++) nm2_host_tick(&host, t);
    for (uint32_t t = 11001; t <= 25000; t += 5) nm2_host_tick(&host, t);
    int zero_len = 0;
    for (int i = 0; i < n_sent; i++) {
        zero_len += code_of(&sent[i], 0) == 0xFF;
    }
    TEST_ASSERT_EQUAL(6, zero_len);  // +20, +40, +300 ms, +1, +3, +10 s, then nothing (the rest are pings)
}

static void test_retransmit_on_request(void) {
    reset();
    invite(&A, 0);
    for (int i = 0; i < 40; i++) {
        ump_t u = ump_m1_cv(0, 0x90, (uint8_t)i, 100);
        nm2_host_send(&host, 0, &u, 1, 0);
    }
    n_sent = 0;
    dgram_t d;
    start(&d);
    uint32_t pl = 0;
    cmd(&d, 0x80, 30, &pl, 1);
    deliver_to_host(&A, &d, 1);
    TEST_ASSERT_EQUAL(1, n_sent);
    TEST_ASSERT_EQUAL(10, commands_in(&sent[0]));  // 30..39
    TEST_ASSERT_EQUAL(30, seq_of(&sent[0], 0));
    start(&d);
    cmd(&d, 0x80, 2, &pl, 1);  // fell out of the 32-command history
    deliver_to_host(&A, &d, 2);
    TEST_ASSERT_EQUAL_HEX8(0x81, code_of(&sent[1], 0));
    TEST_ASSERT_EQUAL(0, sent[1].b[8]);  // first still available: 8 (40 - 32)
    TEST_ASSERT_EQUAL(8, sent[1].b[9]);
}

// ---- a lossy network -----------------------------------------------------------------------------------------

// A client sending 2000 commands (with FEC 2) through a channel that drops, duplicates and reorders datagrams.
// It answers the host's retransmit requests from its own history. Whatever arrives must be in order, once; and
// with retransmits answered, nothing may be lost.
static void lossy_run(int drop_pct, int dup_pct, int swap_pct, bool answer_retransmits, unsigned seed) {
    reset();
    srand(seed);
    invite(&A, 0);
    dgram_t delayed;
    bool have_delayed = false;
    uint32_t t = 0;
    int next_retransmit_from = -1;
    for (int i = 0; i < 2000; i++, t += 3) {
        dgram_t d;
        start(&d);
        int from = next_retransmit_from >= 0 ? next_retransmit_from : (i - 2 < 0 ? 0 : i - 2);
        next_retransmit_from = -1;
        for (int k = from; k <= i; k++) {
            uint32_t w = note(k);
            cmd(&d, 0xFF, (uint16_t)k, &w, 1);
        }
        int r = rand() % 100;
        if (r < drop_pct) {
            // lost
        } else if (r < drop_pct + swap_pct && !have_delayed) {
            delayed = d, have_delayed = true;  // arrives after the next one
        } else {
            deliver_to_host(&A, &d, t);
            if (have_delayed) deliver_to_host(&A, &delayed, t), have_delayed = false;
            if (rand() % 100 < dup_pct) deliver_to_host(&A, &d, t);
        }
        n_sent = 0;
        nm2_host_tick(&host, t + 1);
        nm2_host_tick(&host, t + 2);
        for (int s = 0; s < n_sent; s++)
            if (answer_retransmits && code_of(&sent[s], 0) == 0x80) next_retransmit_from = seq_of(&sent[s], 0);
    }
    for (uint32_t end = t + 200; t < end; t++) nm2_host_tick(&host, t);
    // in order, once
    for (int i = 1; i < n_delivered; i++) {
        int a = (int)((delivered[i - 1] >> 8 & 0x7F) | (delivered[i - 1] & 0x7F) << 7);
        int b = (int)((delivered[i] >> 8 & 0x7F) | (delivered[i] & 0x7F) << 7);
        TEST_ASSERT_TRUE_MESSAGE(b > a, "out of order or duplicated");
    }
    // Everything up to the last command that arrived is either delivered or counted lost. (Commands lost after
    // that cannot be noticed by any receiver: nothing later arrives to show the gap.)
    uint32_t last = delivered[n_delivered - 1];
    int last_seq = (int)((last >> 8 & 0x7F) | (last & 0x7F) << 7);
    TEST_ASSERT_EQUAL(last_seq + 1, n_delivered + (int)host.s[0].stats.rx_lost);
}

static void test_lossy_network_fec_only(void) { lossy_run(10, 5, 5, false, 1); }

// The same bad channel twice: answering retransmit requests must recover most of what FEC alone loses (not all:
// a retransmission can be lost too).
static void test_lossy_network_with_retransmit(void) {
    lossy_run(30, 5, 10, false, 2);
    uint32_t without = host.s[0].stats.rx_lost;
    lossy_run(30, 5, 10, true, 2);
    uint32_t with = host.s[0].stats.rx_lost;
    TEST_ASSERT_TRUE(without >= 10);
    TEST_ASSERT_TRUE_MESSAGE(with * 4 <= without, "retransmit recovered less than 3/4 of the losses");
    TEST_ASSERT_TRUE(host.s[0].stats.rx_recovered > 0);
}
static void test_terrible_network_stays_in_order(void) { lossy_run(70, 20, 20, false, 3); }

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_invitation_is_accepted_with_name_and_id);
    RUN_TEST(test_third_client_is_turned_away);
    RUN_TEST(test_ping_without_a_session_resumes_it);
    RUN_TEST(test_data_without_a_session_resumes_it_with_only_the_newest_command);
    RUN_TEST(test_nothing_is_sent_until_the_reset_is_answered);
    RUN_TEST(test_unanswered_reset_ends_the_session_quietly);
    RUN_TEST(test_bye_in_answer_to_a_reset_ends_it);
    RUN_TEST(test_invitation_after_a_resume_is_a_new_session);
    RUN_TEST(test_no_free_slot_still_gets_bye);
    RUN_TEST(test_unknown_and_authenticated_commands_are_naked);
    RUN_TEST(test_bye_ends_the_session);
    RUN_TEST(test_silent_client_is_pinged_but_kept);
    RUN_TEST(test_silent_client_gives_way_to_a_new_one);
    RUN_TEST(test_ping_reply_keeps_the_session);
    RUN_TEST(test_in_order_and_fec_duplicates);
    RUN_TEST(test_fec_fills_a_lost_datagram);
    RUN_TEST(test_gap_is_held_then_filled_by_retransmit);
    RUN_TEST(test_gap_given_up_after_150_ms);
    RUN_TEST(test_retransmit_error_ends_the_wait);
    RUN_TEST(test_hold_overflow_gives_up);
    RUN_TEST(test_sequence_wraps);
    RUN_TEST(test_session_reset_restarts_sequences);
    RUN_TEST(test_long_command_in_order_is_delivered_whole);
    RUN_TEST(test_multiword_umps_are_split_correctly);
    RUN_TEST(test_send_carries_two_fec_repeats);
    RUN_TEST(test_many_umps_split_into_commands);
    RUN_TEST(test_idle_zero_length_commands);
    RUN_TEST(test_retransmit_on_request);
    RUN_TEST(test_lossy_network_fec_only);
    RUN_TEST(test_lossy_network_with_retransmit);
    RUN_TEST(test_terrible_network_stays_in_order);
    return UNITY_END();
}
