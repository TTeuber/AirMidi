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
        } else {  // release
            uint32_t held = now - b->pressed_at;
            if (b->very_long_fired) {
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

// ---- battery -------------------------------------------------------------------------------------------------

void battery_init(battery_t *b) { memset(b, 0, sizeof *b); }

void battery_update(battery_t *b, uint32_t mv) {
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
