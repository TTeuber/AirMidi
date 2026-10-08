#include "airmidi/router.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static router_t rt;
static int dst[PORT_COUNT];
static ump_t out[PORT_COUNT];

static void test_default_is_everything_to_everything_else(void) {
    router_init(&rt);
    ump_t u = ump_m1_cv(0, 0x90, 60, 100);
    int n = router_route(&rt, PORT_TRS, &u, dst, out);
    TEST_ASSERT_EQUAL(PORT_COUNT - 2, n);  // not back to TRS, not to the CI initiator
    for (int i = 0; i < n; i++) {
        TEST_ASSERT_NOT_EQUAL(PORT_TRS, dst[i]);
        TEST_ASSERT_NOT_EQUAL(PORT_CI, dst[i]);
        TEST_ASSERT_TRUE(ump_equal(&u, &out[i]));
    }
}

// The device's MIDI-CI replies reach the initiator and every other port (so a CI host behind the box can talk
// to the device too); the initiator's own messages go to TRS only.
static void test_ci_port_talks_only_sysex_with_trs(void) {
    router_init(&rt);
    uint8_t b[6] = {0x7E, 0x7F, 0x0D, 0x71, 2, 0};
    ump_t sx = ump_sysex7(0, UMP_SYSEX_START, b, 6), note = ump_m1_cv(0, 0x90, 60, 100);
    TEST_ASSERT_EQUAL(PORT_COUNT - 1, router_route(&rt, PORT_TRS, &sx, dst, out));
    TEST_ASSERT_EQUAL(1, router_route(&rt, PORT_CI, &sx, dst, out));
    TEST_ASSERT_EQUAL(PORT_TRS, dst[0]);
    TEST_ASSERT_EQUAL(0, router_route(&rt, PORT_CI, &note, dst, out));
    TEST_ASSERT_EQUAL(PORT_COUNT - 2, router_route(&rt, PORT_BLE_0, &sx, dst, out));  // not BLE_0, not CI
    for (int i = 0; i < PORT_COUNT - 2; i++) TEST_ASSERT_NOT_EQUAL(PORT_CI, dst[i]);
}

static void test_stream_and_utility_messages_are_never_routed(void) {
    router_init(&rt);
    ump_t stream = {{0xF0000101, 0x1F, 0, 0}}, noop = {{0}}, jr = {{0x00201234}};
    TEST_ASSERT_EQUAL(0, router_route(&rt, PORT_NET_0, &stream, dst, out));
    TEST_ASSERT_EQUAL(0, router_route(&rt, PORT_NET_0, &noop, dst, out));
    TEST_ASSERT_EQUAL(0, router_route(&rt, PORT_NET_0, &jr, dst, out));
}

static void test_bad_source_routes_nothing(void) {
    router_init(&rt);
    ump_t u = ump_m1_cv(0, 0x90, 60, 100);
    TEST_ASSERT_EQUAL(0, router_route(&rt, -1, &u, dst, out));
    TEST_ASSERT_EQUAL(0, router_route(&rt, PORT_COUNT, &u, dst, out));
}

static void test_disabled_route(void) {
    router_init(&rt);
    for (int d = 0; d < PORT_COUNT; d++) rt.r[PORT_BLE_0][d].enabled = d == PORT_TRS;
    ump_t u = ump_m1_cv(0, 0xB0, 7, 100);
    TEST_ASSERT_EQUAL(1, router_route(&rt, PORT_BLE_0, &u, dst, out));
    TEST_ASSERT_EQUAL(PORT_TRS, dst[0]);
}

static void test_channel_filter_and_remap(void) {
    router_init(&rt);
    for (int d = 0; d < PORT_COUNT; d++) rt.r[PORT_TRS][d].enabled = d == PORT_BLE_0;
    route_t *r = &rt.r[PORT_TRS][PORT_BLE_0];
    r->channels = 1 << 0 | 1 << 9;  // channels 1 and 10
    r->channel_map[9] = 3;          // 10 -> 4
    ump_t ch1 = ump_m1_cv(2, 0x90, 60, 100), ch2 = ump_m1_cv(2, 0x91, 60, 100), ch10 = ump_m1_cv(2, 0x99, 36, 90);
    TEST_ASSERT_EQUAL(1, router_route(&rt, PORT_TRS, &ch1, dst, out));
    TEST_ASSERT_EQUAL(0, router_route(&rt, PORT_TRS, &ch2, dst, out));
    TEST_ASSERT_EQUAL(1, router_route(&rt, PORT_TRS, &ch10, dst, out));
    TEST_ASSERT_EQUAL_HEX32(0x2293245A, out[0].w[0]);  // group and data untouched, channel 4
    // channel filter and remap do not apply to system messages
    ump_t clock = ump_system(0, 0xF8, 0, 0);
    TEST_ASSERT_EQUAL(1, router_route(&rt, PORT_TRS, &clock, dst, out));
    TEST_ASSERT_TRUE(ump_equal(&clock, &out[0]));
}

