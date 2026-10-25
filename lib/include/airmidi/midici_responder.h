// MIDI-CI responder: the device side of what the box asks. Answers Discovery, Property Exchange Capabilities and
// Get Property Data from a table of resources (JSON text the caller owns), generates the ResourceList from that
// table, chunks replies to each initiator's Receivable Maximum SysEx Message Size, and NAKs the inquiries it does
// not support. Small enough for a synth's firmware; the Mac stand-in (devices/mac_standin) uses it first.
//
// MIDI Message Report (M2-101 9), when the device gives a report function: the initiator asks for current values,
// the responder sends the Reply (begin), the device sends its values as ordinary MIDI messages from the function,
// the responder sends the End. Everything goes through the device's own output, so the order holds.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "airmidi/midici.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MIDICI_RESPONDER_PEERS 4   // initiators remembered (their MUID and max SysEx size)
#define MIDICI_RESPONDER_OUT 512   // largest message sent; also what it tells initiators it can receive
#define MIDICI_RESPONDER_LIST 512  // room for the generated ResourceList

typedef struct {
    const char *resource;  // e.g. "DeviceInfo", "AllCtrlList", "CtrlMapList"
    const char *res_id;    // NULL, or the "resId" it answers to (CtrlMapList entries)
    const char *json;      // the property data, ASCII
} midici_resource_t;

// Send the current values of the given channel controller types (MIDICI_REPORT_*) as MIDI messages, through the
// same output as the responder's SysEx. channel: 0-15, or -1 for every channel the device uses. changed_only:
// only values that differ from their defaults (Message Data Control 0x01).
typedef void (*midici_report_fn)(void *ctx, int channel, uint8_t types, bool changed_only);

typedef struct {
    uint32_t muid;
    midici_ident_t ident;
    midici_report_fn report;  // NULL: Process Inquiry is NAKed
    uint8_t report_types;     // what report can send
    const midici_resource_t *res;
    int nres;
    void (*send)(void *ctx, const uint8_t *sx, size_t n);  // SysEx contents, no F0/F7
    void *ctx;
    struct {
        uint32_t muid, max_sysex;
    } peers[MIDICI_RESPONDER_PEERS];
    unsigned next_peer;
    char list[MIDICI_RESPONDER_LIST];
    size_t list_len;
    uint8_t out[MIDICI_RESPONDER_OUT];
} midici_responder_t;

// False if the ResourceList for this table does not fit MIDICI_RESPONDER_LIST.
bool midici_responder_init(midici_responder_t *r, uint32_t muid, const midici_ident_t *ident,
                           const midici_resource_t *res, int nres, void (*send)(void *ctx, const uint8_t *sx, size_t n),
                           void *ctx);

// Offer MIDI Message Report: types = the MIDICI_REPORT_* the device can report, fn sends them.
void midici_responder_set_report(midici_responder_t *r, uint8_t types, midici_report_fn fn);

// A SysEx arrived (contents). Anything that is not MIDI-CI for this device is ignored.
void midici_responder_receive(midici_responder_t *r, const uint8_t *sx, size_t n);

#ifdef __cplusplus
}
#endif
