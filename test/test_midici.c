// MIDI-CI: the JSON scanner, message layouts, and the initiator talking to the responder over a simulated link
// that can lose, hold back and inject messages.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "airmidi/json_scan.h"
#include "airmidi/midici.h"
#include "airmidi/midici_initiator.h"
#include "airmidi/midici_responder.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

// --- json_scan ---

static void test_json_finds_keys_at_any_depth_but_not_inside_strings(void) {
    const char *j = "{\"title\":\"a \\\"resource\\\": x\",\"list\":[{\"resource\":\"DeviceInfo\"}],\"status\": 200}";
    char s[32];
    int64_t v;
    TEST_ASSERT_TRUE(json_find_str(j, strlen(j), "resource", s, sizeof s));
    TEST_ASSERT_EQUAL_STRING("DeviceInfo", s);
    TEST_ASSERT_TRUE(json_find_int(j, strlen(j), "status", &v));
    TEST_ASSERT_EQUAL(200, v);
    TEST_ASSERT_FALSE(json_find_int(j, strlen(j), "nope", &v));
    TEST_ASSERT_FALSE(json_find_int(j, strlen(j), "title", &v));  // a string, not an integer
    // an escaped quote does not end a string: without that rule this text has no "status" key at all
    const char *e = "{\"t\":\"\\\"\",\"status\":1}";
    TEST_ASSERT_TRUE(json_find_int(e, strlen(e), "status", &v));
    TEST_ASSERT_EQUAL(1, v);
}

static void test_json_values(void) {
    const char *j = "{\"a\":-12,\"b\":1.5,\"c\":\"x\\ny\",\"d\":\"abcdefgh\",\"e\":4294967295}";
    size_t n = strlen(j);
    char s[8];
    int64_t v;
    TEST_ASSERT_TRUE(json_find_int(j, n, "a", &v));
    TEST_ASSERT_EQUAL(-12, v);
    TEST_ASSERT_FALSE(json_find_int(j, n, "b", &v));           // not an integer
    TEST_ASSERT_FALSE(json_find_str(j, n, "c", s, sizeof s));  // escapes are not unescaped: refused
    TEST_ASSERT_FALSE(json_find_str(j, n, "d", s, sizeof s));  // 8 characters + NUL do not fit 8
    TEST_ASSERT_TRUE(json_find_int(j, n, "e", &v));
    TEST_ASSERT_EQUAL(4294967295LL, v);
}

static void test_json_truncated_text_is_safe(void) {
    const char *j = "{\"resource\":\"Devi";
    char s[32];
    for (size_t n = 0; n <= strlen(j); n++) TEST_ASSERT_FALSE(json_find_str(j, n, "resource", s, sizeof s));
}

// --- message layouts ---

static const midici_ident_t box = {{0x7D, 0, 0}, 0, 1, {0, 5, 0, 0}};

static void test_discovery_layout(void) {
    uint8_t m[32];
    size_t n = midici_discovery(m, 0x0ABCDEF, &box, MIDICI_CAT_PE, 512, 0);
    const uint8_t want[] = {0x7E, 0x7F, 0x0D, 0x70, 0x02, 0x6F, 0x1B, 0x2F, 0x05, 0x7F, 0x7F, 0x7F, 0x7F, 0x7D, 0,
                            0,    0,    0,    1,    0,    0,    5,    0,    0,    0x08, 0,    4,    0,    0,    0};
    TEST_ASSERT_EQUAL(sizeof want, n);
    TEST_ASSERT_EQUAL_MEMORY(want, m, n);
    midici_msg_t msg;
    midici_discovery_t d;
    TEST_ASSERT_TRUE(midici_parse(m, n, &msg));
    TEST_ASSERT_EQUAL_HEX32(0x0ABCDEF, msg.src);
    TEST_ASSERT_EQUAL_HEX32(MIDICI_BROADCAST, msg.dst);
    TEST_ASSERT_TRUE(midici_parse_discovery(&msg, &d));
    TEST_ASSERT_EQUAL(512, d.max_sysex);
    TEST_ASSERT_EQUAL(1, d.ident.model);
}

