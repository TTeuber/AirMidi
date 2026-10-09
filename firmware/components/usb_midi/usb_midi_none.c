// Boards without USB OTG (the C6): no USB MIDI.
#include <string.h>

#include "usb_midi.h"

void usb_midi_start(const char *name) { (void)name; }
void usb_midi_get_stats(usb_midi_stats_t *out) { memset(out, 0, sizeof *out); }
void usb_midi_bootloader(void) {}
