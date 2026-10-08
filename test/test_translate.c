#include <stdlib.h>

#include "airmidi/translate.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

// ---- scaling (M2-104 D.1) ------------------------------------------------------------------------------------

static void test_scale_up_spec_examples(void) {
    TEST_ASSERT_EQUAL_HEX32(0x1400, scale_up(10, 7, 16));
    TEST_ASSERT_EQUAL_HEX32(0x8000, scale_up(64, 7, 16));
    TEST_ASSERT_EQUAL_HEX32(0xAEBA, scale_up(87, 7, 16));
    TEST_ASSERT_EQUAL_HEX32(0xFFFF, scale_up(127, 7, 16));
    TEST_ASSERT_EQUAL_HEX32(0x0200, scale_up(1, 7, 16));  // D.3.1: velocity 1 -> 0x0200
}

// The spec's own 7 -> 16 bit routine (M2-104 D.1.3, "Code for Min-Center-Max Scaling Up from 7-Bit to 16-Bit"),
// as an independent reference for every 7-bit value.
static uint16_t spec_scale_up_7_to_16(uint8_t value7) {
    uint16_t shifted = (uint16_t)value7 << 9;
    if (value7 <= 64) return shifted;
    uint16_t repeat6 = value7 & 0x3F;
    return shifted | (repeat6 << 3) | (repeat6 >> 3);
}

static void test_scale_up_matches_spec_reference(void) {
    for (uint8_t v = 0; v < 128; v++) TEST_ASSERT_EQUAL_HEX32(spec_scale_up_7_to_16(v), scale_up(v, 7, 16));
    TEST_ASSERT_EQUAL_HEX32(0x8208, scale_up(65, 7, 16));  // just above center: the bit repeat starts
}

static void test_scale_up_min_center_max(void) {
    TEST_ASSERT_EQUAL_HEX32(0, scale_up(0, 7, 32));
    TEST_ASSERT_EQUAL_HEX32(0x80000000, scale_up(64, 7, 32));
    TEST_ASSERT_EQUAL_HEX32(0xFFFFFFFF, scale_up(127, 7, 32));
    TEST_ASSERT_EQUAL_HEX32(0x80000000, scale_up(0x2000, 14, 32));
    TEST_ASSERT_EQUAL_HEX32(0xFFFFFFFF, scale_up(0x3FFF, 14, 32));
}

static void test_scale_down_spec_examples(void) {
    TEST_ASSERT_EQUAL(0x0A, scale_down(0x1400, 16, 7));
    TEST_ASSERT_EQUAL(0x40, scale_down(0x8000, 16, 7));
    TEST_ASSERT_EQUAL(0x57, scale_down(0xAEBA, 16, 7));
    TEST_ASSERT_EQUAL(0x7F, scale_down(0xFFFF, 16, 7));
}

static void test_scale_round_trips_are_lossless(void) {
    for (uint32_t v = 0; v < 128; v++) {
        TEST_ASSERT_EQUAL(v, scale_down(scale_up(v, 7, 32), 32, 7));
        TEST_ASSERT_EQUAL(v, scale_down(scale_up(v, 7, 16), 16, 7));
    }
    for (uint32_t v = 0; v < 16384; v++) TEST_ASSERT_EQUAL(v, scale_down(scale_up(v, 14, 32), 32, 14));
}

static void test_scale_up_is_monotonic(void) {
    for (uint32_t v = 1; v < 16384; v++) TEST_ASSERT_TRUE(scale_up(v, 14, 32) > scale_up(v - 1, 14, 32));
}

// ---- MIDI 2.0 -> MIDI 1.0 ------------------------------------------------------------------------------------

static ump_t m1[4];

static int m2_to_m1_of(ump_t u) { return m2_to_m1(&u, m1); }

static void test_m2_note_on_off(void) {
    TEST_ASSERT_EQUAL(1, m2_to_m1(&(ump_t){{0x40933C00, 0xAEBA0000}}, m1));
    TEST_ASSERT_EQUAL_HEX32(0x20933C57, m1[0].w[0]);
    TEST_ASSERT_EQUAL(1, m2_to_m1(&(ump_t){{0x40833C00, 0x80000000}}, m1));
    TEST_ASSERT_EQUAL_HEX32(0x20833C40, m1[0].w[0]);
}