static void test_get_with_res_id(void) {
    uint8_t m[128];
    size_t n = midici_pe_get(m, 1, 2, 9, "CtrlMapList", "wave");
    midici_msg_t msg;
    midici_pe_t pe;
    TEST_ASSERT_TRUE(midici_parse(m, n, &msg));
    TEST_ASSERT_TRUE(midici_parse_pe(&msg, &pe));
    TEST_ASSERT_EQUAL(9, pe.request_id);
    TEST_ASSERT_EQUAL_STRING_LEN("{\"resource\":\"CtrlMapList\",\"resId\":\"wave\"}", pe.header, pe.header_len);
    TEST_ASSERT_EQUAL(1, pe.chunks);
    TEST_ASSERT_EQUAL(1, pe.this_chunk);
    TEST_ASSERT_EQUAL(0, pe.data_len);
    char longname[MIDICI_NAME_MAX + 1];
    memset(longname, 'a', MIDICI_NAME_MAX);
    longname[MIDICI_NAME_MAX] = 0;
    TEST_ASSERT_EQUAL(0, midici_pe_get(m, 1, 2, 9, longname, NULL));
}

static void test_parse_rejects_what_is_not_midi_ci(void) {
    uint8_t m[32];
    size_t n = midici_discovery(m, 5, &box, 0, 512, 0);
    midici_msg_t msg;
    TEST_ASSERT_FALSE(midici_parse(m, MIDICI_HEADER - 1, &msg));
    m[2] = 0x06;  // Device Inquiry, not MIDI-CI
    TEST_ASSERT_FALSE(midici_parse(m, n, &msg));
    m[2] = 0x0D, m[20] = 0x80;  // a status byte inside
    TEST_ASSERT_FALSE(midici_parse(m, n, &msg));
}

static void test_ack_nak_round_trip(void) {
    uint8_t m[32];
    size_t n = midici_ack_nak(m, MIDICI_ACK, 1, 2, MIDICI_PE_GET, MIDICI_ACK_TIMEOUT_WAIT, 30, 17);
    midici_msg_t msg;
    midici_ack_t a;
    TEST_ASSERT_TRUE(midici_parse(m, n, &msg));
    TEST_ASSERT_EQUAL(MIDICI_ACK, msg.sub2);
    midici_parse_ack(&msg, &a);
    TEST_ASSERT_EQUAL(MIDICI_PE_GET, a.original_sub2);
    TEST_ASSERT_EQUAL(MIDICI_ACK_TIMEOUT_WAIT, a.status);
    TEST_ASSERT_EQUAL(30, a.status_data);
    TEST_ASSERT_EQUAL(17, a.details[0]);
}

// --- initiator <-> responder over a simulated link ---

#define DEV_MUID 0x0123456
#define BOX_MUID 0x0654321
#define MAX_MSG 1024
#define QLEN 256

typedef struct {
    uint8_t to_device;  // 1: initiator -> responder, 0: the other way
    uint16_t n;
    uint8_t b[MAX_MSG];
} wire_msg_t;

static wire_msg_t q[QLEN];
static int qhead, qtail;
static uint32_t now;
static size_t largest_to_box;
static int sent_to_device;
// fault injection
static int drop_to_box_index = -1;  // drop the n-th message the device sends (0-based), -1 none
static int to_box_count;
static bool device_silent_after_caps;
static int pe_sent;

static void push(uint8_t to_device, const uint8_t *b, size_t n) {
    TEST_ASSERT_TRUE(n <= MAX_MSG);
    TEST_ASSERT_TRUE((qtail + 1) % QLEN != qhead);
    wire_msg_t *m = &q[qtail];
    m->to_device = to_device, m->n = (uint16_t)n;
    memcpy(m->b, b, n);
    qtail = (qtail + 1) % QLEN;
}

static void box_send(void *ctx, const uint8_t *sx, size_t n) {
    (void)ctx;
    sent_to_device++;
    push(1, sx, n);
}

static void dev_send(void *ctx, const uint8_t *sx, size_t n) {
    (void)ctx;
    int i = to_box_count++;
    if (n + 2 > largest_to_box) largest_to_box = n + 2;  // F0 and F7 count against the max SysEx size
    if (i == drop_to_box_index) return;
    if (sx[3] == MIDICI_PE_GET_REPLY) pe_sent++;
    push(0, sx, n);
}

// What the box got back.
static struct {
    char resource[MIDICI_MAX_GETS][MIDICI_NAME_MAX], res_id[MIDICI_MAX_GETS][MIDICI_NAME_MAX];
    int status[MIDICI_MAX_GETS];
    char data[MIDICI_MAX_GETS][4096];
    int n;
    int done;  // -1 while running
    bool fetch;
    int devices;
} got;

