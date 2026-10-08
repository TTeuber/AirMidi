#include "airmidi/ui.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

// ---- button --------------------------------------------------------------------------------------------------

static button_t b;
static uint32_t t;
static button_event_t events[16];
static int n_events;

// Hold the pin at `pressed` for ms milliseconds, sampling every 10 ms, recording events.
static void hold(bool pressed, uint32_t ms) {
    for (uint32_t i = 0; i < ms; i += 10, t += 10) {  // counts, so the clock may wrap
        button_event_t e = button_update(&b, pressed, t);
        if (e) events[n_events++] = e;
    }
}

static void reset(void) {
    button_init(&b);
    t = 1000;
    n_events = 0;
}

static void test_short_press_after_double_window(void) {
    reset();
    hold(true, 150);
    hold(false, 280);
    TEST_ASSERT_EQUAL(0, n_events);  // still inside the double-press window
    hold(false, 100);
    TEST_ASSERT_EQUAL(1, n_events);
    TEST_ASSERT_EQUAL(BUTTON_SHORT, events[0]);
}

static void test_double_press(void) {
    reset();
    hold(true, 100);
    hold(false, 150);
    hold(true, 100);
    hold(false, 500);
    TEST_ASSERT_EQUAL(1, n_events);
    TEST_ASSERT_EQUAL(BUTTON_DOUBLE, events[0]);
}

static void test_bounce_is_ignored(void) {
    reset();
    for (int i = 0; i < 3; i++) hold(true, 10), hold(false, 10);  // contact chatter shorter than the debounce
    hold(true, 100);
    hold(false, 500);
    TEST_ASSERT_EQUAL(1, n_events);
    TEST_ASSERT_EQUAL(BUTTON_SHORT, events[0]);
}

static void test_long_press_fires_on_release(void) {
    reset();
    hold(true, 3000);
    TEST_ASSERT_EQUAL(0, n_events);
    TEST_ASSERT_TRUE(button_held_ms(&b, t) >= 2950);  // minus the debounce
    hold(false, 500);
    TEST_ASSERT_EQUAL(1, n_events);
    TEST_ASSERT_EQUAL(BUTTON_LONG, events[0]);
}

static void test_very_long_fires_while_held_and_only_once(void) {
    reset();
    hold(true, 8100);
    TEST_ASSERT_EQUAL(1, n_events);
    TEST_ASSERT_EQUAL(BUTTON_VERY_LONG, events[0]);
    hold(true, 2000);
    hold(false, 500);
    TEST_ASSERT_EQUAL(1, n_events);  // no LONG on release
}

static void test_short_then_long(void) {
    reset();
    hold(true, 100);
    hold(false, 100);
    hold(true, 2500);  // second press turns into a long one: the first stands alone
    hold(false, 500);
    TEST_ASSERT_EQUAL(2, n_events);
    TEST_ASSERT_EQUAL(BUTTON_SHORT, events[0]);
    TEST_ASSERT_EQUAL(BUTTON_LONG, events[1]);
}

static void test_time_wraps(void) {
    button_init(&b);
    t = 0xFFFFFF00u;  // the ms clock wraps after 49 days
    n_events = 0;
    hold(true, 100);
    hold(false, 500);
    TEST_ASSERT_EQUAL(1, n_events);
    TEST_ASSERT_EQUAL(BUTTON_SHORT, events[0]);
}

// ---- LED -----------------------------------------------------------------------------------------------------

static int lit_ms(led_state_t s, uint32_t from, uint32_t ms, uint32_t activity) {
    int on = 0;
    for (uint32_t i = 0; i < ms; i++) on += led_level(s, from + i, activity);
    return on;
}

static void test_led_patterns_duty(void) {
    TEST_ASSERT_EQUAL(0, lit_ms(LED_OFF, 0, 3000, 0));
    TEST_ASSERT_EQUAL(300, lit_ms(LED_ADVERTISING, 0, 3000, 0));
    TEST_ASSERT_EQUAL(3000, lit_ms(LED_CONNECTED, 1000, 3000, 0));
    TEST_ASSERT_EQUAL(1500, lit_ms(LED_WIFI_JOINING, 0, 3000, 0));
    TEST_ASSERT_EQUAL(400, lit_ms(LED_SETUP, 0, 3000, 0));
    TEST_ASSERT_EQUAL(360, lit_ms(LED_LOW_BATTERY, 0, 4000, 0));
    TEST_ASSERT_EQUAL(1500, lit_ms(LED_OTA, 0, 3000, 0));
}

static void test_activity_flicker(void) {
    TEST_ASSERT_FALSE(led_level(LED_CONNECTED, 5000, 4990));
    TEST_ASSERT_TRUE(led_level(LED_CONNECTED, 5000, 5000 - LED_ACTIVITY_MS));
    TEST_ASSERT_EQUAL(1000 - LED_ACTIVITY_MS, lit_ms(LED_CONNECTED, 5000, 1000, 5000));
}

// ---- battery -------------------------------------------------------------------------------------------------

static void test_percent_curve(void) {
    TEST_ASSERT_EQUAL(100, battery_percent(4250));
    TEST_ASSERT_EQUAL(100, battery_percent(4200));
    TEST_ASSERT_EQUAL(50, battery_percent(3800));
    TEST_ASSERT_EQUAL(45, battery_percent(3775));
    TEST_ASSERT_EQUAL(0, battery_percent(3300));
    TEST_ASSERT_EQUAL(0, battery_percent(3000));
    for (uint32_t mv = 3300; mv < 4200; mv++) TEST_ASSERT_TRUE(battery_percent(mv + 1) >= battery_percent(mv));
}

static void test_low_warning_with_hysteresis(void) {
    battery_t bt;
    battery_init(&bt);
    battery_update(&bt, 3600);
    TEST_ASSERT_EQUAL(3600, bt.mv);
    TEST_ASSERT_FALSE(bt.low);
    for (int i = 0; i < 40; i++) battery_update(&bt, 3480);
    TEST_ASSERT_TRUE(bt.low);
    for (int i = 0; i < 40; i++) battery_update(&bt, 3520);  // back above 3500 but inside the hysteresis
    TEST_ASSERT_TRUE(bt.low);
    for (int i = 0; i < 40; i++) battery_update(&bt, 3600);
    TEST_ASSERT_FALSE(bt.low);
}

static void test_empty_needs_a_sustained_low(void) {
    battery_t bt;
    battery_init(&bt);
    battery_update(&bt, 3320);
    battery_update(&bt, 3000);  // one deep sag (radio burst): the average stays above 3300
    TEST_ASSERT_FALSE(bt.empty);
    for (int i = 0; i < 3; i++) battery_update(&bt, 3320);
    TEST_ASSERT_FALSE(bt.empty);
    for (int i = 0; i < 30; i++) battery_update(&bt, 3250);
    TEST_ASSERT_TRUE(bt.empty);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_short_press_after_double_window);
    RUN_TEST(test_double_press);
    RUN_TEST(test_bounce_is_ignored);
    RUN_TEST(test_long_press_fires_on_release);
    RUN_TEST(test_very_long_fires_while_held_and_only_once);
    RUN_TEST(test_short_then_long);
    RUN_TEST(test_time_wraps);
    RUN_TEST(test_led_patterns_duty);
    RUN_TEST(test_activity_flicker);
    RUN_TEST(test_percent_curve);
    RUN_TEST(test_low_warning_with_hysteresis);
    RUN_TEST(test_empty_needs_a_sustained_low);
    return UNITY_END();
}
