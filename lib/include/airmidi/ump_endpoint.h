// The box as a UMP Endpoint (M2-104 7.1): answers the UMP Stream messages a Network MIDI 2.0 client sends after
// connecting (Endpoint Discovery, Stream Configuration Request, Function Block Discovery). One bidirectional
// function block spanning group 1 (all ports share it for now).
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "airmidi/ump.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *name;                 // UMP Endpoint Name, up to 98 bytes of UTF-8
    const char *product_instance_id;  // up to 42 ASCII characters
    uint8_t manufacturer[3];          // SysEx ID (0x7D 0 0: educational / development)
    uint16_t family, model;
    uint8_t version[4];
    const char *block_name;  // the function block's name, up to 91 bytes
    bool midi2_capable;      // may switch to MIDI 2.0 protocol on request
    uint8_t ci_version;      // MIDI-CI message version the block carries (2 = MIDI-CI 1.2), 0 = none
    uint8_t protocol;        // current: 1 = MIDI 1.0 (MT 0x2), 2 = MIDI 2.0 (MT 0x4)
} ump_endpoint_t;

#define UMP_ENDPOINT_MAX_REPLY 24

// Handle one UMP Stream message (MT 0xF). Writes the replies to out and returns how many (0 for anything that is
// not a request). A Stream Configuration Request may change ep->protocol.
int ump_endpoint_handle(ump_endpoint_t *ep, const ump_t *req, ump_t out[UMP_ENDPOINT_MAX_REPLY]);

// What the box sends unasked when a session starts: everything a full Endpoint Discovery would get (Endpoint Info,
// Device Identity, name, Product Instance Id, Stream Configuration). macOS never asks over Network MIDI 2.0; told,
// it asks for the function blocks and runs MIDI-CI Discovery on them (docs/design/network-midi.md).
int ump_endpoint_announce(ump_endpoint_t *ep, ump_t out[UMP_ENDPOINT_MAX_REPLY]);

#ifdef __cplusplus
}
#endif