static void test_m2_tiny_velocity_never_becomes_note_off(void) {
    TEST_ASSERT_EQUAL(1, m2_to_m1(&(ump_t){{0x40903C00, 0x00010000}}, m1));
    TEST_ASSERT_EQUAL_HEX32(0x20903C01, m1[0].w[0]);
    TEST_ASSERT_EQUAL(1, m2_to_m1(&(ump_t){{0x40903C00, 0x00000000}}, m1));
    TEST_ASSERT_EQUAL_HEX32(0x20903C01, m1[0].w[0]);
}

static void test_m2_controllers_and_pressure(void) {
    TEST_ASSERT_EQUAL(1, m2_to_m1_of(ump_m2_cv(2, 0xB, 5, 74, 0, 0xFFFFFFFF)));
    TEST_ASSERT_EQUAL_HEX32(0x22B54A7F, m1[0].w[0]);
    TEST_ASSERT_EQUAL(1, m2_to_m1_of(ump_m2_cv(0, 0xA, 1, 60, 0, 0x80000000)));
    TEST_ASSERT_EQUAL_HEX32(0x20A13C40, m1[0].w[0]);
    TEST_ASSERT_EQUAL(1, m2_to_m1_of(ump_m2_cv(0, 0xD, 1, 0, 0, 0x02000000)));
    TEST_ASSERT_EQUAL_HEX32(0x20D10100, m1[0].w[0]);
}

static void test_m2_pitch_bend(void) {
    TEST_ASSERT_EQUAL(1, m2_to_m1_of(ump_m2_cv(0, 0xE, 0, 0, 0, 0x80000000)));
    TEST_ASSERT_EQUAL_HEX32(0x20E00040, m1[0].w[0]);  // 0x2000: LSB 0, MSB 0x40
    TEST_ASSERT_EQUAL(1, m2_to_m1_of(ump_m2_cv(0, 0xE, 0, 0, 0, 0xFFFFFFFF)));
    TEST_ASSERT_EQUAL_HEX32(0x20E07F7F, m1[0].w[0]);
}

static void test_m2_program_change(void) {
    TEST_ASSERT_EQUAL(1, m2_to_m1_of(ump_m2_cv(0, 0xC, 3, 0, 0, 0x05000000)));
    TEST_ASSERT_EQUAL_HEX32(0x20C30500, m1[0].w[0]);
    // bank valid: Bank Select MSB, LSB, then the program
    TEST_ASSERT_EQUAL(3, m2_to_m1_of(ump_m2_cv(0, 0xC, 3, 0, 1, 0x05000102)));
    TEST_ASSERT_EQUAL_HEX32(0x20B30001, m1[0].w[0]);
    TEST_ASSERT_EQUAL_HEX32(0x20B32002, m1[1].w[0]);
    TEST_ASSERT_EQUAL_HEX32(0x20C30500, m1[2].w[0]);
}

static void test_m2_rpn_and_nrpn(void) {
    TEST_ASSERT_EQUAL(4, m2_to_m1_of(ump_m2_cv(0, 0x2, 0, 0, 1, scale_up(0x2000 | 0x15, 14, 32))));
    TEST_ASSERT_EQUAL_HEX32(0x20B06500, m1[0].w[0]);  // CC 101 = 0
    TEST_ASSERT_EQUAL_HEX32(0x20B06401, m1[1].w[0]);  // CC 100 = 1
    TEST_ASSERT_EQUAL_HEX32(0x20B00640, m1[2].w[0]);  // CC 6 = 0x40
    TEST_ASSERT_EQUAL_HEX32(0x20B02615, m1[3].w[0]);  // CC 38 = 0x15
    TEST_ASSERT_EQUAL(4, m2_to_m1_of(ump_m2_cv(0, 0x3, 9, 0x12, 0x34, 0)));
    TEST_ASSERT_EQUAL_HEX32(0x20B96312, m1[0].w[0]);  // CC 99
    TEST_ASSERT_EQUAL_HEX32(0x20B96234, m1[1].w[0]);  // CC 98
}

static void test_m2_untranslatable(void) {
    const uint8_t per_note[] = {0x0, 0x1, 0x4, 0x5, 0x6, 0xF};
    for (int i = 0; i < 6; i++) TEST_ASSERT_EQUAL(0, m2_to_m1_of(ump_m2_cv(0, per_note[i], 0, 60, 0, 1)));
}

static void test_m2_other_types_pass_through(void) {
    ump_t sys = ump_system(0, 0xF8, 0, 0);
    TEST_ASSERT_EQUAL(1, m2_to_m1(&sys, m1));
    TEST_ASSERT_TRUE(ump_equal(&sys, &m1[0]));
}

