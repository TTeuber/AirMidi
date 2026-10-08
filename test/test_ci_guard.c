#include "airmidi/ci_guard.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static ci_guard_t g;
static const uint8_t discovery[] = {0x7E, 0x7F, 0x0D, 0x70, 0x02, 0x11, 0x22, 0x33, 0x44, 0x7F, 0x7F, 0x7F, 0x7F};
enum { BOX = 8, MAC = 3, PAGE = 5 };

// Feed a whole SysEx (1 byte or more) through the guard; returns how many of its packets passed.
static int feed(int src, bool guarded, const uint8_t *d, int n, uint32_t now) {
    int passed = 0;
    for (int at = 0; at < n; at += 6) {
        uint8_t st = n <= 6        ? UMP_SYSEX_COMPLETE
                     : at == 0     ? UMP_SYSEX_START
                     : at + 6 >= n ? UMP_SYSEX_END
                                   : UMP_SYSEX_CONTINUE;
        ump_t u = ump_sysex7(0, st, d + at, n - at < 6 ? n - at : 6);
        passed += ci_guard_pass(&g, src, guarded, &u, now);
    }
    return passed;
}

static void test_first_discovery_passes_whole(void) {
    ci_guard_init(&g);
    TEST_ASSERT_EQUAL(3, feed(MAC, true, discovery, sizeof discovery, 0));
    TEST_ASSERT_EQUAL(0, g.dropped);
}

static void test_copy_back_within_a_second_is_dropped_whole(void) {
    ci_guard_init(&g);
    feed(BOX, false, discovery, sizeof discovery, 1000);                       // the box's own scan
    TEST_ASSERT_EQUAL(0, feed(MAC, true, discovery, sizeof discovery, 1080));  // back from the Mac
    TEST_ASSERT_EQUAL(1, g.dropped);
    TEST_ASSERT_EQUAL(0, feed(MAC, true, discovery, sizeof discovery, 1999));  // still inside the second
    TEST_ASSERT_EQUAL(3, feed(MAC, true, discovery, sizeof discovery, 2000));  // a second later: a new one
}

static void test_unguarded_source_always_passes(void) {
    ci_guard_init(&g);
    feed(MAC, true, discovery, sizeof discovery, 0);
    TEST_ASSERT_EQUAL(3, feed(BOX, false, discovery, sizeof discovery, 10));
    TEST_ASSERT_EQUAL(0, feed(MAC, true, discovery, sizeof discovery, 900));  // the box's counts as the last one
}

static void test_other_messages_are_untouched(void) {
    ci_guard_init(&g);
    feed(BOX, false, discovery, sizeof discovery, 0);
    // Discovery Reply, PE Capabilities, PE Get, Invalidate MUID: other MIDI-CI messages pass
    const uint8_t others[] = {0x71, 0x30, 0x34, 0x7E};
    for (int i = 0; i < 4; i++) {
        uint8_t ci[] = {0x7E, 0x7F, 0x0D, others[i], 0x02, 1, 2, 3, 4, 5, 6, 7, 8};
        TEST_ASSERT_EQUAL(3, feed(MAC, true, ci, sizeof ci, 10));
    }
    const uint8_t plain[] = {0x7D, 0x01, 0x02};
    TEST_ASSERT_EQUAL(1, feed(MAC, true, plain, sizeof plain, 10));
    ump_t note = ump_m1_cv(0, 0x90, 60, 100);
    TEST_ASSERT_TRUE(ci_guard_pass(&g, MAC, true, &note, 10));
    TEST_ASSERT_EQUAL(0, g.dropped);
}

static void test_dropping_one_source_leaves_the_others_alone(void) {
    ci_guard_init(&g);
    feed(BOX, false, discovery, sizeof discovery, 0);
    ump_t first = ump_sysex7(0, UMP_SYSEX_START, discovery, 6);
    TEST_ASSERT_FALSE(ci_guard_pass(&g, MAC, true, &first, 10));
    const uint8_t plain[] = {0x7D, 1, 2, 3, 4, 5, 6, 7};  // another source's SysEx in the middle
    TEST_ASSERT_EQUAL(2, feed(PAGE, true, plain, sizeof plain, 11));
    ump_t rest = ump_sysex7(0, UMP_SYSEX_END, discovery + 6, 6);
    TEST_ASSERT_FALSE(ci_guard_pass(&g, MAC, true, &rest, 12));
    TEST_ASSERT_EQUAL(2, feed(MAC, true, plain, sizeof plain, 13));  // the Mac's next SysEx passes
}

static void test_sysex_cut_short_ends_the_drop(void) {
    ci_guard_init(&g);
    feed(BOX, false, discovery, sizeof discovery, 0);
    ump_t first = ump_sysex7(0, UMP_SYSEX_START, discovery, 6);
    TEST_ASSERT_FALSE(ci_guard_pass(&g, MAC, true, &first, 10));
    const uint8_t plain[] = {0x7D, 1, 2, 3, 4, 5, 6, 7};  // a new message without the END: the drop is over
    TEST_ASSERT_EQUAL(2, feed(MAC, true, plain, sizeof plain, 11));
}

static void test_clock_wraps(void) {
    ci_guard_init(&g);
    feed(BOX, false, discovery, sizeof discovery, 0xFFFFFF00u);
    TEST_ASSERT_EQUAL(0, feed(MAC, true, discovery, sizeof discovery, 0x00000100u));  // 512 ms later
    TEST_ASSERT_EQUAL(3, feed(MAC, true, discovery, sizeof discovery, 0x00000400u));  // 1280 ms later
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_first_discovery_passes_whole);
    RUN_TEST(test_copy_back_within_a_second_is_dropped_whole);
    RUN_TEST(test_unguarded_source_always_passes);
    RUN_TEST(test_other_messages_are_untouched);
    RUN_TEST(test_dropping_one_source_leaves_the_others_alone);
    RUN_TEST(test_sysex_cut_short_ends_the_drop);
    RUN_TEST(test_clock_wraps);
    return UNITY_END();
}
