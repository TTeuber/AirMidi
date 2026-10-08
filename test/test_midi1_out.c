#include <stdlib.h>
#include <string.h>

#include "airmidi/midi1.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static midi1_out_t enc;
static uint8_t bytes[4096];
static size_t n_bytes;

static void reset(bool running_status) {
    midi1_out_init(&enc, running_status);
    n_bytes = 0;
}

static void put(ump_t u) {
    uint8_t buf[MIDI1_OUT_MAX];
    size_t n = midi1_out_encode(&enc, &u, buf);
    TEST_ASSERT_TRUE(n <= MIDI1_OUT_MAX);
    TEST_ASSERT_TRUE(n_bytes + n <= sizeof bytes);
    memcpy(bytes + n_bytes, buf, n);
    n_bytes += n;
}

#define EXPECT(...)                                               \
    do {                                                          \
        const uint8_t want_[] = {__VA_ARGS__};                    \
        TEST_ASSERT_EQUAL_size_t(sizeof want_, n_bytes);          \
        TEST_ASSERT_EQUAL_HEX8_ARRAY(want_, bytes, sizeof want_); \
    } while (0)

static void test_channel_messages_without_running_status(void) {
    reset(false);
    put(ump_m1_cv(0, 0x90, 60, 100));
    put(ump_m1_cv(0, 0x90, 62, 100));
    put(ump_m1_cv(0, 0xC3, 5, 0));
    EXPECT(0x90, 60, 100, 0x90, 62, 100, 0xC3, 5);
}

static void test_running_status(void) {
    reset(true);
    put(ump_m1_cv(0, 0x90, 60, 100));
    put(ump_m1_cv(0, 0x90, 62, 100));
    put(ump_m1_cv(0, 0x80, 60, 0));
    put(ump_m1_cv(0, 0x80, 62, 0));
    EXPECT(0x90, 60, 100, 62, 100, 0x80, 60, 0, 62, 0);
}

static void test_real_time_keeps_running_status(void) {
    reset(true);
    put(ump_m1_cv(0, 0xB0, 7, 100));
    put(ump_system(0, 0xF8, 0, 0));
    put(ump_m1_cv(0, 0xB0, 10, 64));
    EXPECT(0xB0, 7, 100, 0xF8, 10, 64);
}

static void test_system_common_resets_running_status(void) {
    reset(true);
    put(ump_m1_cv(0, 0xB0, 7, 100));
    put(ump_system(0, 0xF3, 2, 0));
    put(ump_m1_cv(0, 0xB0, 10, 64));
    put(ump_system(0, 0xF2, 1, 2));
    put(ump_system(0, 0xF6, 0, 0));
    EXPECT(0xB0, 7, 100, 0xF3, 2, 0xB0, 10, 64, 0xF2, 1, 2, 0xF6);
}

static void test_reset_running(void) {
    reset(true);
    put(ump_m1_cv(0, 0x90, 60, 100));
    midi1_out_reset_running(&enc);
    put(ump_m1_cv(0, 0x90, 62, 100));
    EXPECT(0x90, 60, 100, 0x90, 62, 100);
}

static void test_sysex_packets(void) {
    reset(true);
    const uint8_t a[] = {1, 2, 3, 4, 5, 6}, b[] = {7, 8}, c[] = {9};
    put(ump_m1_cv(0, 0x90, 60, 100));
    put(ump_sysex7(0, UMP_SYSEX_START, a, 6));
    put(ump_system(0, 0xF8, 0, 0));  // real-time in the middle of SysEx is legal
    put(ump_sysex7(0, UMP_SYSEX_CONTINUE, b, 2));
    put(ump_sysex7(0, UMP_SYSEX_END, c, 1));
    put(ump_m1_cv(0, 0x90, 62, 100));  // SysEx cancelled running status: status byte again
    EXPECT(0x90, 60, 100, 0xF0, 1, 2, 3, 4, 5, 6, 0xF8, 7, 8, 9, 0xF7, 0x90, 62, 100);
}

static void test_sysex_complete_and_empty(void) {
    reset(false);
    const uint8_t a[] = {0x7D, 0x01};
    put(ump_sysex7(0, UMP_SYSEX_COMPLETE, a, 2));
    put(ump_sysex7(0, UMP_SYSEX_COMPLETE, NULL, 0));
    EXPECT(0xF0, 0x7D, 0x01, 0xF7, 0xF0, 0xF7);
}

static void test_broken_sysex_is_closed(void) {
    reset(false);
    const uint8_t a[] = {1, 2, 3, 4, 5, 6};
    put(ump_sysex7(0, UMP_SYSEX_START, a, 6));
    put(ump_m1_cv(0, 0x90, 60, 100));  // END never came
    EXPECT(0xF0, 1, 2, 3, 4, 5, 6, 0xF7, 0x90, 60, 100);
}