// ---- MIDI 1.0 -> MIDI 2.0 ------------------------------------------------------------------------------------

static m1_to_m2_t tr;
static ump_t m2[2];

static int to_m2(uint8_t st, uint8_t d1, uint8_t d2) {
    return m1_to_m2(&tr, &(ump_t){{0x20000000u | st << 16 | d1 << 8 | d2}}, m2);
}

static void test_m1_note_on_velocity_zero_is_note_off(void) {
    m1_to_m2_init(&tr);
    TEST_ASSERT_EQUAL(1, to_m2(0x92, 60, 0));
    TEST_ASSERT_EQUAL_HEX32(0x40823C00, m2[0].w[0]);
    TEST_ASSERT_EQUAL_HEX32(0x80000000, m2[0].w[1]);
    TEST_ASSERT_EQUAL(1, to_m2(0x92, 60, 1));
    TEST_ASSERT_EQUAL_HEX32(0x40923C00, m2[0].w[0]);
    TEST_ASSERT_EQUAL_HEX32(0x02000000, m2[0].w[1]);
}

static void test_m1_cc_and_pitch_bend(void) {
    m1_to_m2_init(&tr);
    TEST_ASSERT_EQUAL(1, to_m2(0xB0, 74, 127));
    TEST_ASSERT_EQUAL_HEX32(0x40B04A00, m2[0].w[0]);
    TEST_ASSERT_EQUAL_HEX32(0xFFFFFFFF, m2[0].w[1]);
    TEST_ASSERT_EQUAL(1, to_m2(0xE0, 0x00, 0x40));
    TEST_ASSERT_EQUAL_HEX32(0x80000000, m2[0].w[1]);
}

static void test_m1_bank_select_rides_on_program_change(void) {
    m1_to_m2_init(&tr);
    TEST_ASSERT_EQUAL(1, to_m2(0xC0, 5, 0));
    TEST_ASSERT_EQUAL_HEX32(0x40C00000, m2[0].w[0]);  // bank valid = 0
    TEST_ASSERT_EQUAL_HEX32(0x05000000, m2[0].w[1]);
    TEST_ASSERT_EQUAL(0, to_m2(0xB0, 0, 1));
    TEST_ASSERT_EQUAL(0, to_m2(0xB0, 32, 2));
    TEST_ASSERT_EQUAL(1, to_m2(0xC0, 6, 0));
    TEST_ASSERT_EQUAL_HEX32(0x40C00001, m2[0].w[0]);
    TEST_ASSERT_EQUAL_HEX32(0x06000102, m2[0].w[1]);
    TEST_ASSERT_EQUAL(1, to_m2(0xC1, 7, 0));  // the bank is per channel
    TEST_ASSERT_EQUAL_HEX32(0x40C10000, m2[0].w[0]);
}

static void test_m1_rpn_sequence_becomes_one_message(void) {
    m1_to_m2_init(&tr);
    TEST_ASSERT_EQUAL(0, to_m2(0xB0, 101, 0));
    TEST_ASSERT_EQUAL(0, to_m2(0xB0, 100, 0));
    TEST_ASSERT_EQUAL(0, to_m2(0xB0, 6, 12));
    TEST_ASSERT_EQUAL(1, to_m2(0xB0, 38, 0));
    TEST_ASSERT_EQUAL_HEX32(0x40200000, m2[0].w[0]);  // RPN 0/0
    TEST_ASSERT_EQUAL_HEX32(scale_up(12 << 7, 14, 32), m2[0].w[1]);
}

static void test_m1_nrpn_without_lsb_is_sent_by_the_next_trigger(void) {
    m1_to_m2_init(&tr);
    to_m2(0xB3, 99, 0x12);
    to_m2(0xB3, 98, 0x34);
    TEST_ASSERT_EQUAL(0, to_m2(0xB3, 6, 100));
    TEST_ASSERT_EQUAL(1, to_m2(0xB3, 6, 101));  // a second CC 6 sends the first
    TEST_ASSERT_EQUAL_HEX32(0x40331234, m2[0].w[0]);
    TEST_ASSERT_EQUAL_HEX32(scale_up(100 << 7, 14, 32), m2[0].w[1]);
    TEST_ASSERT_EQUAL(1, to_m2(0xB3, 99, 0x01));  // a new parameter number sends the second
    TEST_ASSERT_EQUAL_HEX32(scale_up(101 << 7, 14, 32), m2[0].w[1]);
    to_m2(0xB3, 98, 0x02);
    TEST_ASSERT_EQUAL(0, to_m2(0xB3, 6, 5));
    TEST_ASSERT_EQUAL(1, m1_to_m2_flush(&tr, 0, 3, m2));  // or the caller's timeout
    TEST_ASSERT_EQUAL_HEX32(0x40330102, m2[0].w[0]);
    TEST_ASSERT_EQUAL(0, m1_to_m2_flush(&tr, 0, 3, m2));
}