static bool on_device(void *ctx, uint32_t muid, const midici_discovery_t *d) {
    (void)ctx;
    TEST_ASSERT_EQUAL_HEX32(DEV_MUID, muid);
    TEST_ASSERT_EQUAL(MIDICI_RESPONDER_OUT, d->max_sysex);
    got.devices++;
    return got.fetch;
}

static void on_resource(void *ctx, const char *resource, const char *res_id, int status, const uint8_t *data,
                        size_t len) {
    (void)ctx;
    TEST_ASSERT_TRUE(got.n < MIDICI_MAX_GETS);
    TEST_ASSERT_TRUE(len < sizeof got.data[0]);
    strcpy(got.resource[got.n], resource);
    strcpy(got.res_id[got.n], res_id);
    got.status[got.n] = status;
    memcpy(got.data[got.n], data, len);
    got.data[got.n][len] = 0;
    got.n++;
}

static void on_done(void *ctx, midici_done_t r) {
    (void)ctx;
    got.done = (int)r;
}

static midici_initiator_t in;
static midici_responder_t dev;
static uint8_t rxbuf[4096];
static bool responder_present;

static const char *device_info =
    "{\"manufacturerId\":[125,0,0],\"manufacturer\":\"Educational Use\",\"family\":\"airmidi\","
    "\"model\":\"test device\",\"version\":\"0.1\"}";
static char ctrl_list[3000];  // built in setup_link: long enough for several chunks
static const char *wave_map = "[{\"value\":0,\"title\":\"Saw\"},{\"value\":2147483648,\"title\":\"Square\"}]";
static const char *lfo_map = "[{\"value\":0,\"title\":\"Sine\"},{\"value\":4294967295,\"title\":\"Random\"}]";

static midici_resource_t table[] = {
    {"DeviceInfo", NULL, NULL},
    {"AllCtrlList", NULL, NULL},
    {"CtrlMapList", "wave", NULL},
    {"CtrlMapList", "lfo", NULL},
};

static void setup_link(size_t buf_cap, bool with_responder) {
    qhead = qtail = 0;
    now = 1000;
    largest_to_box = 0;
    sent_to_device = 0;
    drop_to_box_index = -1;
    to_box_count = 0;
    device_silent_after_caps = false;
    pe_sent = 0;
    memset(&got, 0, sizeof got);
    got.done = -1;
    got.fetch = true;
    responder_present = with_responder;
    // 24 controls, two of them valueSelect with a value map
    size_t n = (size_t)snprintf(ctrl_list, sizeof ctrl_list, "[");
    for (int i = 0; i < 24; i++)
        n += (size_t)snprintf(ctrl_list + n, sizeof ctrl_list - n,
                              "%s{\"title\":\"Control %d\",\"channel\":1,\"ctrlType\":\"cc\",\"ctrlIndex\":[%d],"
                              "\"priority\":%d,\"default\":0%s}",
                              i ? "," : "", i, 20 + i, 1 + i % 5,
                              i == 3   ? ",\"typeHint\":\"valueSelect\",\"ctrlMapId\":\"wave\""
                              : i == 9 ? ",\"typeHint\":\"valueSelect\",\"ctrlMapId\":\"lfo\""
                                       : "");
    snprintf(ctrl_list + n, sizeof ctrl_list - n, "]");
    table[0].json = device_info, table[1].json = ctrl_list, table[2].json = wave_map, table[3].json = lfo_map;
    const midici_ident_t dev_id = {{0x7D, 0, 0}, 0, 2, {0, 0, 1, 0}};
    TEST_ASSERT_TRUE(midici_responder_init(&dev, DEV_MUID, &dev_id, table, 4, dev_send, NULL));
    midici_initiator_cb_t cb = {
        .send = box_send, .on_device = on_device, .on_resource = on_resource, .on_done = on_done};
    midici_initiator_init(&in, BOX_MUID, &box, &cb, rxbuf, buf_cap);
}

// Deliver everything queued, 1 ms per message, and tick the initiator; until the scan ends or time runs out.
static void run(uint32_t ms) {
    uint32_t end = now + ms;
    while (now < end && got.done < 0) {
        if (qhead != qtail) {
            wire_msg_t *m = &q[qhead];
            qhead = (qhead + 1) % QLEN;
            if (m->to_device) {
                bool silent = device_silent_after_caps && m->b[3] == MIDICI_PE_GET;
                if (responder_present && !silent) midici_responder_receive(&dev, m->b, m->n);
            } else {
                midici_initiator_receive(&in, m->b, m->n, now);
            }
        }
        now++;
        midici_initiator_tick(&in, now);
    }
}

