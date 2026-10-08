// The box's whole user interface: one button and one LED. Pure logic on (input, time), so the gestures and the
// blink patterns are host-tested; the firmware samples the pin and drives the LED.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- button --------------------------------------------------------------------------------------------------

typedef enum {
    BUTTON_NONE,
    BUTTON_SHORT,      // pressed < 2 s, no second press within the double-press window: MIDI-CI rescan
    BUTTON_DOUBLE,     // two short presses within the window: all notes off on every output (panic)
    BUTTON_LONG,       // released after 2-8 s: next mode
    BUTTON_VERY_LONG,  // still held at 8 s (fires then, not on release): Setup mode
} button_event_t;

#define BUTTON_DEBOUNCE_MS 20
#define BUTTON_DOUBLE_MS 300
#define BUTTON_LONG_MS 2000
#define BUTTON_VERY_LONG_MS 8000

typedef struct {
    bool raw, stable;      // last sample, debounced state (true = pressed)
    uint32_t raw_since;    // when raw last changed
    uint32_t pressed_at;   // when the debounced press began
    uint32_t released_at;  // when the last short press ended
    bool short_pending;    // a short press waiting to see if a second one follows
    bool very_long_fired;
} button_t;

void button_init(button_t *b);

// Call every few ms (10 ms is plenty) with the pin state; returns at most one event per call.
button_event_t button_update(button_t *b, bool pressed, uint32_t now_ms);

// How long the button has been held (0 if not pressed), so the LED can show a long press being armed.
uint32_t button_held_ms(const button_t *b, uint32_t now_ms);

// ---- LED -----------------------------------------------------------------------------------------------------

typedef enum {  // highest priority last: the firmware shows the most important state
    LED_OFF,
    LED_ADVERTISING,   // nothing connected: 100 ms flash every second
    LED_CONNECTED,     // on, with a short dark flicker on MIDI activity
    LED_WIFI_JOINING,  // 2 Hz blink
    LED_SETUP,         // double flash every 1.5 s
    LED_LOW_BATTERY,   // triple flash every 2 s
    LED_OTA,           // 10 Hz blink
} led_state_t;

#define LED_ACTIVITY_MS 40  // how long MIDI activity darkens a steady LED

// LED level for a state at a time. last_activity_ms: when the last MIDI message passed (0 = never).
bool led_level(led_state_t s, uint32_t now_ms, uint32_t last_activity_ms);

// ---- battery -------------------------------------------------------------------------------------------------

#define BATTERY_WARN_MV 3500   // LED warns
#define BATTERY_EMPTY_MV 3300  // all notes off, deep sleep (the cell's protection would cut at ~2.5-3.0 V)
#define BATTERY_HYSTERESIS_MV 50
#define BATTERY_EMPTY_SAMPLES 5  // consecutive samples below EMPTY: a radio burst's sag is not a flat cell

typedef struct {
    uint32_t mv;  // filtered cell voltage, 0 until the first sample
    bool low, empty;
    int below_empty;
} battery_t;

void battery_init(battery_t *b);

// Feed one cell voltage sample (mV, after the 1:2 divider is undone), about once a second.
void battery_update(battery_t *b, uint32_t cell_mv);

// State of charge from a resting Li-ion voltage, 0-100 (a rough curve; under load it reads a little low).
int battery_percent(uint32_t cell_mv);

#ifdef __cplusplus
}
#endif
