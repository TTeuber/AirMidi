// The box's user interface: one button and one LED (rev A), or a panel of four buttons with an LED over each (rev B).
// Pure logic on (input, time), so the gestures and the blink patterns are host-tested; the firmware samples the pins
// and drives the LEDs.
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
    bool fast_double;  // DOUBLE fires at the second press instead of its release (the panel's panic)
    bool was_double;   // the press under way was the second of a double: its release reports nothing
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

// ---- panel (rev B) -------------------------------------------------------------------------------------------
// A button per mode with its LED over it (proto/airmidi_panel, "four leds, four buttons"): each LED shows its mode
// and how it is doing; a tap on PWR turns the row into a battery bar for 3 s.

typedef enum { PANEL_BLE, PANEL_NET, PANEL_STP, PANEL_PWR, PANEL_KEYS } panel_key_t;

typedef enum { PANEL_RESULT_NONE, PANEL_RESULT_OK, PANEL_RESULT_FAILED } panel_result_t;

typedef enum { PANEL_NET_OFF, PANEL_NET_JOINING, PANEL_NET_JOINED, PANEL_NET_AP } panel_net_t;

#define PANEL_SWEEP_MS 120   // boot: each LED in turn this long, two passes
#define PANEL_RESULT_MS 800  // a result (scan reply, update check) stays on its LED this long
#define PANEL_BAR_MS 3000    // the battery bar after a tap on PWR
#define PANEL_DIM 70         // "up but idle" (joined, no session; MIDI-CI scan blink), of 255

typedef struct {
    // BLE: stack running, a connection up, the pairing window for a new phone open (Setup's own window not counted)
    bool ble_on, ble_connected, ble_pairing;
    panel_net_t net;
    bool net_session;      // a Network MIDI 2.0 or RTP-MIDI session is up
    bool update_checking;  // asking GitHub for the latest release
    bool update_installing;
    panel_result_t update_result;  // of the last check, shown from update_at
    uint32_t update_at;
    bool scanning;               // MIDI-CI over TRS
    panel_result_t scan_result;  // OK = a device answered, shown from scan_at
    uint32_t scan_at;
    bool battery_absent;  // no cell reading (switch off, on USB)
    bool battery_low;
    int battery_percent;
    uint32_t bar_until;            // battery bar showing while now < bar_until (0: not showing)
    uint32_t sweep_from;           // boot sweep start
    uint32_t held_ms[PANEL_KEYS];  // how long each button has been held: past BUTTON_LONG_MS its LED says "release now"
    uint32_t last_activity_ms;     // last MIDI message routed (0 = never)
} panel_state_t;

// Brightness 0-255 of each LED.
void panel_leds(const panel_state_t *s, uint32_t now_ms, uint8_t level[PANEL_KEYS]);

// ---- battery -------------------------------------------------------------------------------------------------

#define BATTERY_ABSENT_MV 2500  // below this there is no cell on A0: power switch off, running from USB
#define BATTERY_WARN_MV 3500    // LED warns
#define BATTERY_EMPTY_MV 3300   // all notes off, deep sleep (the cell's protection would cut at ~2.5-3.0 V)
#define BATTERY_HYSTERESIS_MV 50
#define BATTERY_EMPTY_SAMPLES 5  // consecutive samples below EMPTY: a radio burst's sag is not a flat cell

typedef struct {
    uint32_t mv;  // filtered cell voltage, 0 until the first sample
    bool absent;  // no cell (switch off, on USB power): neither low nor empty
    bool low, empty;
    int below_empty;
} battery_t;

void battery_init(battery_t *b);

// Feed one cell voltage sample (mV, after the 1:2 divider is undone), about once a second. A0 sits after the
// power switch, so with the switch off and USB plugged in it reads ~0 V: that is "absent", never "empty".
void battery_update(battery_t *b, uint32_t cell_mv);

// State of charge from a resting Li-ion voltage, 0-100 (a rough curve; under load it reads a little low).
int battery_percent(uint32_t cell_mv);

#ifdef __cplusplus
}
#endif