static int find(const char *resource, const char *res_id) {
    for (int i = 0; i < got.n; i++)
        if (!strcmp(got.resource[i], resource) && !strcmp(got.res_id[i], res_id)) return i;
    return -1;
}

static void test_full_scan_reads_every_resource(void) {
    setup_link(sizeof rxbuf, true);
    midici_initiator_start(&in, now);
    run(10000);
    TEST_ASSERT_EQUAL(MIDICI_DONE_OK, got.done);
    TEST_ASSERT_EQUAL(1, got.devices);
    TEST_ASSERT_EQUAL(5, got.n);  // ResourceList, DeviceInfo, AllCtrlList, 2 x CtrlMapList
    TEST_ASSERT_EQUAL_STRING("ResourceList", got.resource[0]);
    TEST_ASSERT_EQUAL_STRING(
        "[{\"resource\":\"DeviceInfo\"},{\"resource\":\"AllCtrlList\"},"
        "{\"resource\":\"CtrlMapList\",\"requireId\":true}]",
        got.data[0]);
    int i = find("DeviceInfo", "");
    TEST_ASSERT_TRUE(i >= 0);
    TEST_ASSERT_EQUAL(200, got.status[i]);
    TEST_ASSERT_EQUAL_STRING(device_info, got.data[i]);
    i = find("AllCtrlList", "");
    TEST_ASSERT_TRUE(i >= 0);
    TEST_ASSERT_EQUAL_STRING(ctrl_list, got.data[i]);
    TEST_ASSERT_EQUAL_STRING(wave_map, got.data[find("CtrlMapList", "wave")]);
    TEST_ASSERT_EQUAL_STRING(lfo_map, got.data[find("CtrlMapList", "lfo")]);
    // the control list is chunked, and no message is larger than the 512 bytes the box can receive
    size_t room = MIDICI_MAX_SYSEX - MIDICI_PE_OVERHEAD - strlen("{\"status\":200}");
    TEST_ASSERT_EQUAL(4 + (int)((strlen(ctrl_list) + room - 1) / room), pe_sent);
    TEST_ASSERT_TRUE(largest_to_box <= MIDICI_MAX_SYSEX);
    TEST_ASSERT_FALSE(midici_initiator_busy(&in));
}

static void test_cached_device_is_not_read_again(void) {
    setup_link(sizeof rxbuf, true);
    got.fetch = false;
    midici_initiator_start(&in, now);
    run(10000);
    TEST_ASSERT_EQUAL(MIDICI_DONE_CACHED, got.done);
    TEST_ASSERT_EQUAL(0, got.n);
    TEST_ASSERT_EQUAL(1, sent_to_device);  // only the Discovery
}

static void test_nobody_answers(void) {
    setup_link(sizeof rxbuf, false);
    midici_initiator_start(&in, now);
    uint32_t t0 = now;
    run(10000);
    TEST_ASSERT_EQUAL(MIDICI_DONE_NO_DEVICE, got.done);
    TEST_ASSERT_TRUE(now - t0 >= MIDICI_DISCOVERY_WAIT_MS);  // waits the full 3 s the spec asks for
}

static void test_device_without_property_exchange(void) {
    setup_link(sizeof rxbuf, false);
    midici_initiator_start(&in, now);
    uint8_t m[64];
    const midici_ident_t id = {{0x41, 0, 0}, 1, 1, {1, 0, 0, 0}};
    push(0, m, midici_discovery_reply(m, DEV_MUID, BOX_MUID, &id, MIDICI_CAT_PROFILES, 256, 0));
    run(10000);
    TEST_ASSERT_EQUAL(MIDICI_DONE_NO_PE, got.done);
    TEST_ASSERT_EQUAL(0, got.devices);
}

static void test_strays_are_ignored(void) {
    // macOS's own Discovery, a reply for another initiator, and our own Discovery echoed back
    setup_link(sizeof rxbuf, true);
    midici_initiator_start(&in, now);
    uint8_t m[64];
    const midici_ident_t mac = {{0x11, 0, 0}, 0, 0, {0, 0, 0, 0}};
    push(0, m, midici_discovery(m, 0x0777777, &mac, MIDICI_CAT_PE, 4096, 0));
    push(0, m, midici_discovery_reply(m, 0x0888888, 0x0777777, &mac, MIDICI_CAT_PE, 512, 0));
    push(0, m, midici_discovery(m, BOX_MUID, &box, MIDICI_CAT_PE, 512, 0));
    run(10000);
    TEST_ASSERT_EQUAL(MIDICI_DONE_OK, got.done);
    TEST_ASSERT_EQUAL(1, got.devices);
    TEST_ASSERT_EQUAL(5, got.n);
}

