#include <stdlib.h>
#include <string.h>

#include "airmidi/merge.h"
#include "airmidi/midi1.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static merge_t m;
static const uint8_t six[6] = {1, 2, 3, 4, 5, 6};

static ump_t pop(uint32_t now) {
    ump_t u;
    TEST_ASSERT_TRUE_MESSAGE(merge_pop(&m, &u, now), "expected a message");
    return u;
}

static void expect_empty(uint32_t now) {
    ump_t u;
    TEST_ASSERT_FALSE_MESSAGE(merge_pop(&m, &u, now), "expected nothing");
}

static void expect(ump_t want, uint32_t now) {
    ump_t got = pop(now);
    TEST_ASSERT_EQUAL_HEX32(want.w[0], got.w[0]);
    TEST_ASSERT_EQUAL_HEX32(want.w[1], got.w[1]);
}

static void push(int src, ump_t u, uint32_t now) { merge_push(&m, src, &u, now); }

static void test_passes_through_in_order(void) {
    merge_init(&m, 500);
    push(0, ump_m1_cv(0, 0x90, 60, 100), 0);
    push(1, ump_m1_cv(0, 0xB0, 7, 1), 0);
    expect(ump_m1_cv(0, 0x90, 60, 100), 0);
    expect(ump_m1_cv(0, 0xB0, 7, 1), 0);
    expect_empty(0);
}

static void test_sysex_holds_other_sources_but_not_real_time(void) {
    merge_init(&m, 500);
    push(0, ump_sysex7(0, UMP_SYSEX_START, six, 6), 0);
    push(1, ump_m1_cv(0, 0x90, 60, 100), 1);  // held
    push(1, ump_system(0, 0xF8, 0, 0), 2);    // clock: straight through
    push(0, ump_sysex7(0, UMP_SYSEX_CONTINUE, six, 6), 3);
    push(1, ump_m1_cv(0, 0x80, 60, 0), 4);  // held
    expect(ump_sysex7(0, UMP_SYSEX_START, six, 6), 5);
    expect(ump_system(0, 0xF8, 0, 0), 5);
    expect(ump_sysex7(0, UMP_SYSEX_CONTINUE, six, 6), 5);
    expect_empty(5);
    push(0, ump_sysex7(0, UMP_SYSEX_END, six, 2), 6);
    expect(ump_sysex7(0, UMP_SYSEX_END, six, 2), 6);
    expect(ump_m1_cv(0, 0x90, 60, 100), 6);
    expect(ump_m1_cv(0, 0x80, 60, 0), 6);
    expect_empty(6);
}

static void test_active_source_own_messages_wait_too(void) {
    merge_init(&m, 500);
    push(0, ump_sysex7(0, UMP_SYSEX_START, six, 6), 0);
    push(0, ump_m1_cv(0, 0x90, 60, 100), 1);  // would cut its own SysEx on a MIDI 1.0 wire
    push(0, ump_sysex7(0, UMP_SYSEX_END, six, 1), 2);
    expect(ump_sysex7(0, UMP_SYSEX_START, six, 6), 3);
    expect(ump_sysex7(0, UMP_SYSEX_END, six, 1), 3);
    expect(ump_m1_cv(0, 0x90, 60, 100), 3);
}

static void test_held_sysex_runs_after_and_holds_what_follows(void) {
    merge_init(&m, 500);
    push(0, ump_sysex7(0, UMP_SYSEX_START, six, 6), 0);
    push(1, ump_sysex7(0, UMP_SYSEX_START, six, 6), 1);  // B's SysEx waits
    push(2, ump_m1_cv(0, 0x90, 1, 1), 2);                // C's note waits
    push(1, ump_sysex7(0, UMP_SYSEX_END, six, 3), 3);    // B's end waits
    push(0, ump_sysex7(0, UMP_SYSEX_END, six, 1), 4);    // A ends: B's SysEx runs, then C's note
    expect(ump_sysex7(0, UMP_SYSEX_START, six, 6), 5);
    expect(ump_sysex7(0, UMP_SYSEX_END, six, 1), 5);
    expect(ump_sysex7(0, UMP_SYSEX_START, six, 6), 5);
    expect(ump_sysex7(0, UMP_SYSEX_END, six, 3), 5);
    expect(ump_m1_cv(0, 0x90, 1, 1), 5);
    expect_empty(5);
    TEST_ASSERT_EQUAL(-1, m.active);
}

