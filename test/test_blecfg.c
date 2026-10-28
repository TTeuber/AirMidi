#include <string.h>

#include "airmidi/blecfg.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static uint8_t buf[64];
static blecfg_rx_t rx;

// Split msg at piece_max and feed every piece; returns the last result.
static blecfg_result_t roundtrip(uint8_t id, const char *msg, size_t piece_max, int *pieces) {
    size_t len = strlen(msg), off = 0;
    uint8_t piece[256];
    blecfg_result_t r = BLECFG_BAD;
    *pieces = 0;
    do {
        size_t n = blecfg_piece(id, (const uint8_t *)msg, len, &off, piece_max, piece);
        TEST_ASSERT_TRUE(n <= piece_max);
        r = blecfg_rx_feed(&rx, piece, n);
        (*pieces)++;
        if (off < len) TEST_ASSERT_EQUAL(BLECFG_MORE, r);
    } while (off < len);
    return r;
}

static void test_short_message_is_one_piece(void) {
    blecfg_rx_init(&rx, buf, sizeof buf);
    int pieces;
    TEST_ASSERT_EQUAL(BLECFG_DONE, roundtrip(7, "GET /api/status\n", 20, &pieces));
    TEST_ASSERT_EQUAL(1, pieces);
    TEST_ASSERT_EQUAL(7, rx.id);
    TEST_ASSERT_EQUAL_MEMORY("GET /api/status\n", rx.buf, rx.len);
}

static void test_long_message_splits_and_reassembles(void) {
    blecfg_rx_init(&rx, buf, sizeof buf);
    const char *m = "POST /api/wifi/try\n{\"ssid\":\"home\",\"pass\":\"correct horse\"}";
    int pieces;
    TEST_ASSERT_EQUAL(BLECFG_DONE, roundtrip(1, m, 8, &pieces));  // 6 bytes of message per piece
    TEST_ASSERT_EQUAL((strlen(m) + 5) / 6, pieces);
    TEST_ASSERT_EQUAL(strlen(m), rx.len);
    TEST_ASSERT_EQUAL_MEMORY(m, rx.buf, rx.len);
}

static void test_empty_message_is_a_header_alone(void) {
    blecfg_rx_init(&rx, buf, sizeof buf);
    uint8_t piece[8];
    size_t off = 0;
    TEST_ASSERT_EQUAL(BLECFG_HEADER, blecfg_piece(3, NULL, 0, &off, 20, piece));
    TEST_ASSERT_EQUAL(BLECFG_LAST, piece[1]);
    TEST_ASSERT_EQUAL(BLECFG_DONE, blecfg_rx_feed(&rx, piece, 2));
    TEST_ASSERT_EQUAL(0, rx.len);
}

static void test_new_id_drops_an_unfinished_message(void) {
    blecfg_rx_init(&rx, buf, sizeof buf);
    const uint8_t a[] = {1, 0, 'G', 'E'};  // first piece of message 1, never finished
    const uint8_t b[] = {2, BLECFG_LAST, 'O', 'K'};
    TEST_ASSERT_EQUAL(BLECFG_MORE, blecfg_rx_feed(&rx, a, sizeof a));
    TEST_ASSERT_EQUAL(BLECFG_DONE, blecfg_rx_feed(&rx, b, sizeof b));
    TEST_ASSERT_EQUAL(2, rx.id);
    TEST_ASSERT_EQUAL_MEMORY("OK", rx.buf, 2);
}

static void test_too_long_is_reported_once_complete(void) {
    blecfg_rx_init(&rx, buf, 10);
    int pieces;
    TEST_ASSERT_EQUAL(BLECFG_TOO_LONG, roundtrip(4, "POST /api/settings\n{\"name\":\"x\"}", 8, &pieces));
    TEST_ASSERT_EQUAL(BLECFG_DONE, roundtrip(5, "GET /x\n", 20, &pieces));  // and the next one is fine
}

static void test_runt_piece_is_bad(void) {
    blecfg_rx_init(&rx, buf, sizeof buf);
    const uint8_t p[] = {1};
    TEST_ASSERT_EQUAL(BLECFG_BAD, blecfg_rx_feed(&rx, p, 1));
}

static void test_parse_request(void) {
    char method[8], path[32];
    const uint8_t *body;
    size_t blen;
    const char *m = "POST /api/mode\n{\"mode\":\"network\"}";
    TEST_ASSERT_TRUE(
        blecfg_parse_request((const uint8_t *)m, strlen(m), method, sizeof method, path, sizeof path, &body, &blen));
    TEST_ASSERT_EQUAL_STRING("POST", method);
    TEST_ASSERT_EQUAL_STRING("/api/mode", path);
    TEST_ASSERT_EQUAL(strlen("{\"mode\":\"network\"}"), blen);
    TEST_ASSERT_EQUAL_MEMORY("{\"mode\":\"network\"}", body, blen);

    const char *g = "GET /api/wifi?ssid=a%20b\n";
    TEST_ASSERT_TRUE(
        blecfg_parse_request((const uint8_t *)g, strlen(g), method, sizeof method, path, sizeof path, &body, &blen));
    TEST_ASSERT_EQUAL_STRING("/api/wifi?ssid=a%20b", path);
    TEST_ASSERT_EQUAL(0, blen);

    const char *bad[] = {"GET /api/status", "GET\n", " /x\n", "GET \n"};
    for (int i = 0; i < 4; i++)
        TEST_ASSERT_FALSE(blecfg_parse_request((const uint8_t *)bad[i], strlen(bad[i]), method, sizeof method, path,
                                               sizeof path, &body, &blen));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_short_message_is_one_piece);
    RUN_TEST(test_long_message_splits_and_reassembles);
    RUN_TEST(test_empty_message_is_a_header_alone);
    RUN_TEST(test_new_id_drops_an_unfinished_message);
    RUN_TEST(test_too_long_is_reported_once_complete);
    RUN_TEST(test_runt_piece_is_bad);
    RUN_TEST(test_parse_request);
    return UNITY_END();
}