static void test_lost_chunk_fails_that_resource_only(void) {
    setup_link(sizeof rxbuf, true);
    // device messages: 0 Discovery reply, 1 caps reply, 2 ResourceList, 3 DeviceInfo, 4.. AllCtrlList chunks
    drop_to_box_index = 5;  // the second chunk of the control list
    midici_initiator_start(&in, now);
    run(20000);
    TEST_ASSERT_EQUAL(MIDICI_DONE_OK, got.done);
    TEST_ASSERT_EQUAL(MIDICI_STATUS_BAD_DATA, got.status[find("AllCtrlList", "")]);
    TEST_ASSERT_EQUAL_STRING("", got.data[find("AllCtrlList", "")]);
    TEST_ASSERT_EQUAL(200, got.status[find("DeviceInfo", "")]);
    TEST_ASSERT_EQUAL(-1, find("CtrlMapList", "wave"));  // nothing to follow from a broken list
}

static void test_device_goes_silent(void) {
    setup_link(sizeof rxbuf, true);
    device_silent_after_caps = true;
    midici_initiator_start(&in, now);
    run(20000);
    TEST_ASSERT_EQUAL(MIDICI_DONE_TIMEOUT, got.done);
    TEST_ASSERT_EQUAL(1, got.n);
    TEST_ASSERT_EQUAL(MIDICI_STATUS_TIMEOUT, got.status[0]);
}

static void test_timeout_wait_ack_extends_the_deadline(void) {
    setup_link(sizeof rxbuf, true);
    device_silent_after_caps = true;
    midici_initiator_start(&in, now);
    run(100);  // Discovery, caps, the ResourceList Get (unanswered)
    TEST_ASSERT_EQUAL(-1, got.done);
    uint8_t m[64];
    // 5 s more for request 1
    push(0, m, midici_ack_nak(m, MIDICI_ACK, DEV_MUID, BOX_MUID, MIDICI_PE_GET, MIDICI_ACK_TIMEOUT_WAIT, 50, 1));
    run(7000);
    TEST_ASSERT_EQUAL(-1, got.done);  // 3 s + 5 s from the Get, not yet
    run(2000);
    TEST_ASSERT_EQUAL(MIDICI_DONE_TIMEOUT, got.done);
}

static void test_invalidate_device_ends_the_scan(void) {
    setup_link(sizeof rxbuf, true);
    device_silent_after_caps = true;
    midici_initiator_start(&in, now);
    run(100);
    uint8_t m[32];
    push(0, m, midici_invalidate(m, DEV_MUID, DEV_MUID));
    run(100);
    TEST_ASSERT_EQUAL(MIDICI_DONE_INVALIDATED, got.done);
}

static void test_reply_larger_than_the_buffer(void) {
    setup_link(1024, true);  // the control list is ~2.5 KB
    midici_initiator_start(&in, now);
    run(20000);
    TEST_ASSERT_EQUAL(MIDICI_DONE_OK, got.done);
    TEST_ASSERT_EQUAL(MIDICI_STATUS_TOO_LARGE, got.status[find("AllCtrlList", "")]);
    TEST_ASSERT_EQUAL(200, got.status[find("DeviceInfo", "")]);
}

static void test_scan_can_run_again(void) {
    setup_link(sizeof rxbuf, true);
    for (int k = 0; k < 3; k++) {
        memset(&got, 0, sizeof got);
        got.done = -1, got.fetch = true;
        midici_initiator_start(&in, now);
        run(10000);
        TEST_ASSERT_EQUAL(MIDICI_DONE_OK, got.done);
        TEST_ASSERT_EQUAL(5, got.n);
    }
}

// --- the responder alone ---

static void test_responder_404_and_nak(void) {
    setup_link(sizeof rxbuf, true);
    uint8_t m[128];
    push(1, m, midici_pe_get(m, BOX_MUID, DEV_MUID, 3, "ProgramList", NULL));
    // Set Property Data: not supported, NAKed with the request id
    size_t n = midici_pe_get(m, BOX_MUID, DEV_MUID, 4, "DeviceInfo", NULL);
    m[3] = 0x36;
    push(1, m, n);
    // a reply addressed to the device: never answered
    n = midici_pe_caps(m, MIDICI_PE_CAPS_REPLY, BOX_MUID, DEV_MUID, 1);
    push(1, m, n);
    while (qhead != qtail) {
        wire_msg_t *w = &q[qhead];
        qhead = (qhead + 1) % QLEN;
        if (w->to_device) midici_responder_receive(&dev, w->b, w->n);
    }
    TEST_ASSERT_EQUAL(2, to_box_count);
}

