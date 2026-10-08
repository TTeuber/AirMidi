#include <string.h>

#include "airmidi/ump_endpoint.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static ump_endpoint_t ep;
static ump_t out[UMP_ENDPOINT_MAX_REPLY];

static void reset(void) {
    ep = (ump_endpoint_t){.name = "airmidi-7530",
                          .product_instance_id = "10BDA38F7530",
                          .manufacturer = {0x7D, 0, 0},
                          .family = 0,
                          .model = 1,
                          .version = {0, 4, 0, 0},
                          .block_name = "airmidi",
                          .midi2_capable = true,
                          .ci_version = 2,
                          .protocol = 1};
}

static ump_t discovery(uint8_t filter) { return (ump_t){{0xF0000101, filter, 0, 0}}; }

static int status_of(const ump_t *u) { return (u->w[0] >> 16) & 0x3FF; }
static int format_of(const ump_t *u) { return (u->w[0] >> 26) & 0x3; }

// Reassemble a text field from its UMPs (skip = bytes before the text in the first word's low half).
static void text_of(const ump_t *u, int n, int skip, char *s) {
    int k = 0;
    for (int i = 0; i < n; i++) {
        uint8_t b[16];
        for (int j = 0; j < 4; j++)
            b[4 * j] = u[i].w[j] >> 24, b[4 * j + 1] = u[i].w[j] >> 16, b[4 * j + 2] = u[i].w[j] >> 8,
                  b[4 * j + 3] = u[i].w[j];
        for (int j = 2 + skip; j < 16 && b[j]; j++) s[k++] = (char)b[j];
    }
    s[k] = 0;
}

static void test_full_endpoint_discovery(void) {
    reset();
    ump_t req = discovery(0x1F);
    int n = ump_endpoint_handle(&ep, &req, out);
    TEST_ASSERT_EQUAL(5, n);  // info, identity, name (12 chars: 1 UMP), product id, stream config
    TEST_ASSERT_EQUAL(0x001, status_of(&out[0]));
    TEST_ASSERT_EQUAL_HEX32(0xF0010101, out[0].w[0]);                               // UMP 1.1
    TEST_ASSERT_EQUAL_HEX32(1u << 31 | 1u << 24 | 1u << 9 | 1u << 8, out[0].w[1]);  // static, 1 block, M2, M1
    TEST_ASSERT_EQUAL(0x002, status_of(&out[1]));
    TEST_ASSERT_EQUAL_HEX32(0x007D0000, out[1].w[1]);
    TEST_ASSERT_EQUAL_HEX32(0x00000100, out[1].w[2]);  // family 0, model 1 (LSB first)
    TEST_ASSERT_EQUAL_HEX32(0x00040000, out[1].w[3]);
    char s[100];
    TEST_ASSERT_EQUAL(0x003, status_of(&out[2]));
    TEST_ASSERT_EQUAL(0, format_of(&out[2]));
    text_of(&out[2], 1, 0, s);
    TEST_ASSERT_EQUAL_STRING("airmidi-7530", s);
    TEST_ASSERT_EQUAL(0x004, status_of(&out[3]));
    text_of(&out[3], 1, 0, s);
    TEST_ASSERT_EQUAL_STRING("10BDA38F7530", s);
    TEST_ASSERT_EQUAL_HEX32(0xF0060100, out[4].w[0]);  // MIDI 1.0 protocol, no JR timestamps
}

static void test_long_name_spans_packets(void) {
    reset();
    ep.name = "a name long enough to need three UMP packets";  // 44 bytes: 14 + 14 + 14 + 2
    ump_t req = discovery(0x04);
    int n = ump_endpoint_handle(&ep, &req, out);
    TEST_ASSERT_EQUAL(4, n);
    TEST_ASSERT_EQUAL(1, format_of(&out[0]));  // start
    TEST_ASSERT_EQUAL(2, format_of(&out[1]));  // continue
    TEST_ASSERT_EQUAL(2, format_of(&out[2]));
    TEST_ASSERT_EQUAL(3, format_of(&out[3]));  // end
    char s[100];
    text_of(out, n, 0, s);
    TEST_ASSERT_EQUAL_STRING(ep.name, s);
}

