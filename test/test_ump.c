#include "airmidi/ump.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static void test_words_per_message_type(void) {
    const int expected[16] = {1, 1, 1, 2, 2, 4, 1, 1, 2, 2, 2, 3, 3, 4, 4, 4};
    for (int mt = 0; mt < 16; mt++) TEST_ASSERT_EQUAL_INT_MESSAGE(expected[mt], ump_words_for_mt(mt), "mt");
}

static void test_m1_cv_fields(void) {
    ump_t u = ump_m1_cv(3, 0x92, 60, 100);
    TEST_ASSERT_EQUAL_HEX32(0x23923C64, u.w[0]);
    TEST_ASSERT_EQUAL(UMP_MT_M1_CV, ump_mt(&u));
    TEST_ASSERT_EQUAL(3, ump_group(&u));
    TEST_ASSERT_EQUAL_HEX8(0x92, ump_status(&u));
    TEST_ASSERT_EQUAL(60, ump_byte3(&u));
    TEST_ASSERT_EQUAL(100, ump_byte4(&u));
    TEST_ASSERT_EQUAL(1, ump_words(&u));
}

static void test_data_bytes_are_masked_to_7_bits(void) {
    ump_t u = ump_m1_cv(0, 0xB0, 0xFF, 0x80);
    TEST_ASSERT_EQUAL_HEX32(0x20B07F00, u.w[0]);
}

static void test_system_message(void) {
    ump_t u = ump_system(0, 0xF2, 0x10, 0x20);
    TEST_ASSERT_EQUAL_HEX32(0x10F21020, u.w[0]);
}

static void test_sysex7_round_trip(void) {
    const uint8_t data[6] = {1, 2, 3, 4, 5, 6};
    for (int n = 0; n <= 6; n++) {
        ump_t u = ump_sysex7(5, UMP_SYSEX_CONTINUE, data, n);
        uint8_t out[6] = {0};
        TEST_ASSERT_EQUAL(UMP_MT_SYSEX7, ump_mt(&u));
        TEST_ASSERT_EQUAL(5, ump_group(&u));
        TEST_ASSERT_EQUAL(UMP_SYSEX_CONTINUE, ump_sysex7_status(&u));
        TEST_ASSERT_EQUAL(n, ump_sysex7_data(&u, out));
        if (n) TEST_ASSERT_EQUAL_UINT8_ARRAY(data, out, n);
    }
}

static void test_sysex7_layout(void) {
    const uint8_t data[6] = {0x7E, 0x7F, 0x0D, 0x70, 0x02, 0x11};
    ump_t u = ump_sysex7(0, UMP_SYSEX_START, data, 6);
    TEST_ASSERT_EQUAL_HEX32(0x30167E7F, u.w[0]);
    TEST_ASSERT_EQUAL_HEX32(0x0D700211, u.w[1]);
}

static void test_sysex7_malformed_count_does_not_overrun(void) {
    ump_t u = {{0x300F0102, 0x03040506}};  // count 15
    uint8_t out[6];
    TEST_ASSERT_EQUAL(6, ump_sysex7_data(&u, out));
}

static void test_m2_cv_layout(void) {
    ump_t u = ump_m2_cv(1, 0x9, 2, 60, 0, 0xFFFF0000);
    TEST_ASSERT_EQUAL_HEX32(0x41923C00, u.w[0]);
    TEST_ASSERT_EQUAL_HEX32(0xFFFF0000, u.w[1]);
    TEST_ASSERT_EQUAL(2, ump_words(&u));
}

static void test_equal_compares_only_used_words(void) {
    ump_t a = ump_m1_cv(0, 0x90, 1, 2), b = a;
    b.w[1] = 0xDEADBEEF;  // not part of a 32-bit message
    TEST_ASSERT_TRUE(ump_equal(&a, &b));
    b.w[0] ^= 1;
    TEST_ASSERT_FALSE(ump_equal(&a, &b));
}

static void test_midi1_data_len(void) {
    TEST_ASSERT_EQUAL(2, midi1_data_len(0x80));
    TEST_ASSERT_EQUAL(2, midi1_data_len(0x9F));
    TEST_ASSERT_EQUAL(2, midi1_data_len(0xA0));
    TEST_ASSERT_EQUAL(2, midi1_data_len(0xB5));
    TEST_ASSERT_EQUAL(1, midi1_data_len(0xC0));
    TEST_ASSERT_EQUAL(1, midi1_data_len(0xDF));
    TEST_ASSERT_EQUAL(2, midi1_data_len(0xE0));
    TEST_ASSERT_EQUAL(-1, midi1_data_len(0xF0));
    TEST_ASSERT_EQUAL(1, midi1_data_len(0xF1));
    TEST_ASSERT_EQUAL(2, midi1_data_len(0xF2));
    TEST_ASSERT_EQUAL(1, midi1_data_len(0xF3));
    TEST_ASSERT_EQUAL(-1, midi1_data_len(0xF4));
    TEST_ASSERT_EQUAL(-1, midi1_data_len(0xF5));
    TEST_ASSERT_EQUAL(0, midi1_data_len(0xF6));
    TEST_ASSERT_EQUAL(0, midi1_data_len(0xF8));
    TEST_ASSERT_EQUAL(-1, midi1_data_len(0xF9));
    TEST_ASSERT_EQUAL(-1, midi1_data_len(0xFD));
    TEST_ASSERT_EQUAL(0, midi1_data_len(0xFF));
    TEST_ASSERT_EQUAL(-1, midi1_data_len(0x40));
}

