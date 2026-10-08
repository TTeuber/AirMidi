#include <string.h>

#include "airmidi/midi1.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static midi1_in_t parser;
static ump_t got[64];
static int n_got;

static void feed(const uint8_t *bytes, size_t n) {
    for (size_t i = 0; i < n; i++) {
        ump_t out[2];
        int k = midi1_in_feed(&parser, bytes[i], out);
        TEST_ASSERT_TRUE(k >= 0 && k <= 2);
        for (int j = 0; j < k; j++) {
            TEST_ASSERT_TRUE(n_got < 64);
            got[n_got++] = out[j];
        }
    }
}

#define FEED(...)                           \
    do {                                    \
        const uint8_t b_[] = {__VA_ARGS__}; \
        feed(b_, sizeof b_);                \
    } while (0)

static void expect(const ump_t *want, int n) {
    TEST_ASSERT_EQUAL_INT_MESSAGE(n, n_got, "number of UMPs");
    for (int i = 0; i < n; i++) {
        TEST_ASSERT_EQUAL_HEX32_MESSAGE(want[i].w[0], got[i].w[0], "word 0");
        TEST_ASSERT_EQUAL_HEX32_MESSAGE(want[i].w[1], got[i].w[1], "word 1");
    }
}

static void reset(uint8_t group) {
    midi1_in_init(&parser, group);
    n_got = 0;
    memset(got, 0, sizeof got);
}

static void test_note_on(void) {
    reset(0);
    FEED(0x90, 60, 100);
    ump_t want[] = {ump_m1_cv(0, 0x90, 60, 100)};
    expect(want, 1);
}

static void test_group_is_stamped(void) {
    reset(7);
    FEED(0xB3, 74, 1, 0xF8);
    ump_t want[] = {ump_m1_cv(7, 0xB3, 74, 1), ump_system(7, 0xF8, 0, 0)};
    expect(want, 2);
}

static void test_running_status(void) {
    reset(0);
    FEED(0x90, 60, 100, 62, 101, 64, 0);
    ump_t want[] = {ump_m1_cv(0, 0x90, 60, 100), ump_m1_cv(0, 0x90, 62, 101), ump_m1_cv(0, 0x90, 64, 0)};
    expect(want, 3);
}

static void test_running_status_one_data_byte(void) {
    reset(0);
    FEED(0xC1, 5, 6, 7);
    ump_t want[] = {ump_m1_cv(0, 0xC1, 5, 0), ump_m1_cv(0, 0xC1, 6, 0), ump_m1_cv(0, 0xC1, 7, 0)};
    expect(want, 3);
}

static void test_real_time_between_status_and_data(void) {
    reset(0);
    FEED(0x90, 0xF8, 60, 0xFA, 100, 62, 0xFE, 1);
    ump_t want[] = {ump_system(0, 0xF8, 0, 0), ump_system(0, 0xFA, 0, 0), ump_m1_cv(0, 0x90, 60, 100),
                    ump_system(0, 0xFE, 0, 0), ump_m1_cv(0, 0x90, 62, 1)};
    expect(want, 5);
}

static void test_system_common_cancels_running_status(void) {
    reset(0);
    FEED(0x90, 60, 100, 0xF3, 5, 62, 100);
    ump_t want[] = {ump_m1_cv(0, 0x90, 60, 100), ump_system(0, 0xF3, 5, 0)};
    expect(want, 2);
    TEST_ASSERT_EQUAL(2, parser.dropped);  // 62 100 had no status
}

static void test_song_position_and_tune_request(void) {
    reset(0);
    FEED(0xF2, 0x10, 0x20, 0xF6, 0xF1, 0x35);
    ump_t want[] = {ump_system(0, 0xF2, 0x10, 0x20), ump_system(0, 0xF6, 0, 0), ump_system(0, 0xF1, 0x35, 0)};
    expect(want, 3);
}

static void test_data_without_status_is_dropped(void) {
    reset(0);
    FEED(60, 100, 0x80, 60, 0);
    ump_t want[] = {ump_m1_cv(0, 0x80, 60, 0)};
    expect(want, 1);
    TEST_ASSERT_EQUAL(2, parser.dropped);
}

static void test_message_cut_short_by_status(void) {
    reset(0);
    FEED(0x90, 60, 0xB0, 7, 100);
    ump_t want[] = {ump_m1_cv(0, 0xB0, 7, 100)};
    expect(want, 1);
    TEST_ASSERT_EQUAL(1, parser.dropped);
}

static void test_undefined_statuses_are_dropped(void) {
    reset(0);
    FEED(0x90, 60, 100, 0xF4, 61, 1, 0xF9, 0xFD, 0xF5);
    ump_t want[] = {ump_m1_cv(0, 0x90, 60, 100)};
    expect(want, 1);
    TEST_ASSERT_EQUAL(6, parser.dropped);  // F4, 61, 1 (no status after F4), F9, FD, F5
}

