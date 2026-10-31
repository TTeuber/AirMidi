#include "airmidi/ui.h"

#include <string.h>

// ---- button --------------------------------------------------------------------------------------------------

void button_init(button_t *b) { memset(b, 0, sizeof *b); }

button_event_t button_update(button_t *b, bool pressed, uint32_t now) {
    if (pressed != b->raw) b->raw = pressed, b->raw_since = now;
    button_event_t ev = BUTTON_NONE;

    if (b->raw != b->stable && now - b->raw_since >= BUTTON_DEBOUNCE_MS) {
        b->stable = b->raw;
        if (b->stable) {  // press
            b->pressed_at = now;
            b->very_long_fired = false;
            if (b->fast_double && b->short_pending) {
                b->short_pending = false;
                b->was_double = true;
                ev = BUTTON_DOUBLE;
            }
        } else {  // release
            uint32_t held = now - b->pressed_at;
            if (b->was_double) {
                b->was_double = false;  // reported at the press
            } else if (b->very_long_fired) {
                // already reported at 8 s
            } else if (held >= BUTTON_LONG_MS) {
                ev = BUTTON_LONG;
            } else if (b->short_pending) {
                b->short_pending = false;
                ev = BUTTON_DOUBLE;
            } else {
                b->short_pending = true;
                b->released_at = now;
            }
        }
    }
    if (ev) return ev;

    if (b->stable && !b->very_long_fired && now - b->pressed_at >= BUTTON_VERY_LONG_MS) {
        b->very_long_fired = true;
        b->short_pending = false;
        return BUTTON_VERY_LONG;
    }
    // A short press becomes SHORT once the window for a second press has passed (and no second press started).
    if (b->short_pending && !b->stable && now - b->released_at >= BUTTON_DOUBLE_MS) {
        b->short_pending = false;
        return BUTTON_SHORT;
    }
    // A long press after a short one: the short one stands on its own.
    if (b->short_pending && b->stable && now - b->pressed_at >= BUTTON_LONG_MS) {
        b->short_pending = false;
        return BUTTON_SHORT;
    }
    return BUTTON_NONE;
}

uint32_t button_held_ms(const button_t *b, uint32_t now) { return b->stable ? now - b->pressed_at : 0; }

// ---- LED -----------------------------------------------------------------------------------------------------

// n flashes of on_ms, off_ms apart, then dark for the rest of the period
static bool flashes(uint32_t now, uint32_t period, int n, uint32_t on_ms, uint32_t gap_ms) {
    uint32_t t = now % period;
    for (int i = 0; i < n; i++) {
        uint32_t start = i * (on_ms + gap_ms);
        if (t >= start && t < start + on_ms) return true;
    }
    return false;
}

bool led_level(led_state_t s, uint32_t now, uint32_t last_activity) {
    switch (s) {
        case LED_ADVERTISING:
            return flashes(now, 1000, 1, 100, 0);
        case LED_CONNECTED:
            return !(last_activity && now - last_activity < LED_ACTIVITY_MS);
        case LED_WIFI_JOINING:
            return now % 500 < 250;
        case LED_SETUP:
            return flashes(now, 1500, 2, 100, 150);
        case LED_LOW_BATTERY:
            return flashes(now, 2000, 3, 60, 120);
        case LED_OTA:
            return now % 100 < 50;
        default:
            return false;
    }
}

// ---- panel ---------------------------------------------------------------------------------------------------

#define ON 255

static uint8_t blink(uint32_t now, uint32_t period, uint8_t level) { return now % period < period / 2 ? level : 0; }

// The battery bar: a quarter per LED, left to right, at least one lit. Without a cell reading (the switch is off and
// USB powers the box) all four blink together instead.
static uint8_t bar_level(const panel_state_t *s, int key, uint32_t now) {
    if (s->battery_absent) return blink(now, 1000, ON);
    int n = (s->battery_percent + 24) / 25;
    if (n < 1) n = 1;
    return key < n ? ON : 0;
}

static uint8_t ble_level(const panel_state_t *s, uint32_t now) {
    if (!s->ble_on) return 0;
    if (s->ble_pairing) return blink(now, 200, ON);
    if (s->ble_connected) return s->last_activity_ms && now - s->last_activity_ms < LED_ACTIVITY_MS ? 0 : ON;
    return flashes(now, 1000, 1, 100, 0) ? ON : 0;  // advertising
}

static uint8_t result_level(panel_result_t r, uint32_t since) {
    if (r == PANEL_RESULT_OK) return ON;
    return flashes(since, PANEL_RESULT_MS, 3, 60, 120) ? ON : 0;  // three quick flashes: nothing / failed
}