// sysex7_split / sysex7_rx: collect the packets of a message and get the same bytes back.
static ump_t packets[128];
static int npackets;
static void collect(void *ctx, const ump_t *u) {
    (void)ctx;
    packets[npackets++] = *u;
}

static void test_sysex7_split_and_reassemble(void) {
    uint8_t msg[500], buf[512];
    for (int i = 0; i < 500; i++) msg[i] = (uint8_t)(i * 7) & 0x7F;
    const size_t lengths[] = {0, 1, 6, 7, 12, 13, 500};
    for (size_t t = 0; t < sizeof lengths / sizeof lengths[0]; t++) {
        size_t n = lengths[t];
        npackets = 0;
        int k = sysex7_split(3, msg, n, collect, NULL);
        TEST_ASSERT_EQUAL(npackets, k);
        TEST_ASSERT_EQUAL(n <= 6 ? 1 : (int)((n + 5) / 6), k);
        TEST_ASSERT_EQUAL(n <= 6 ? UMP_SYSEX_COMPLETE : UMP_SYSEX_START, ump_sysex7_status(&packets[0]));
        sysex7_rx_t rx;
        sysex7_rx_init(&rx, buf, sizeof buf);
        for (int i = 0; i < k; i++) {
            TEST_ASSERT_EQUAL(3, ump_group(&packets[i]));
            TEST_ASSERT_EQUAL(i == k - 1, sysex7_rx_push(&rx, &packets[i]));
        }
        TEST_ASSERT_EQUAL(n, rx.len);
        if (n) TEST_ASSERT_EQUAL_MEMORY(msg, buf, n);
    }
}

static void test_sysex7_rx_drops_a_message_that_does_not_fit(void) {
    uint8_t msg[40] = {1, 2, 3}, buf[20];
    sysex7_rx_t rx;
    sysex7_rx_init(&rx, buf, sizeof buf);
    npackets = 0;
    sysex7_split(0, msg, sizeof msg, collect, NULL);
    for (int i = 0; i < npackets; i++) TEST_ASSERT_FALSE(sysex7_rx_push(&rx, &packets[i]));
    TEST_ASSERT_EQUAL(1, rx.overflows);
    // the next one that fits comes through
    npackets = 0;
    sysex7_split(0, msg, 20, collect, NULL);
    bool got = false;
    for (int i = 0; i < npackets; i++) got = sysex7_rx_push(&rx, &packets[i]);
    TEST_ASSERT_TRUE(got);
    TEST_ASSERT_EQUAL(20, rx.len);
}

static void test_sysex7_rx_start_abandons_and_stray_packets_are_ignored(void) {
    uint8_t a[] = {1, 2, 3, 4, 5, 6, 7, 8, 9}, buf[64];
    sysex7_rx_t rx;
    sysex7_rx_init(&rx, buf, sizeof buf);
    ump_t cont = ump_sysex7(0, UMP_SYSEX_CONTINUE, a, 6), end = ump_sysex7(0, UMP_SYSEX_END, a, 3);
    TEST_ASSERT_FALSE(sysex7_rx_push(&rx, &cont));  // no Start before it
    TEST_ASSERT_FALSE(sysex7_rx_push(&rx, &end));
    ump_t start = ump_sysex7(0, UMP_SYSEX_START, a, 6), note = ump_m1_cv(0, 0x90, 60, 1);
    sysex7_rx_push(&rx, &start);
    TEST_ASSERT_FALSE(sysex7_rx_push(&rx, &note));  // not SysEx: ignored, the message goes on
    sysex7_rx_push(&rx, &start);                    // a new Start: the first message is abandoned
    TEST_ASSERT_TRUE(sysex7_rx_push(&rx, &end));
    TEST_ASSERT_EQUAL(9, rx.len);
    TEST_ASSERT_EQUAL_MEMORY(a, buf, 6);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_words_per_message_type);
    RUN_TEST(test_m1_cv_fields);
    RUN_TEST(test_data_bytes_are_masked_to_7_bits);
    RUN_TEST(test_system_message);
    RUN_TEST(test_sysex7_round_trip);
    RUN_TEST(test_sysex7_layout);
    RUN_TEST(test_sysex7_malformed_count_does_not_overrun);
    RUN_TEST(test_m2_cv_layout);
    RUN_TEST(test_equal_compares_only_used_words);
    RUN_TEST(test_midi1_data_len);
    RUN_TEST(test_sysex7_split_and_reassemble);
    RUN_TEST(test_sysex7_rx_drops_a_message_that_does_not_fit);
    RUN_TEST(test_sysex7_rx_start_abandons_and_stray_packets_are_ignored);
    return UNITY_END();
}