// SysEx of every length 0..20 is packed into 6-byte UMPs: COMPLETE if it fits in one, else START, CONTINUE...,
// END, and no packet is ever empty except a zero-length COMPLETE.
static void test_sysex_packetization(void) {
    for (int len = 0; len <= 20; len++) {
        reset(0);
        uint8_t msg[24];
        msg[0] = 0xF0;
        for (int i = 0; i < len; i++) msg[1 + i] = (uint8_t)(i + 1);
        msg[1 + len] = 0xF7;
        feed(msg, len + 2);
        int packets = len <= 6 ? 1 : (len + 5) / 6;
        TEST_ASSERT_EQUAL_INT_MESSAGE(packets, n_got, "packets");
        int pos = 0;
        for (int p = 0; p < n_got; p++) {
            uint8_t d[6];
            int n = ump_sysex7_data(&got[p], d);
            uint8_t st = ump_sysex7_status(&got[p]);
            uint8_t want = packets == 1     ? UMP_SYSEX_COMPLETE
                           : p == 0         ? UMP_SYSEX_START
                           : p == n_got - 1 ? UMP_SYSEX_END
                                            : UMP_SYSEX_CONTINUE;
            TEST_ASSERT_EQUAL_MESSAGE(want, st, "packet status");
            if (packets > 1) TEST_ASSERT_TRUE(n > 0);
            for (int i = 0; i < n; i++) TEST_ASSERT_EQUAL(++pos, d[i]);
        }
        TEST_ASSERT_EQUAL(len, pos);
    }
}

static void test_real_time_inside_sysex(void) {
    reset(0);
    FEED(0xF0, 1, 2, 0xF8, 3, 4, 5, 6, 0xFC, 7, 0xF7);
    const uint8_t a[] = {1, 2, 3, 4, 5, 6}, b[] = {7};
    ump_t want[] = {ump_system(0, 0xF8, 0, 0), ump_system(0, 0xFC, 0, 0), ump_sysex7(0, UMP_SYSEX_START, a, 6),
                    ump_sysex7(0, UMP_SYSEX_END, b, 1)};
    expect(want, 4);
}

static void test_sysex_cut_short_by_status(void) {
    reset(0);
    FEED(0xF0, 1, 2, 0x90, 60, 100);
    const uint8_t a[] = {1, 2};
    ump_t want[] = {ump_sysex7(0, UMP_SYSEX_COMPLETE, a, 2), ump_m1_cv(0, 0x90, 60, 100)};
    expect(want, 2);
}

static void test_tune_request_ends_sysex_and_counts(void) {
    reset(0);
    FEED(0xF0, 1, 2, 3, 4, 5, 6, 7, 0xF6);
    const uint8_t a[] = {1, 2, 3, 4, 5, 6}, b[] = {7};
    ump_t want[] = {ump_sysex7(0, UMP_SYSEX_START, a, 6), ump_sysex7(0, UMP_SYSEX_END, b, 1),
                    ump_system(0, 0xF6, 0, 0)};
    expect(want, 3);
}

static void test_sysex_restarted_by_f0(void) {
    reset(0);
    FEED(0xF0, 1, 0xF0, 2, 0xF7);
    const uint8_t a[] = {1}, b[] = {2};
    ump_t want[] = {ump_sysex7(0, UMP_SYSEX_COMPLETE, a, 1), ump_sysex7(0, UMP_SYSEX_COMPLETE, b, 1)};
    expect(want, 2);
}

static void test_sysex_cancels_running_status(void) {
    reset(0);
    FEED(0x90, 60, 100, 0xF0, 0xF7, 62, 100);
    ump_t want[] = {ump_m1_cv(0, 0x90, 60, 100), ump_sysex7(0, UMP_SYSEX_COMPLETE, NULL, 0)};
    expect(want, 2);
    TEST_ASSERT_EQUAL(2, parser.dropped);
}

static void test_stray_f7_is_ignored(void) {
    reset(0);
    FEED(0xF7, 0x90, 60, 100, 0xF7, 62, 1);
    // F7 is a status byte, so it also ends running status: 62 1 are dropped.
    ump_t want[] = {ump_m1_cv(0, 0x90, 60, 100)};
    expect(want, 1);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_note_on);
    RUN_TEST(test_group_is_stamped);
    RUN_TEST(test_running_status);
    RUN_TEST(test_running_status_one_data_byte);
    RUN_TEST(test_real_time_between_status_and_data);
    RUN_TEST(test_system_common_cancels_running_status);
    RUN_TEST(test_song_position_and_tune_request);
    RUN_TEST(test_data_without_status_is_dropped);
    RUN_TEST(test_message_cut_short_by_status);
    RUN_TEST(test_undefined_statuses_are_dropped);
    RUN_TEST(test_sysex_packetization);
    RUN_TEST(test_real_time_inside_sysex);
    RUN_TEST(test_sysex_cut_short_by_status);
    RUN_TEST(test_tune_request_ends_sysex_and_counts);
    RUN_TEST(test_sysex_restarted_by_f0);
    RUN_TEST(test_sysex_cancels_running_status);
    RUN_TEST(test_stray_f7_is_ignored);
    return UNITY_END();
}