static void test_m1_rpn_null_function_is_not_translated(void) {
    m1_to_m2_init(&tr);
    to_m2(0xB0, 101, 0x7F);
    to_m2(0xB0, 100, 0x7F);
    TEST_ASSERT_EQUAL(0, to_m2(0xB0, 6, 1));
    TEST_ASSERT_EQUAL(0, to_m2(0xB0, 38, 1));
}

static void test_m1_data_entry_without_parameter_is_dropped(void) {
    m1_to_m2_init(&tr);
    TEST_ASSERT_EQUAL(0, to_m2(0xB0, 6, 1));
    TEST_ASSERT_EQUAL(0, to_m2(0xB0, 38, 1));
}

static void test_m1_other_types_pass_through(void) {
    m1_to_m2_init(&tr);
    const uint8_t d[] = {1, 2};
    ump_t sx = ump_sysex7(0, UMP_SYSEX_COMPLETE, d, 2);
    TEST_ASSERT_EQUAL(1, m1_to_m2(&tr, &sx, m2));
    TEST_ASSERT_TRUE(ump_equal(&sx, &m2[0]));
}

// MIDI 1.0 -> 2.0 -> 1.0 gives back the original for every channel message that translates one to one.
static void test_m1_round_trip(void) {
    m1_to_m2_init(&tr);
    srand(99);
    for (int i = 0; i < 20000; i++) {
        uint8_t st = (uint8_t)(0x80 | (rand() % 7) << 4 | (rand() % 16));
        uint8_t d1 = rand() & 0x7F, d2 = rand() & 0x7F;
        if ((st & 0xF0) == 0xB0 && (d1 == 0 || d1 == 32 || d1 == 6 || d1 == 38 || (d1 >= 98 && d1 <= 101))) continue;
        if ((st & 0xF0) == 0x90 && d2 == 0) continue;  // becomes a Note Off, by design
        if (midi1_data_len(st) == 1) d2 = 0;
        ump_t in = ump_m1_cv(rand() % 16, st, d1, d2);
        TEST_ASSERT_EQUAL(1, m1_to_m2(&tr, &in, m2));
        TEST_ASSERT_EQUAL(1, m2_to_m1(&m2[0], m1));
        TEST_ASSERT_EQUAL_HEX32(in.w[0], m1[0].w[0]);
    }
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_scale_up_spec_examples);
    RUN_TEST(test_scale_up_matches_spec_reference);
    RUN_TEST(test_scale_up_min_center_max);
    RUN_TEST(test_scale_down_spec_examples);
    RUN_TEST(test_scale_round_trips_are_lossless);
    RUN_TEST(test_scale_up_is_monotonic);
    RUN_TEST(test_m2_note_on_off);
    RUN_TEST(test_m2_tiny_velocity_never_becomes_note_off);
    RUN_TEST(test_m2_controllers_and_pressure);
    RUN_TEST(test_m2_pitch_bend);
    RUN_TEST(test_m2_program_change);
    RUN_TEST(test_m2_rpn_and_nrpn);
    RUN_TEST(test_m2_untranslatable);
    RUN_TEST(test_m2_other_types_pass_through);
    RUN_TEST(test_m1_note_on_velocity_zero_is_note_off);
    RUN_TEST(test_m1_cc_and_pitch_bend);
    RUN_TEST(test_m1_bank_select_rides_on_program_change);
    RUN_TEST(test_m1_rpn_sequence_becomes_one_message);
    RUN_TEST(test_m1_nrpn_without_lsb_is_sent_by_the_next_trigger);
    RUN_TEST(test_m1_rpn_null_function_is_not_translated);
    RUN_TEST(test_m1_data_entry_without_parameter_is_dropped);
    RUN_TEST(test_m1_other_types_pass_through);
    RUN_TEST(test_m1_round_trip);
    return UNITY_END();
}