static void test_remap_midi2_message_keeps_data(void) {
    router_init(&rt);
    for (int d = 0; d < PORT_COUNT; d++) rt.r[PORT_NET_0][d].enabled = d == PORT_TRS;
    rt.r[PORT_NET_0][PORT_TRS].channel_map[0] = 15;
    ump_t u = ump_m2_cv(1, 0xB, 0, 74, 0, 0x12345678);
    TEST_ASSERT_EQUAL(1, router_route(&rt, PORT_NET_0, &u, dst, out));
    TEST_ASSERT_EQUAL_HEX32(0x41BF4A00, out[0].w[0]);
    TEST_ASSERT_EQUAL_HEX32(0x12345678, out[0].w[1]);
}

static void test_type_filter(void) {
    router_init(&rt);
    for (int d = 0; d < PORT_COUNT; d++) rt.r[PORT_TRS][d].enabled = d == PORT_BLE_0;
    rt.r[PORT_TRS][PORT_BLE_0].types = MSG_ALL & ~(MSG_CLOCK | MSG_SYSEX);
    ump_t clock = ump_system(0, 0xF8, 0, 0), start = ump_system(0, 0xFA, 0, 0), spp = ump_system(0, 0xF2, 0, 0);
    const uint8_t d[] = {1};
    ump_t sx = ump_sysex7(0, UMP_SYSEX_COMPLETE, d, 1), note = ump_m1_cv(0, 0x90, 1, 1);
    TEST_ASSERT_EQUAL(0, router_route(&rt, PORT_TRS, &clock, dst, out));
    TEST_ASSERT_EQUAL(0, router_route(&rt, PORT_TRS, &start, dst, out));
    TEST_ASSERT_EQUAL(0, router_route(&rt, PORT_TRS, &sx, dst, out));
    TEST_ASSERT_EQUAL(1, router_route(&rt, PORT_TRS, &spp, dst, out));
    TEST_ASSERT_EQUAL(1, router_route(&rt, PORT_TRS, &note, dst, out));
}

static uint16_t cls(ump_t u) { return router_class(&u); }

static void test_classes(void) {
    TEST_ASSERT_EQUAL(MSG_NOTE, cls(ump_m1_cv(0, 0x80, 1, 1)));
    TEST_ASSERT_EQUAL(MSG_POLY_PRESSURE, cls(ump_m1_cv(0, 0xA0, 1, 1)));
    TEST_ASSERT_EQUAL(MSG_CONTROL, cls(ump_m1_cv(0, 0xB0, 1, 1)));
    TEST_ASSERT_EQUAL(MSG_PROGRAM, cls(ump_m1_cv(0, 0xC0, 1, 0)));
    TEST_ASSERT_EQUAL(MSG_CHANNEL_PRESSURE, cls(ump_m1_cv(0, 0xD0, 1, 0)));
    TEST_ASSERT_EQUAL(MSG_PITCH_BEND, cls(ump_m1_cv(0, 0xE0, 1, 1)));
    TEST_ASSERT_EQUAL(MSG_CONTROL, cls(ump_m2_cv(0, 0x2, 0, 0, 0, 0)));
    TEST_ASSERT_EQUAL(MSG_PER_NOTE, cls(ump_m2_cv(0, 0x6, 0, 60, 0, 0)));
    TEST_ASSERT_EQUAL(MSG_CLOCK, cls(ump_system(0, 0xFC, 0, 0)));
    TEST_ASSERT_EQUAL(MSG_SYSTEM, cls(ump_system(0, 0xFE, 0, 0)));
    TEST_ASSERT_EQUAL(MSG_OTHER, router_class(&(ump_t){{0xD0000000}}));
    TEST_ASSERT_EQUAL(MSG_SYSEX, router_class(&(ump_t){{0x50000000}}));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_default_is_everything_to_everything_else);
    RUN_TEST(test_ci_port_talks_only_sysex_with_trs);
    RUN_TEST(test_stream_and_utility_messages_are_never_routed);
    RUN_TEST(test_bad_source_routes_nothing);
    RUN_TEST(test_disabled_route);
    RUN_TEST(test_channel_filter_and_remap);
    RUN_TEST(test_remap_midi2_message_keeps_data);
    RUN_TEST(test_type_filter);
    RUN_TEST(test_classes);
    return UNITY_END();
}