static void test_responder_chunks_to_the_initiators_size(void) {
    setup_link(sizeof rxbuf, true);
    uint8_t m[64];
    const midici_ident_t small = {{0x7D, 0, 0}, 0, 9, {0}};
    // an initiator that takes only 128-byte messages
    push(1, m, midici_discovery(m, 0x0111111, &small, MIDICI_CAT_PE, 128, 0));
    uint8_t g[128];
    push(1, g, midici_pe_get(g, 0x0111111, DEV_MUID, 1, "AllCtrlList", NULL));
    while (qhead != qtail) {
        wire_msg_t *w = &q[qhead];
        qhead = (qhead + 1) % QLEN;
        if (w->to_device) midici_responder_receive(&dev, w->b, w->n);
    }
    TEST_ASSERT_TRUE(largest_to_box <= 128);
    TEST_ASSERT_TRUE(pe_sent > (int)(strlen(ctrl_list) / 128));
}

// --- robustness: random and mutated input never crashes (ASan/UBSan) and never answers garbage ---

static void test_fuzz(void) {
    srand(1234);
    setup_link(sizeof rxbuf, true);
    midici_initiator_cb_t cb = {.send = box_send};  // no result callbacks: they check for sane input
    midici_initiator_init(&in, BOX_MUID, &box, &cb, rxbuf, sizeof rxbuf);
    uint8_t valid[600], m[600];
    for (int iter = 0; iter < 20000; iter++) {
        size_t n;
        switch (iter % 4) {
            case 0:
                n = midici_pe_get_reply(valid, DEV_MUID, BOX_MUID, (uint8_t)(rand() & 0x7F), "{\"status\":200}",
                                        (uint16_t)(rand() % 4), (uint16_t)(rand() % 4), (const uint8_t *)ctrl_list,
                                        (size_t)(rand() % 400));
                break;
            case 1:
                n = midici_pe_get(valid, BOX_MUID, DEV_MUID, 1, "AllCtrlList", "x");
                break;
            case 2:
                n = midici_discovery_reply(valid, DEV_MUID, BOX_MUID, &box, MIDICI_CAT_PE, 512, 0);
                break;
            default:
                n = (size_t)(rand() % 64);
                for (size_t i = 0; i < n; i++) valid[i] = (uint8_t)(rand() & 0x7F);
                if (n > 3) valid[0] = 0x7E, valid[2] = 0x0D;
        }
        memcpy(m, valid, n);
        int flips = rand() % 4;
        for (int f = 0; f < flips && n; f++) m[rand() % n] = (uint8_t)(rand() & 0x7F);
        if (n && rand() % 4 == 0) n = (size_t)(rand() % (int)n);  // cut short
        if (iter % 50 == 0) midici_initiator_start(&in, now);
        midici_initiator_receive(&in, m, n, now);
        midici_responder_receive(&dev, m, n);
        now += 7;
        midici_initiator_tick(&in, now);
        qhead = qtail = 0;  // drop whatever was sent
    }
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_json_finds_keys_at_any_depth_but_not_inside_strings);
    RUN_TEST(test_json_values);
    RUN_TEST(test_json_truncated_text_is_safe);
    RUN_TEST(test_discovery_layout);
    RUN_TEST(test_get_with_res_id);
    RUN_TEST(test_parse_rejects_what_is_not_midi_ci);
    RUN_TEST(test_ack_nak_round_trip);
    RUN_TEST(test_full_scan_reads_every_resource);
    RUN_TEST(test_cached_device_is_not_read_again);
    RUN_TEST(test_nobody_answers);
    RUN_TEST(test_device_without_property_exchange);
    RUN_TEST(test_strays_are_ignored);
    RUN_TEST(test_lost_chunk_fails_that_resource_only);
    RUN_TEST(test_device_goes_silent);
    RUN_TEST(test_timeout_wait_ack_extends_the_deadline);
    RUN_TEST(test_invalidate_device_ends_the_scan);
    RUN_TEST(test_reply_larger_than_the_buffer);
    RUN_TEST(test_scan_can_run_again);
    RUN_TEST(test_responder_404_and_nak);
    RUN_TEST(test_responder_chunks_to_the_initiators_size);
    RUN_TEST(test_fuzz);
    return UNITY_END();
}
