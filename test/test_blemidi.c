#include <stdlib.h>
#include <string.h>

#include "airmidi/blemidi.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

// ---- decode --------------------------------------------------------------------------------------------------

static uint8_t out[256];

#define DECODE(...) blemidi_decode((const uint8_t[]){__VA_ARGS__}, sizeof((const uint8_t[]){__VA_ARGS__}), out)

static void test_decode_one_message(void) {
    TEST_ASSERT_EQUAL(3, DECODE(0x80, 0x80, 0x90, 60, 100));
    const uint8_t want[] = {0x90, 60, 100};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, out, 3);
}

static void test_decode_several_messages_and_running_status(void) {
    // note on with ts, a second note in running status without ts, a CC with its own ts
    TEST_ASSERT_EQUAL(7, DECODE(0xA5, 0xC1, 0x90, 60, 100, 62, 100, 0xC2, 0xB0, 7));
    const uint8_t want[] = {0x90, 60, 100, 62, 100, 0xB0, 7};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, out, 7);
}

static void test_decode_running_status_with_timestamp(void) {
    TEST_ASSERT_EQUAL(5, DECODE(0x80, 0x81, 0x90, 60, 100, 0x82, 62, 100));
    const uint8_t want[] = {0x90, 60, 100, 62, 100};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, out, 5);
}

static void test_decode_sysex_across_packets_with_real_time(void) {
    TEST_ASSERT_EQUAL(4, DECODE(0x80, 0x80, 0xF0, 1, 2, 3));
    TEST_ASSERT_EQUAL_HEX8(0xF0, out[0]);
    TEST_ASSERT_EQUAL(5, DECODE(0x80, 4, 5, 0x81, 0xF8, 6, 0x82, 0xF7));  // continuation: header, data
    const uint8_t want[] = {4, 5, 0xF8, 6, 0xF7};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, out, 5);
}

static void test_decode_rejects_bad_headers(void) {
    TEST_ASSERT_EQUAL(-1, blemidi_decode(NULL, 0, out));
    TEST_ASSERT_EQUAL(-1, DECODE(0x40, 0x80, 0x90, 60, 100));  // bit 7 clear
    TEST_ASSERT_EQUAL(-1, DECODE(0xC0, 0x80, 0x90, 60, 100));  // reserved bit 6 set
    TEST_ASSERT_EQUAL(0, DECODE(0x80));                        // header only: valid, empty
}

// ---- encode --------------------------------------------------------------------------------------------------

static uint8_t pkts[64][BLEMIDI_MAX_PACKET];
static size_t pkt_len[64];
static int n_pkts;

static void collect(void *ctx, const uint8_t *p, size_t len) {
    (void)ctx;
    TEST_ASSERT_TRUE(n_pkts < 64);
    memcpy(pkts[n_pkts], p, len);
    pkt_len[n_pkts++] = len;
}

static blemidi_tx_t tx;

static void start(size_t max) {
    n_pkts = 0;
    blemidi_tx_init(&tx, max, collect, NULL);
}

static void test_encode_note_with_timestamp(void) {
    start(BLEMIDI_MAX_PACKET);
    ump_t u = ump_m1_cv(0, 0x90, 60, 100);
    blemidi_tx_add(&tx, &u, 0x1234);  // 13 bits: 0x1234 -> hi 0x24, lo 0x34
    TEST_ASSERT_EQUAL(0, n_pkts);     // nothing until the flush
    blemidi_tx_flush(&tx);
    TEST_ASSERT_EQUAL(1, n_pkts);
    const uint8_t want[] = {0x80 | 0x24, 0x80 | 0x34, 0x90, 60, 100};
    TEST_ASSERT_EQUAL_size_t(sizeof want, pkt_len[0]);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, pkts[0], sizeof want);
}

static void test_encode_batches_messages_and_repeats_status(void) {
    start(BLEMIDI_MAX_PACKET);
    ump_t a = ump_m1_cv(0, 0x90, 60, 100), b = ump_m1_cv(0, 0x90, 62, 100), c = ump_system(0, 0xF8, 0, 0);
    blemidi_tx_add(&tx, &a, 5);
    blemidi_tx_add(&tx, &b, 6);
    blemidi_tx_add(&tx, &c, 6);
    blemidi_tx_flush(&tx);
    const uint8_t want[] = {0x80, 0x85, 0x90, 60, 100, 0x86, 0x90, 62, 100, 0x86, 0xF8};
    TEST_ASSERT_EQUAL(1, n_pkts);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, pkts[0], sizeof want);
}

static void test_encode_new_packet_when_timestamp_high_bits_change(void) {
    start(BLEMIDI_MAX_PACKET);
    ump_t a = ump_m1_cv(0, 0x90, 60, 100);
    blemidi_tx_add(&tx, &a, 127);
    blemidi_tx_add(&tx, &a, 128);
    blemidi_tx_flush(&tx);
    TEST_ASSERT_EQUAL(2, n_pkts);
    TEST_ASSERT_EQUAL_HEX8(0x80, pkts[0][0]);
    TEST_ASSERT_EQUAL_HEX8(0x81, pkts[1][0]);
}

static void test_encode_never_splits_a_message(void) {
    start(BLEMIDI_MIN_PACKET);  // 20 bytes: header + 4 x (ts + 3 bytes) = 17, a 5th note does not fit
    ump_t a = ump_m1_cv(0, 0x90, 60, 100);
    for (int i = 0; i < 5; i++) blemidi_tx_add(&tx, &a, 0);
    blemidi_tx_flush(&tx);
    TEST_ASSERT_EQUAL(2, n_pkts);
    TEST_ASSERT_EQUAL_size_t(17, pkt_len[0]);
    TEST_ASSERT_EQUAL_size_t(5, pkt_len[1]);
}

