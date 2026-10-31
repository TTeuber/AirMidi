#include <string.h>

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

// The panel's panic: the double fires at the second press, and its release adds nothing.
static void test_fast_double_fires_at_second_press(void) {
    reset();
    b.fast_double = true;
    hold(true, 100);
    hold(false, 150);
    hold(true, 30);  // debounce passed: reported already
    TEST_ASSERT_EQUAL(1, n_events);
    TEST_ASSERT_EQUAL(BUTTON_DOUBLE, events[0]);
    hold(true, 100);
    hold(false, 500);
    TEST_ASSERT_EQUAL(1, n_events);
}

static void test_fast_double_leaves_short_and_long_alone(void) {
    reset();
    b.fast_double = true;
    hold(true, 100);
    hold(false, 500);
    hold(true, 2500);
    hold(false, 500);
    TEST_ASSERT_EQUAL(2, n_events);
    TEST_ASSERT_EQUAL(BUTTON_SHORT, events[0]);
    TEST_ASSERT_EQUAL(BUTTON_LONG, events[1]);
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

// ---- panel ---------------------------------------------------------------------------------------------------

static panel_state_t ps;

static void panel_reset(void) {
    memset(&ps, 0, sizeof ps);
    ps.battery_percent = 80;
}

static int panel_lit_ms(int key, uint32_t from, uint32_t ms) {
    int on = 0;
    uint8_t lv[PANEL_KEYS];
    for (uint32_t i = 0; i < ms; i++) panel_leds(&ps, from + i, lv), on += lv[key] > 0;
    return on;
}

static void test_panel_boot_sweep(void) {
    panel_reset();
    ps.sweep_from = 100;
    uint8_t lv[PANEL_KEYS];
    for (int k = 0; k < 2 * PANEL_KEYS; k++) {
        panel_leds(&ps, 100 + k * PANEL_SWEEP_MS + 10, lv);
        for (int j = 0; j < PANEL_KEYS; j++) TEST_ASSERT_EQUAL(j == k % PANEL_KEYS ? 255 : 0, lv[j]);
    }
    panel_leds(&ps, 100 + 2 * PANEL_KEYS * PANEL_SWEEP_MS, lv);
    TEST_ASSERT_EQUAL(255, lv[PANEL_PWR]);  // sweep over: PWR on, the rest show their state (all off)
    TEST_ASSERT_EQUAL(0, lv[PANEL_BLE]);
}

static void test_panel_mode_leds(void) {
    panel_reset();
    ps.ble_on = true;
    TEST_ASSERT_EQUAL(300, panel_lit_ms(PANEL_BLE, 2000, 3000));  // advertising
    ps.ble_connected = true;
    TEST_ASSERT_EQUAL(3000, panel_lit_ms(PANEL_BLE, 2000, 3000));
    ps.ble_pairing = true;
    TEST_ASSERT_EQUAL(1500, panel_lit_ms(PANEL_BLE, 2000, 3000));
    TEST_ASSERT_EQUAL(0, panel_lit_ms(PANEL_NET, 2000, 3000));
    ps.net = PANEL_NET_JOINED;
    uint8_t lv[PANEL_KEYS];
    panel_leds(&ps, 5000, lv);
    TEST_ASSERT_EQUAL(PANEL_DIM, lv[PANEL_NET]);  // joined, no session
    ps.net_session = true;
    panel_leds(&ps, 5000, lv);
    TEST_ASSERT_EQUAL(255, lv[PANEL_NET]);
    ps.net = PANEL_NET_AP;
    TEST_ASSERT_EQUAL(0, panel_lit_ms(PANEL_NET, 2000, 3000));
    TEST_ASSERT_EQUAL(400, panel_lit_ms(PANEL_STP, 2000, 3000));  // the access point: double flash
}

static void test_panel_results_then_back(void) {
    panel_reset();
    ps.scan_result = PANEL_RESULT_OK;
    ps.scan_at = 5000;
    TEST_ASSERT_EQUAL(PANEL_RESULT_MS, panel_lit_ms(PANEL_STP, 5000, 3000));  // found: steady, then dark
    ps.scan_result = PANEL_RESULT_FAILED;
    TEST_ASSERT_EQUAL(180, panel_lit_ms(PANEL_STP, 5000, 3000));  // nothing: three flashes
    ps.update_result = PANEL_RESULT_OK;
    ps.update_at = 5000;
    ps.net = PANEL_NET_JOINED;
    TEST_ASSERT_EQUAL(3000, panel_lit_ms(PANEL_NET, 5000, 3000));  // up to date, then joined (dim)
}

static void test_panel_battery_bar(void) {
    panel_reset();
    ps.bar_until = 8000;
    uint8_t lv[PANEL_KEYS];
    int expect[][2] = {{100, 4}, {80, 4}, {76, 4}, {75, 3}, {51, 3}, {50, 2}, {26, 2}, {25, 1}, {5, 1}, {0, 1}};
    for (unsigned i = 0; i < sizeof expect / sizeof expect[0]; i++) {
        ps.battery_percent = expect[i][0];
        panel_leds(&ps, 6000, lv);
        for (int k = 0; k < PANEL_KEYS; k++) TEST_ASSERT_EQUAL_MESSAGE(k < expect[i][1] ? 255 : 0, lv[k], "bar");
    }
    ps.battery_absent = true;  // on USB, switch off: all four blink together
    TEST_ASSERT_EQUAL(1000, panel_lit_ms(PANEL_BLE, 6000, 2000));
    TEST_ASSERT_EQUAL(1000, panel_lit_ms(PANEL_PWR, 6000, 2000));
    ps.battery_absent = false;
    panel_leds(&ps, 8000, lv);  // bar over
    TEST_ASSERT_EQUAL(0, lv[PANEL_BLE]);
}

static void test_panel_hold_lights_its_led(void) {
    panel_reset();
    ps.held_ms[PANEL_NET] = BUTTON_LONG_MS - 1;
    uint8_t lv[PANEL_KEYS];
    panel_leds(&ps, 5000, lv);
    TEST_ASSERT_EQUAL(0, lv[PANEL_NET]);
    ps.held_ms[PANEL_NET] = BUTTON_LONG_MS;
    panel_leds(&ps, 5000, lv);
    TEST_ASSERT_EQUAL(255, lv[PANEL_NET]);
}

static void test_panel_low_battery(void) {
    panel_reset();
    ps.battery_low = true;
    TEST_ASSERT_EQUAL(360, panel_lit_ms(PANEL_PWR, 2000, 4000));
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

// Regression: on USB with the power switch off, A0 reads ~0 V. That once shut the box down after 5 s.
static void test_no_cell_is_absent_not_empty(void) {
    battery_t bt;
    battery_init(&bt);
    for (int i = 0; i < 30; i++) battery_update(&bt, 12);
    TEST_ASSERT_TRUE(bt.absent);
    TEST_ASSERT_FALSE(bt.empty);
    TEST_ASSERT_FALSE(bt.low);
    battery_update(&bt, 3900);  // switch turned on: fresh average, not dragged down by the zeros
    TEST_ASSERT_FALSE(bt.absent);
    TEST_ASSERT_EQUAL(3900, bt.mv);
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
    RUN_TEST(test_fast_double_fires_at_second_press);
    RUN_TEST(test_fast_double_leaves_short_and_long_alone);
    RUN_TEST(test_led_patterns_duty);
    RUN_TEST(test_activity_flicker);
    RUN_TEST(test_panel_boot_sweep);
    RUN_TEST(test_panel_mode_leds);
    RUN_TEST(test_panel_results_then_back);
    RUN_TEST(test_panel_battery_bar);
    RUN_TEST(test_panel_hold_lights_its_led);
    RUN_TEST(test_panel_low_battery);
    RUN_TEST(test_percent_curve);
    RUN_TEST(test_low_warning_with_hysteresis);
    RUN_TEST(test_empty_needs_a_sustained_low);
    RUN_TEST(test_no_cell_is_absent_not_empty);
    return UNITY_END();
}
