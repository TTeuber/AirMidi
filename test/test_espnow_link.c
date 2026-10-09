#include <stdlib.h>
#include <string.h>

#include "airmidi/espnow_link.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

// ---- a fake air: frames reach every box on the same channel (unicast: only the addressee) ----------------------

typedef struct {
    espnow_link_t l;
    uint8_t mac[6];
    uint8_t channel;
    ump_t got[1024];
    int n_got, ups, downs;
    char peer_name[32];
} box_t;

#define BOXES 3
static box_t boxes[BOXES];
static int n_boxes;
static int drop_pct, dup_pct;  // what the air does to unicast data frames

static void air_send(void *ctx, const uint8_t mac[6], const uint8_t *data, size_t len);

static void on_deliver(void *ctx, const ump_t *u) {
    box_t *b = ctx;
    TEST_ASSERT_TRUE(b->n_got < 1024);
    b->got[b->n_got++] = *u;
}

static void on_peer(void *ctx, const uint8_t mac[6], bool up, const char *name) {
    box_t *b = ctx;
    (void)mac;
    up ? b->ups++ : b->downs++;
    strcpy(b->peer_name, name);
}

static void on_channel(void *ctx, uint8_t ch) { ((box_t *)ctx)->channel = ch; }

static uint32_t now;

static void air_send(void *ctx, const uint8_t mac[6], const uint8_t *data, size_t len) {
    box_t *from = ctx;
    for (int i = 0; i < n_boxes; i++) {
        box_t *to = &boxes[i];
        if (to == from || to->channel != from->channel) continue;
        if (mac && memcmp(mac, to->mac, 6)) continue;
        if (mac && rand() % 100 < drop_pct) continue;
        espnow_link_receive(&to->l, from->mac, data, len, now);
        if (mac && rand() % 100 < dup_pct) espnow_link_receive(&to->l, from->mac, data, len, now);  // retry
    }
}

static box_t *add_box(const char *group, const char *name, uint8_t id) {
    box_t *b = &boxes[n_boxes++];
    memset(b, 0, sizeof *b);
    uint8_t mac[6] = {0x10, 0xBD, 0xA3, 0, 0, id};
    memcpy(b->mac, mac, 6);
    espnow_callbacks_t cb = {air_send, on_deliver, on_peer, on_channel};
    espnow_link_init(&b->l, group, name, &cb, b, now);
    return b;
}

static void reset(void) {
    n_boxes = 0;
    now = 0;
    drop_pct = dup_pct = 0;
    srand(1);
}

static void run(uint32_t ms) {
    for (uint32_t end = now + ms; now < end; now += 10)
        for (int i = 0; i < n_boxes; i++) espnow_link_tick(&boxes[i].l, now);
}

// ---- tests ---------------------------------------------------------------------------------------------------

static void test_boxes_in_one_group_find_each_other(void) {
    reset();
    box_t *a = add_box("airmidi", "airmidi-7530", 1), *b = add_box("airmidi", "airmidi-6dac", 2);
    box_t *c = add_box("stage", "airmidi-0001", 3);  // another cable in the same room
    run(1500);
    TEST_ASSERT_EQUAL(1, espnow_link_peer_count(&a->l));
    TEST_ASSERT_EQUAL(1, espnow_link_peer_count(&b->l));
    TEST_ASSERT_EQUAL(0, espnow_link_peer_count(&c->l));
    TEST_ASSERT_EQUAL_STRING("airmidi-6dac", a->peer_name);
    TEST_ASSERT_EQUAL_STRING("airmidi-7530", b->peer_name);
}

// UMPs of every size, enough for several frames, arrive in order and whole.
static void test_midi_goes_across_in_order(void) {
    reset();
    box_t *a = add_box("airmidi", "a", 1), *b = add_box("airmidi", "b", 2);
    run(1500);
    ump_t u[200];
    for (int i = 0; i < 200; i++) {
        if (i % 3 == 0) u[i] = ump_m1_cv(0, 0x90, i % 128, 1);
        if (i % 3 == 1) u[i] = ump_m2_cv(0, 0xB, 0, 7, 0, (uint32_t)i << 20);
        if (i % 3 == 2) u[i] = ump_sysex7(0, UMP_SYSEX_COMPLETE, (const uint8_t *)"\x01\x02\x03\x04\x05\x06", 6);
    }
    TEST_ASSERT_EQUAL(1, espnow_link_send(&a->l, u, 200, now));
    TEST_ASSERT_TRUE(a->l.stats.tx_frames > 4);  // 333 words: 6 frames
    TEST_ASSERT_EQUAL(200, b->n_got);
    for (int i = 0; i < 200; i++) TEST_ASSERT_TRUE(ump_equal(&u[i], &b->got[i]));
    TEST_ASSERT_EQUAL(0, b->l.stats.rx_lost);
}

