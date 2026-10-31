// The buttons and LEDs, sampled and driven every 10 ms from an esp_timer callback; the gestures and patterns
// themselves are lib/ui (host-tested). Rev A: one button (key 0) and one LED. Rev B: the panel, a button per key
// (panel_key_t) with an LED over each, dimmed by PWM.
#pragma once

#include <stdbool.h>

#include "airmidi/ui.h"

// on_button runs in the esp_timer task: keep it short (set a flag, queue a message). A button already held when
// this starts (the hold that woke the box) reports nothing until it has been released.
void ui_start(void (*on_button)(int key, button_event_t ev));

// Rev A: what the LED should show; MIDI activity (from the bus) flickers it in LED_CONNECTED.
void ui_set_led(led_state_t s);

// Rev B: what the panel's LEDs should show. ui_io fills in the hold times, the boot sweep, the battery bar and MIDI
// activity itself.
void ui_set_panel(const panel_state_t *s);

// Rev B: the four LEDs become a battery bar for PANEL_BAR_MS.
void ui_show_battery(void);

// All LEDs off for good (powering off).
void ui_dark(void);

// The button is down right now (raw pin).
bool ui_button_down(int key);
