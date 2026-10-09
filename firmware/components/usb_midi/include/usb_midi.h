// USB MIDI on the S3's USB port (TinyUSB): a class-compliant USB MIDI 1.0 device (PORT_USB), and the console on a
// USB serial port beside it. TinyUSB takes the S3's only USB PHY from the built-in USB-Serial-JTAG, so the console
// and flashing move to the composite device: esptool's reset pattern on the serial port (RTS, then DTR) restarts the
// chip into its ROM bootloader, which comes up on USB-Serial-JTAG again.
#pragma once

#include <stdbool.h>
#include <stdint.h>

// Start USB (the S3 only; does nothing elsewhere). Call early: console output moves to USB from here on.
void usb_midi_start(const char *name);

typedef struct {
    bool mounted;
    uint32_t rx_packets, tx_packets, tx_dropped;
} usb_midi_stats_t;

void usb_midi_get_stats(usb_midi_stats_t *out);

// Restart into the ROM bootloader, which comes up on USB-Serial-JTAG for esptool (the S3; nothing elsewhere).
void usb_midi_bootloader(void);