// The radio retries a frame whose acknowledgement it missed: the copy is dropped. A frame lost for good is counted.
static void test_duplicates_dropped_losses_counted(void) {
    reset();
    box_t *a = add_box("airmidi", "a", 1), *b = add_box("airmidi", "b", 2);
    run(1500);
    drop_pct = 20, dup_pct = 30;
    for (int i = 0; i < 500; i++) {
        ump_t u = ump_m1_cv(0, 0x90, i % 128, 1 + i / 128);
        espnow_link_send(&a->l, &u, 1, now);
    }
    TEST_ASSERT_TRUE(b->l.stats.rx_dups > 50);
    TEST_ASSERT_TRUE(b->l.stats.rx_lost > 50);
    // Everything up to the last message that arrived was delivered or counted lost (later losses cannot be seen).
    const ump_t *last = &b->got[b->n_got - 1];
    int last_index = ump_byte3(last) + 128 * (ump_byte4(last) - 1);
    TEST_ASSERT_EQUAL(last_index + 1, b->n_got + (int)b->l.stats.rx_lost);
    for (int i = 1; i < b->n_got; i++) {  // in order, never twice
        int x = ump_byte3(&b->got[i - 1]) + 128 * (ump_byte4(&b->got[i - 1]) - 1);
        int y = ump_byte3(&b->got[i]) + 128 * (ump_byte4(&b->got[i]) - 1);
        TEST_ASSERT_TRUE(y > x);
    }
}

static void test_silent_peer_goes_away(void) {
    reset();
    box_t *a = add_box("airmidi", "a", 1);
    add_box("airmidi", "b", 2);
    run(1500);
    n_boxes = 1;  // b is switched off
    run(ESPNOW_PEER_TIMEOUT_MS + 100);
    TEST_ASSERT_EQUAL(0, espnow_link_peer_count(&a->l));
    TEST_ASSERT_EQUAL(1, a->downs);
    ump_t u = ump_m1_cv(0, 0x90, 1, 1);
    TEST_ASSERT_EQUAL(0, espnow_link_send(&a->l, &u, 1, now));
}

// A box on an access point's channel stays there; the other one looks channel by channel until it hears it, then
// stays too. Two boxes that are both free find each other as well, whatever channel they start on.
static void test_free_box_finds_one_on_an_access_point_channel(void) {
    for (uint8_t ap = 1; ap <= 13; ap++) {
        reset();
        box_t *net = add_box("airmidi", "in-network", 1), *play = add_box("airmidi", "in-play", 2);
        espnow_link_set_ap_channel(&net->l, ap, now);
        net->channel = ap;
        run(2 * 12 * 2100);  // the worst case: the whole sweep
        TEST_ASSERT_EQUAL_MESSAGE(1, espnow_link_peer_count(&play->l), "not found");
        TEST_ASSERT_EQUAL(ap, play->channel);
        TEST_ASSERT_EQUAL(ap, net->channel);
        uint8_t before = play->channel;
        run(10000);
        TEST_ASSERT_EQUAL(before, play->channel);  // no hopping while it has a peer
    }
}

static void test_two_free_boxes_meet(void) {
    reset();
    box_t *a = add_box("airmidi", "airmidi-7530", 1), *b = add_box("airmidi", "airmidi-6dac", 2);
    b->l.hop_index = 5, b->l.channel = b->channel = 3;  // started at another time: out of step, sweeping
    run(30000);
    TEST_ASSERT_EQUAL(1, espnow_link_peer_count(&a->l));
    TEST_ASSERT_EQUAL(a->channel, b->channel);
}

static void test_garbage_is_ignored(void) {
    reset();
    box_t *a = add_box("airmidi", "a", 1), *b = add_box("airmidi", "b", 2);
    run(1500);
    for (int i = 0; i < 20000; i++) {
        uint8_t f[ESPNOW_FRAME_MAX];
        size_t len = (size_t)(rand() % sizeof f);
        for (size_t k = 0; k < len; k++) f[k] = (uint8_t)rand();
        if (len > 4 && rand() % 2) f[0] = 'a', f[1] = 'M', f[2] = 1, f[3] = (uint8_t)(1 + rand() % 2);
        espnow_link_receive(&a->l, b->mac, f, len, now);
        a->n_got = 0;
    }
    TEST_ASSERT_TRUE(espnow_link_peer_count(&a->l) <= ESPNOW_PEERS);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_boxes_in_one_group_find_each_other);
    RUN_TEST(test_midi_goes_across_in_order);
    RUN_TEST(test_duplicates_dropped_losses_counted);
    RUN_TEST(test_silent_peer_goes_away);
    RUN_TEST(test_free_box_finds_one_on_an_access_point_channel);
    RUN_TEST(test_two_free_boxes_meet);
    RUN_TEST(test_garbage_is_ignored);
    return UNITY_END();
}