static void test_sysex_continuation_without_start_is_dropped(void) {
    reset(false);
    const uint8_t a[] = {1, 2};
    put(ump_sysex7(0, UMP_SYSEX_CONTINUE, a, 2));
    put(ump_sysex7(0, UMP_SYSEX_END, a, 2));
    TEST_ASSERT_EQUAL_size_t(0, n_bytes);
}

static void test_midi2_is_translated(void) {
    reset(true);
    put(ump_m2_cv(0, 0x9, 0, 60, 0, 0xFFFF0000));  // note on, full velocity
    put(ump_m2_cv(0, 0x2, 0, 0, 0, 0x80000000));   // RPN 0/0 (pitch bend range), center
    EXPECT(0x90, 60, 127, 0xB0, 101, 0, 100, 0, 6, 64, 38, 0);
}

static void test_messages_without_midi1_meaning_are_dropped(void) {
    reset(false);
    put((ump_t){{0x00000000}});                    // utility NOOP
    put((ump_t){{0xF0000101, 0x0000001F, 0, 0}});  // stream: endpoint discovery
    put(ump_m2_cv(0, 0x0, 0, 60, 1, 0));           // per-note registered controller
    put(ump_system(0, 0xF4, 0, 0));                // undefined system common
    put(ump_system(0, 0xF0, 0, 0));                // SysEx start does not travel as MT 0x1
    TEST_ASSERT_EQUAL_size_t(0, n_bytes);
}

// Round trip: a random but valid MIDI 1.0 stream (with running status, real-time bytes sprinkled anywhere, SysEx
// of random length) goes through midi1_in and back through midi1_out (running status on) and must come out as
// the same bytes. The generator writes the canonical form: running status whenever the status repeats.
static uint8_t r7(void) { return rand() & 0x7F; }

static void test_round_trip_random_streams(void) {
    srand(1234);
    for (int iter = 0; iter < 200; iter++) {
        uint8_t in[2048];
        size_t n_in = 0;
        uint8_t running = 0;
        while (n_in < sizeof in - 64) {
            int kind = rand() % 10;
            if (kind < 6) {  // channel message
                uint8_t st = (uint8_t)(0x80 | (rand() % 7) << 4 | (rand() % 16));
                if (st != running) in[n_in++] = st;
                running = st;
                in[n_in++] = r7();
                if (midi1_data_len(st) == 2) in[n_in++] = r7();
            } else if (kind < 8) {  // real-time
                static const uint8_t rt[] = {0xF8, 0xFA, 0xFB, 0xFC, 0xFE, 0xFF};
                in[n_in++] = rt[rand() % 6];
            } else if (kind == 8) {  // system common
                static const uint8_t sc[] = {0xF1, 0xF2, 0xF3, 0xF6};
                uint8_t st = sc[rand() % 4];
                in[n_in++] = st;
                for (int i = 0; i < midi1_data_len(st); i++) in[n_in++] = r7();
                running = 0;
            } else {  // SysEx, with the odd real-time byte inside
                int len = rand() % 40;
                in[n_in++] = 0xF0;
                for (int i = 0; i < len; i++) {
                    if (rand() % 10 == 0) in[n_in++] = 0xF8;
                    in[n_in++] = r7();
                }
                in[n_in++] = 0xF7;
                running = 0;
            }
        }

        midi1_in_t p;
        midi1_in_init(&p, 0);
        reset(true);
        for (size_t i = 0; i < n_in; i++) {
            ump_t u[2];
            int k = midi1_in_feed(&p, in[i], u);
            for (int j = 0; j < k; j++) put(u[j]);
        }
        TEST_ASSERT_EQUAL_UINT32(0, p.dropped);
        // Real-time bytes inside a SysEx come out ahead of the SysEx bytes still held in the parser (up to 6), so
        // compare with the real-time bytes removed, and check their count separately.
        uint8_t a[2048], b[4096];
        size_t na = 0, nb = 0, rta = 0, rtb = 0;
        for (size_t i = 0; i < n_in; i++) in[i] >= 0xF8 ? (void)rta++ : (void)(a[na++] = in[i]);
        for (size_t i = 0; i < n_bytes; i++) bytes[i] >= 0xF8 ? (void)rtb++ : (void)(b[nb++] = bytes[i]);
        TEST_ASSERT_EQUAL_size_t(rta, rtb);
        TEST_ASSERT_EQUAL_size_t(na, nb);
        TEST_ASSERT_EQUAL_HEX8_ARRAY(a, b, na);
    }
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_channel_messages_without_running_status);
    RUN_TEST(test_running_status);
    RUN_TEST(test_real_time_keeps_running_status);
    RUN_TEST(test_system_common_resets_running_status);
    RUN_TEST(test_reset_running);
    RUN_TEST(test_sysex_packets);
    RUN_TEST(test_sysex_complete_and_empty);
    RUN_TEST(test_broken_sysex_is_closed);
    RUN_TEST(test_sysex_continuation_without_start_is_dropped);
    RUN_TEST(test_midi2_is_translated);
    RUN_TEST(test_messages_without_midi1_meaning_are_dropped);
    RUN_TEST(test_round_trip_random_streams);
    return UNITY_END();
}