static void test_name_of_exactly_14_bytes_is_one_packet(void) {
    reset();
    ep.name = "fourteen bytes";
    ump_t req = discovery(0x04);
    TEST_ASSERT_EQUAL(1, ump_endpoint_handle(&ep, &req, out));
    TEST_ASSERT_EQUAL(0, format_of(&out[0]));
}

static void test_stream_configuration(void) {
    reset();
    ump_t to2 = {{0xF0050200}}, to1 = {{0xF0050100}}, to3 = {{0xF0050300}};
    TEST_ASSERT_EQUAL(1, ump_endpoint_handle(&ep, &to2, out));
    TEST_ASSERT_EQUAL_HEX32(0xF0060200, out[0].w[0]);
    TEST_ASSERT_EQUAL(2, ep.protocol);
    TEST_ASSERT_EQUAL(1, ump_endpoint_handle(&ep, &to3, out));  // unknown protocol: keep and say so
    TEST_ASSERT_EQUAL_HEX32(0xF0060200, out[0].w[0]);
    TEST_ASSERT_EQUAL(1, ump_endpoint_handle(&ep, &to1, out));
    TEST_ASSERT_EQUAL(1, ep.protocol);
    ep.midi2_capable = false;
    TEST_ASSERT_EQUAL(1, ump_endpoint_handle(&ep, &to2, out));
    TEST_ASSERT_EQUAL(1, ep.protocol);
}

static void test_function_block_discovery(void) {
    reset();
    ump_t all = {{0xF010FF03}}, other = {{0xF0100303}};
    int n = ump_endpoint_handle(&ep, &all, out);
    TEST_ASSERT_EQUAL(2, n);
    TEST_ASSERT_EQUAL(0x011, status_of(&out[0]));
    TEST_ASSERT_EQUAL_HEX32(0xF0118033, out[0].w[0]);  // active, block 0, hint 0b11, bidirectional
    TEST_ASSERT_EQUAL_HEX32(0x00010200, out[0].w[1]);  // group 0, 1 group, MIDI-CI 1.2
    TEST_ASSERT_EQUAL(0x012, status_of(&out[1]));
    char s[100];
    text_of(&out[1], 1, 1, s);
    TEST_ASSERT_EQUAL_STRING("airmidi", s);
    TEST_ASSERT_EQUAL_HEX32(0x00, (out[1].w[0] >> 8) & 0xFF);     // block number
    TEST_ASSERT_EQUAL(0, ump_endpoint_handle(&ep, &other, out));  // there is no block 3
}

static void test_announcement_is_the_full_discovery_reply(void) {
    reset();
    ump_t asked[UMP_ENDPOINT_MAX_REPLY];
    ump_t req = discovery(0x1F);
    int n = ump_endpoint_handle(&ep, &req, asked);
    TEST_ASSERT_EQUAL(n, ump_endpoint_announce(&ep, out));
    TEST_ASSERT_EQUAL_MEMORY(asked, out, n * sizeof(ump_t));
    TEST_ASSERT_EQUAL(0x001, status_of(&out[0]));      // Endpoint Info first
    TEST_ASSERT_EQUAL(0x006, status_of(&out[n - 1]));  // Stream Configuration last
}

static void test_ignores_everything_else(void) {
    reset();
    ump_t notification = {{0xF0010101, 0, 0, 0}}, note = ump_m1_cv(0, 0x90, 60, 100);
    TEST_ASSERT_EQUAL(0, ump_endpoint_handle(&ep, &notification, out));
    TEST_ASSERT_EQUAL(0, ump_endpoint_handle(&ep, &note, out));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_full_endpoint_discovery);
    RUN_TEST(test_long_name_spans_packets);
    RUN_TEST(test_name_of_exactly_14_bytes_is_one_packet);
    RUN_TEST(test_stream_configuration);
    RUN_TEST(test_function_block_discovery);
    RUN_TEST(test_announcement_is_the_full_discovery_reply);
    RUN_TEST(test_ignores_everything_else);
    return UNITY_END();
}