// Regression: B's SysEx is released and finishes within one pass; C's note off must not overtake its note on,
// which had to wait again behind B's START.
static void test_release_keeps_each_sources_order(void) {
    merge_init(&m, 500);
    push(0, ump_sysex7(0, UMP_SYSEX_START, six, 6), 0);  // A active
    push(1, ump_sysex7(0, UMP_SYSEX_START, six, 6), 1);  // B waits
    push(2, ump_m1_cv(0, 0x92, 60, 100), 2);             // C note on waits
    push(1, ump_sysex7(0, UMP_SYSEX_END, six, 1), 3);    // B waits
    push(2, ump_m1_cv(0, 0x82, 60, 0), 4);               // C note off waits
    push(0, ump_sysex7(0, UMP_SYSEX_END, six, 1), 5);
    expect(ump_sysex7(0, UMP_SYSEX_START, six, 6), 6);
    expect(ump_sysex7(0, UMP_SYSEX_END, six, 1), 6);
    expect(ump_sysex7(0, UMP_SYSEX_START, six, 6), 6);
    expect(ump_sysex7(0, UMP_SYSEX_END, six, 1), 6);
    expect(ump_m1_cv(0, 0x92, 60, 100), 6);
    expect(ump_m1_cv(0, 0x82, 60, 0), 6);
    expect_empty(6);
}

static void test_timeout_closes_sysex_and_drops_its_rest(void) {
    merge_init(&m, 500);
    push(0, ump_sysex7(3, UMP_SYSEX_START, six, 6), 100);
    push(1, ump_m1_cv(0, 0x90, 60, 100), 150);
    expect(ump_sysex7(3, UMP_SYSEX_START, six, 6), 200);
    expect_empty(599);                                   // 499 ms: still waiting
    expect(ump_sysex7(3, UMP_SYSEX_END, NULL, 0), 600);  // closed, in the SysEx's group
    expect(ump_m1_cv(0, 0x90, 60, 100), 600);
    TEST_ASSERT_EQUAL(1, m.timeouts);
    push(0, ump_sysex7(3, UMP_SYSEX_CONTINUE, six, 6), 700);  // the late rest is dropped
    push(0, ump_sysex7(3, UMP_SYSEX_END, six, 1), 701);
    expect_empty(702);
    TEST_ASSERT_EQUAL(2, m.dropped);
    push(0, ump_sysex7(3, UMP_SYSEX_COMPLETE, six, 2), 800);  // a new SysEx from it is fine
    expect(ump_sysex7(3, UMP_SYSEX_COMPLETE, six, 2), 800);
}

static void test_overflow_is_counted_not_blocking(void) {
    merge_init(&m, 500);
    for (int i = 0; i < MERGE_QUEUE + 5; i++) push(0, ump_m1_cv(0, 0x90, i & 0x7F, 1), 0);
    TEST_ASSERT_EQUAL(5, m.dropped);
    push(0, ump_sysex7(0, UMP_SYSEX_START, six, 6), 0);  // ready queue full: dropped too
    TEST_ASSERT_EQUAL(6, m.dropped);
}