static uint8_t net_level(const panel_state_t *s, uint32_t now) {
    if (s->update_installing) return blink(now, 100, ON);
    if (s->update_checking) return blink(now, 200, ON);
    if (s->update_result && now - s->update_at < PANEL_RESULT_MS)
        return result_level(s->update_result, now - s->update_at);
    switch (s->net) {
        case PANEL_NET_JOINING:
            return blink(now, 500, ON);
        case PANEL_NET_JOINED:
            if (!s->net_session) return PANEL_DIM;
            return s->last_activity_ms && now - s->last_activity_ms < LED_ACTIVITY_MS ? 0 : ON;
        default:
            return 0;  // the access point is STP's
    }
}

static uint8_t stp_level(const panel_state_t *s, uint32_t now) {
    if (s->scanning) return blink(now, 250, PANEL_DIM);
    if (s->scan_result && now - s->scan_at < PANEL_RESULT_MS) return result_level(s->scan_result, now - s->scan_at);
    if (s->net == PANEL_NET_AP) return flashes(now, 1500, 2, 100, 150) ? ON : 0;
    return 0;
}

static uint8_t pwr_level(const panel_state_t *s, uint32_t now) {
    if (s->battery_low) return flashes(now, 2000, 3, 60, 120) ? ON : 0;
    return ON;
}

void panel_leds(const panel_state_t *s, uint32_t now, uint8_t level[PANEL_KEYS]) {
    uint32_t since = now - s->sweep_from;
    if (since < 2 * PANEL_KEYS * PANEL_SWEEP_MS) {  // boot sweep, left to right twice
        for (int k = 0; k < PANEL_KEYS; k++) level[k] = (int)(since / PANEL_SWEEP_MS % PANEL_KEYS) == k ? ON : 0;
        return;
    }
    if (s->bar_until && (int32_t)(s->bar_until - now) > 0) {
        for (int k = 0; k < PANEL_KEYS; k++) level[k] = bar_level(s, k, now);
        return;
    }
    level[PANEL_BLE] = ble_level(s, now);
    level[PANEL_NET] = net_level(s, now);
    level[PANEL_STP] = stp_level(s, now);
    level[PANEL_PWR] = pwr_level(s, now);
    for (int k = 0; k < PANEL_KEYS; k++)  // held long enough for its hold action: steady, so you know to let go
        if (s->held_ms[k] >= BUTTON_LONG_MS) level[k] = ON;
}

// ---- battery -------------------------------------------------------------------------------------------------

void battery_init(battery_t *b) { memset(b, 0, sizeof *b); }

void battery_update(battery_t *b, uint32_t mv) {
    if (mv < BATTERY_ABSENT_MV) {  // no cell: forget the average, so a cell switched in later starts fresh
        b->mv = 0;
        b->absent = true;
        b->low = b->empty = false;
        b->below_empty = 0;
        return;
    }
    b->absent = false;
    // Exponential average over ~8 samples: smooths ADC noise and short sags while radios transmit.
    b->mv = b->mv ? (b->mv * 7 + mv + 4) / 8 : mv;
    if (b->mv < BATTERY_WARN_MV) b->low = true;
    if (b->mv >= BATTERY_WARN_MV + BATTERY_HYSTERESIS_MV) b->low = false;
    b->below_empty = b->mv < BATTERY_EMPTY_MV ? b->below_empty + 1 : 0;
    if (b->below_empty >= BATTERY_EMPTY_SAMPLES) b->empty = true;
}

int battery_percent(uint32_t mv) {
    // Typical LiCoO2 / LiPo resting voltage vs state of charge.
    static const uint16_t curve[][2] = {{4200, 100}, {4100, 90}, {4000, 80}, {3900, 65}, {3800, 50}, {3750, 40},
                                        {3700, 30},  {3650, 20}, {3600, 12}, {3500, 5},  {3400, 2},  {3300, 0}};
    const int n = sizeof curve / sizeof curve[0];
    if (mv >= curve[0][0]) return 100;
    if (mv <= curve[n - 1][0]) return 0;
    for (int i = 1; i < n; i++) {
        if (mv >= curve[i][0]) {  // linear between curve[i] and curve[i-1]
            uint32_t v0 = curve[i][0], v1 = curve[i - 1][0], p0 = curve[i][1], p1 = curve[i - 1][1];
            return (int)(p0 + (mv - v0) * (p1 - p0) / (v1 - v0));
        }
    }
    return 0;
}
