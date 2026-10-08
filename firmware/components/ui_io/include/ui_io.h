// The button and the LED, sampled and driven every 10 ms from an esp_timer callback; the gestures and patterns
// themselves are lib/ui (host-tested).
#pragma once

#include "airmidi/ui.h"

// on_button runs in the esp_timer task: keep it short (set a flag, queue a message).
void ui_start(void (*on_button)(button_event_t ev));

// What the LED should show; MIDI activity (from the bus) flickers it in LED_CONNECTED.
void ui_set_led(led_state_t s);