static void test_encode_rpn_from_midi2_stays_in_one_packet_per_message(void) {
    start(BLEMIDI_MAX_PACKET);
    ump_t rpn = ump_m2_cv(0, 0x2, 0, 0, 0, 0x80000000);
    blemidi_tx_add(&tx, &rpn, 0);
    blemidi_tx_flush(&tx);
    const uint8_t want[] = {0x80, 0x80, 0xB0, 101, 0, 0x80, 0xB0, 100, 0, 0x80, 0xB0, 6, 64, 0x80, 0xB0, 38, 0};
    TEST_ASSERT_EQUAL_size_t(sizeof want, pkt_len[0]);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, pkts[0], sizeof want);
}

static void test_set_max_clamps(void) {
    start(1000);
    TEST_ASSERT_EQUAL_size_t(BLEMIDI_MAX_PACKET, tx.max);
    blemidi_tx_set_max(&tx, 5);
    TEST_ASSERT_EQUAL_size_t(BLEMIDI_MIN_PACKET, tx.max);
}

// Round trip at several MTUs: random MIDI 1.0 streams -> UMP -> BLE packets -> decode -> UMP must give the same
// UMPs in the same order, every packet within the limit and starting with a valid header.
static uint8_t r7(void) { return rand() & 0x7F; }

static void round_trip(size_t max) {
    midi1_in_t src, dst;
    midi1_in_init(&src, 0);
    midi1_in_init(&dst, 0);
    ump_t sent[512], got[512];
    int n_sent = 0, n_got = 0;
    start(max);
    uint32_t now = 0;
    while (n_sent < 400) {
        uint8_t msg[64];
        size_t n = 0;
        int kind = rand() % 6;
        if (kind < 3) {
            uint8_t st = (uint8_t)(0x80 | (rand() % 7) << 4 | (rand() % 16));
            msg[n++] = st;
            for (int i = 0; i < midi1_data_len(st); i++) msg[n++] = r7();
        } else if (kind == 3) {
            msg[n++] = 0xF8;
        } else {
            int len = rand() % 50;
            msg[n++] = 0xF0;
            for (int i = 0; i < len; i++) msg[n++] = r7();
            msg[n++] = 0xF7;
        }
        for (size_t i = 0; i < n; i++) {
            ump_t u[2];
            int k = midi1_in_feed(&src, msg[i], u);
            for (int j = 0; j < k; j++) {
                sent[n_sent++] = u[j];
                blemidi_tx_add(&tx, &u[j], now);
            }
        }
        now += rand() % 40;
        if (rand() % 4 == 0) blemidi_tx_flush(&tx);
        // decode what has been emitted so far
        for (int p = 0; p < n_pkts; p++) {
            TEST_ASSERT_TRUE(pkt_len[p] <= max);
            TEST_ASSERT_EQUAL_HEX8(0x80, pkts[p][0] & 0xC0);
            uint8_t bytes[BLEMIDI_MAX_PACKET];
            int nb = blemidi_decode(pkts[p], pkt_len[p], bytes);
            TEST_ASSERT_TRUE(nb >= 0);
            for (int i = 0; i < nb; i++) {
                ump_t u[2];
                int k = midi1_in_feed(&dst, bytes[i], u);
                for (int j = 0; j < k; j++) got[n_got++] = u[j];
            }
        }
        n_pkts = 0;
    }
    blemidi_tx_flush(&tx);
    for (int p = 0; p < n_pkts; p++) {
        uint8_t bytes[BLEMIDI_MAX_PACKET];
        int nb = blemidi_decode(pkts[p], pkt_len[p], bytes);
        for (int i = 0; i < nb; i++) {
            ump_t u[2];
            int k = midi1_in_feed(&dst, bytes[i], u);
            for (int j = 0; j < k; j++) got[n_got++] = u[j];
        }
    }
    // The decoder side re-packs SysEx into 6-byte UMPs the same way, so the UMP streams match one to one.
    TEST_ASSERT_EQUAL(n_sent, n_got);
    for (int i = 0; i < n_sent; i++) TEST_ASSERT_TRUE_MESSAGE(ump_equal(&sent[i], &got[i]), "UMP differs");
    TEST_ASSERT_EQUAL_UINT32(0, dst.dropped);
}

static void test_round_trip_min_mtu(void) {
    srand(7);
    round_trip(BLEMIDI_MIN_PACKET);
}

static void test_round_trip_mid_mtu(void) {
    srand(8);
    round_trip(64);
}

static void test_round_trip_max_mtu(void) {
    srand(9);
    round_trip(BLEMIDI_MAX_PACKET);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_decode_one_message);
    RUN_TEST(test_decode_several_messages_and_running_status);
    RUN_TEST(test_decode_running_status_with_timestamp);
    RUN_TEST(test_decode_sysex_across_packets_with_real_time);
    RUN_TEST(test_decode_rejects_bad_headers);
    RUN_TEST(test_encode_note_with_timestamp);
    RUN_TEST(test_encode_batches_messages_and_repeats_status);
    RUN_TEST(test_encode_new_packet_when_timestamp_high_bits_change);
    RUN_TEST(test_encode_never_splits_a_message);
    RUN_TEST(test_encode_rpn_from_midi2_stays_in_one_packet_per_message);
    RUN_TEST(test_set_max_clamps);
    RUN_TEST(test_round_trip_min_mtu);
    RUN_TEST(test_round_trip_mid_mtu);
    RUN_TEST(test_round_trip_max_mtu);
    return UNITY_END();
}
