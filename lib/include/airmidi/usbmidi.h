// USB MIDI 1.0 event packets (USB Device Class Definition for MIDI Devices 1.0, section 4): what goes over the S3's
// USB port. Each packet is 4 bytes: cable number and Code Index Number (CIN, the kind of message), then up to 3
// MIDI bytes. A SysEx travels 3 bytes per packet, the last packet saying how many bytes end it.
//
// Receiving: usbmidi_packet_bytes() gives a packet's MIDI bytes for a midi1_in parser. Sending: usbmidi_tx turns
// UMP into packets (MIDI 2.0 translated down by midi1_out, as for TRS).
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "airmidi/midi1.h"
#include "airmidi/ump.h"

#ifdef __cplusplus
extern "C" {
#endif

#define USBMIDI_MAX_PACKETS 4  // packets one UMP can become (a SysEx7 packet plus carried bytes, or an RPN as 4 CCs)

// The MIDI bytes in one packet (0-3); a packet with an unknown or reserved CIN has none.
int usbmidi_packet_bytes(const uint8_t pkt[4], uint8_t out[3]);

typedef struct {
    uint8_t cable;   // 0-15
    midi1_out_t m1;  // UMP -> MIDI 1.0 bytes, no running status (USB MIDI has none)
    uint8_t sx[3];   // SysEx bytes waiting for a full packet
    uint8_t n_sx;
    bool in_sysex;
} usbmidi_tx_t;

void usbmidi_tx_init(usbmidi_tx_t *t, uint8_t cable);

// Encode one UMP into out[] and return how many packets (0 for UMPs with no MIDI 1.0 meaning).
int usbmidi_tx_encode(usbmidi_tx_t *t, const ump_t *u, uint8_t out[USBMIDI_MAX_PACKETS][4]);

#ifdef __cplusplus
}
#endif