// Three sources send notes, clocks and SysEx in random interleavings. Written to a MIDI 1.0 wire through
// midi1_out and parsed back with midi1_in, every SysEx must arrive whole and unbroken, and every note too.
static void test_random_interleaving_keeps_every_sysex_whole(void) {
    srand(42);
    for (int iter = 0; iter < 50; iter++) {
        merge_init(&m, 500);
        midi1_out_t enc;
        midi1_out_init(&enc, true);
        midi1_in_t dec;
        midi1_in_init(&dec, 0);
        int sent_notes = 0, got_notes = 0, sent_sysex = 0, got_sysex = 0, sx_pos[3] = {0}, sx_len[3] = {0};
        int got_bytes = 0, want_bytes = 0;
        uint32_t now = 0;
        for (int step = 0; step < 400; step++) {
            int src = rand() % 3;
            if (sx_len[src]) {  // continue this source's SysEx
                int n = sx_len[src] - sx_pos[src] > 6 ? 6 : sx_len[src] - sx_pos[src];
                bool last = sx_pos[src] + n == sx_len[src];
                push(src, ump_sysex7(0, last ? UMP_SYSEX_END : UMP_SYSEX_CONTINUE, six, n), now);
                sx_pos[src] += n;
                if (last) sx_len[src] = 0;
            } else if (rand() % 5 == 0) {
                sx_len[src] = 7 + rand() % 30;
                sx_pos[src] = 6;
                want_bytes += sx_len[src];
                sent_sysex++;
                push(src, ump_sysex7(0, UMP_SYSEX_START, six, 6), now);
            } else if (rand() % 4 == 0) {
                push(src, ump_system(0, 0xF8, 0, 0), now);
            } else {
                push(src, ump_m1_cv(0, 0x90 | src, rand() & 0x7F, 1 + rand() % 127), now);
                sent_notes++;
            }
            now += rand() % 3;
            ump_t u;
            while (merge_pop(&m, &u, now)) {
                uint8_t bytes[MIDI1_OUT_MAX];
                size_t n = midi1_out_encode(&enc, &u, bytes);
                for (size_t i = 0; i < n; i++) {
                    ump_t p[2];
                    int k = midi1_in_feed(&dec, bytes[i], p);
                    for (int j = 0; j < k; j++) {
                        if (ump_mt(&p[j]) == UMP_MT_M1_CV) got_notes++;
                        if (ump_mt(&p[j]) == UMP_MT_SYSEX7) {
                            uint8_t d[6];
                            got_bytes += ump_sysex7_data(&p[j], d);
                            uint8_t st = ump_sysex7_status(&p[j]);
                            if (st == UMP_SYSEX_END || st == UMP_SYSEX_COMPLETE) got_sysex++;
                        }
                    }
                }
            }
        }
        // finish open SysEx so the counts can match
        for (int src = 0; src < 3; src++) {
            while (sx_len[src]) {
                int n = sx_len[src] - sx_pos[src] > 6 ? 6 : sx_len[src] - sx_pos[src];
                bool last = sx_pos[src] + n == sx_len[src];
                push(src, ump_sysex7(0, last ? UMP_SYSEX_END : UMP_SYSEX_CONTINUE, six, n), now);
                sx_pos[src] += n;
                if (last) sx_len[src] = 0;
            }
        }
        ump_t u;
        while (merge_pop(&m, &u, now)) {
            uint8_t bytes[MIDI1_OUT_MAX];
            size_t n = midi1_out_encode(&enc, &u, bytes);
            for (size_t i = 0; i < n; i++) {
                ump_t p[2];
                int k = midi1_in_feed(&dec, bytes[i], p);
                for (int j = 0; j < k; j++) {
                    if (ump_mt(&p[j]) == UMP_MT_M1_CV) got_notes++;
                    if (ump_mt(&p[j]) == UMP_MT_SYSEX7) {
                        uint8_t d[6];
                        got_bytes += ump_sysex7_data(&p[j], d);
                        uint8_t st = ump_sysex7_status(&p[j]);
                        if (st == UMP_SYSEX_END || st == UMP_SYSEX_COMPLETE) got_sysex++;
                    }
                }
            }
        }
        TEST_ASSERT_EQUAL_UINT32(0, m.dropped);
        TEST_ASSERT_EQUAL_UINT32(0, m.timeouts);
        TEST_ASSERT_EQUAL_UINT32(0, dec.dropped);
        TEST_ASSERT_EQUAL(sent_notes, got_notes);
        TEST_ASSERT_EQUAL(sent_sysex, got_sysex);
        TEST_ASSERT_EQUAL(want_bytes, got_bytes);
    }
}

// A slow output: values coalesce to the newest per channel, in place; notes are never merged or reordered.
static void test_bends_coalesce_per_channel(void) {
    merge_init(&m, 500);
    push(0, ump_m1_cv(0, 0xE1, 0, 64), 0);
    push(0, ump_m1_cv(0, 0xE2, 0, 64), 0);  // another channel: its own value
    push(0, ump_m1_cv(0, 0xE1, 10, 65), 0);
    push(0, ump_m1_cv(0, 0xE1, 20, 66), 0);
    push(0, ump_m1_cv(0, 0xE2, 5, 70), 0);
    expect(ump_m1_cv(0, 0xE1, 20, 66), 0);  // the newest value, in the first one's place
    expect(ump_m1_cv(0, 0xE2, 5, 70), 0);
    expect_empty(0);
    TEST_ASSERT_EQUAL(3, m.coalesced);
}

static void test_no_coalescing_across_a_note_on_the_same_channel(void) {
    merge_init(&m, 500);
    push(0, ump_m1_cv(0, 0xE1, 0, 70), 0);     // bend for the note
    push(0, ump_m1_cv(0, 0x91, 50, 100), 0);  // note on
    push(0, ump_m1_cv(0, 0xE1, 0, 72), 0);     // must stay after the note on
    push(0, ump_m1_cv(0, 0x81, 50, 0), 0);
    push(0, ump_m1_cv(0, 0xE1, 0, 64), 0);     // bend for the next note
    push(0, ump_m1_cv(0, 0xE1, 0, 66), 0);     // coalesces with the one just before
    expect(ump_m1_cv(0, 0xE1, 0, 70), 0);
    expect(ump_m1_cv(0, 0x91, 50, 100), 0);
    expect(ump_m1_cv(0, 0xE1, 0, 72), 0);
    expect(ump_m1_cv(0, 0x81, 50, 0), 0);
    expect(ump_m1_cv(0, 0xE1, 0, 66), 0);
    expect_empty(0);
    TEST_ASSERT_EQUAL(1, m.coalesced);
}

static void test_other_channels_in_between_do_not_block(void) {
    merge_init(&m, 500);
    push(0, ump_m1_cv(0, 0xE1, 0, 70), 0);
    push(0, ump_m1_cv(0, 0x92, 55, 100), 0);  // channel 3: unrelated
    push(0, ump_m1_cv(0, 0xE1, 0, 71), 0);
    expect(ump_m1_cv(0, 0xE1, 0, 71), 0);
    expect(ump_m1_cv(0, 0x92, 55, 100), 0);
    expect_empty(0);
}

static void test_sources_and_sequenced_controllers_are_kept(void) {
    merge_init(&m, 500);
    push(0, ump_m1_cv(0, 0xE0, 0, 70), 0);
    push(1, ump_m1_cv(0, 0xE0, 0, 71), 0);  // another source: not merged into source 0's value
    push(0, ump_m1_cv(0, 0xB0, 101, 0), 0);  // RPN select + data entry: every one counts
    push(0, ump_m1_cv(0, 0xB0, 100, 0), 0);
    push(0, ump_m1_cv(0, 0xB0, 6, 48), 0);
    push(0, ump_m1_cv(0, 0xB0, 6, 12), 0);
    push(0, ump_m1_cv(0, 0xB0, 7, 10), 0);  // volume: a value
    push(0, ump_m1_cv(0, 0xB0, 7, 90), 0);
    push(0, ump_m1_cv(0, 0xB0, 10, 64), 0);  // pan: another controller, kept
    expect(ump_m1_cv(0, 0xE0, 0, 70), 0);
    expect(ump_m1_cv(0, 0xE0, 0, 71), 0);
    expect(ump_m1_cv(0, 0xB0, 101, 0), 0);
    expect(ump_m1_cv(0, 0xB0, 100, 0), 0);
    expect(ump_m1_cv(0, 0xB0, 6, 48), 0);
    expect(ump_m1_cv(0, 0xB0, 6, 12), 0);
    expect(ump_m1_cv(0, 0xB0, 7, 90), 0);
    expect(ump_m1_cv(0, 0xB0, 10, 64), 0);
    expect_empty(0);
}

static void test_a_bend_flood_never_drops_a_note_off(void) {
    merge_init(&m, 500);
    for (int i = 0; i < 1000; i++) {  // the output writes nothing meanwhile
        push(0, ump_m1_cv(0, 0xE1, i & 0x7F, 64), 0);
        push(0, ump_m1_cv(0, 0xE2, i & 0x7F, 64), 0);
        push(0, ump_m1_cv(0, 0xE3, i & 0x7F, 64), 0);
    }
    push(0, ump_m1_cv(0, 0x81, 50, 0), 0);
    TEST_ASSERT_EQUAL(0, m.dropped);
    expect(ump_m1_cv(0, 0xE1, 999 & 0x7F, 64), 0);
    expect(ump_m1_cv(0, 0xE2, 999 & 0x7F, 64), 0);
    expect(ump_m1_cv(0, 0xE3, 999 & 0x7F, 64), 0);
    expect(ump_m1_cv(0, 0x81, 50, 0), 0);
    expect_empty(0);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_passes_through_in_order);
    RUN_TEST(test_sysex_holds_other_sources_but_not_real_time);
    RUN_TEST(test_active_source_own_messages_wait_too);
    RUN_TEST(test_held_sysex_runs_after_and_holds_what_follows);
    RUN_TEST(test_release_keeps_each_sources_order);
    RUN_TEST(test_timeout_closes_sysex_and_drops_its_rest);
    RUN_TEST(test_overflow_is_counted_not_blocking);
    RUN_TEST(test_random_interleaving_keeps_every_sysex_whole);
    RUN_TEST(test_bends_coalesce_per_channel);
    RUN_TEST(test_no_coalescing_across_a_note_on_the_same_channel);
    RUN_TEST(test_other_channels_in_between_do_not_block);
    RUN_TEST(test_sources_and_sequenced_controllers_are_kept);
    RUN_TEST(test_a_bend_flood_never_drops_a_note_off);
    return UNITY_END();
}
