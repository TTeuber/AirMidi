// MIDI-CI initiator: finds the device on a MIDI 1.0 link and reads its control list. One scan is Discovery, then
// Property Exchange Capabilities, then Get Property Data for ResourceList, and for DeviceInfo, AllCtrlList and
// each CtrlMapList the AllCtrlList refers to (by "ctrlMapId"), whichever of them the ResourceList names. One
// request at a time, the simplest thing every responder supports.
//
// Pure logic: SysEx contents and the time go in, SysEx contents to send and the results come out through
// callbacks. It never allocates; property data is collected in the caller's buffer.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "airmidi/midici.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MIDICI_DISCOVERY_WAIT_MS 3000  // M2-101 5.5.5: wait at least 3 s for replies to Discovery
#define MIDICI_REPLY_WAIT_MS 3000      // M2-103 12.2: a reply, and each next chunk, within 3 s
#define MIDICI_MAX_GETS 12             // ResourceList, DeviceInfo, AllCtrlList and up to 9 CtrlMapLists
#define MIDICI_MAX_SYSEX 512           // what the initiator can receive (the minimum for a PE initiator)

typedef enum {
    MIDICI_DONE_OK,           // every resource was asked for; each one's own status went to on_resource
    MIDICI_DONE_CACHED,       // on_device said it already has this device's data
    MIDICI_DONE_NO_DEVICE,    // nobody answered Discovery
    MIDICI_DONE_NO_PE,        // a device answered but does not do Property Exchange (or refused it)
    MIDICI_DONE_TIMEOUT,      // the device stopped answering in the middle
    MIDICI_DONE_INVALIDATED,  // an Invalidate MUID for the device or for us; scan again with a new MUID
} midici_done_t;

// Status reported for a resource that did not arrive: the PE status codes (M2-103 table 15) where one fits.
enum {
    MIDICI_STATUS_TIMEOUT = 0,
    MIDICI_STATUS_REFUSED = 1,      // the device answered the request with a NAK
    MIDICI_STATUS_BAD_DATA = 342,   // chunks out of order, or the data set ended early
    MIDICI_STATUS_TOO_LARGE = 413,  // more data than the caller's buffer holds
};

typedef struct {
    void *ctx;
    // Send one MIDI-CI message (SysEx contents, no F0/F7).
    void (*send)(void *ctx, const uint8_t *sx, size_t n);
    // A device answered Discovery. Return true to read its resources, false to stop (the caller has them cached).
    bool (*on_device)(void *ctx, uint32_t muid, const midici_discovery_t *d);
    // One resource finished: status 200 with its property data, or the reason it failed. data is not terminated
    // and only valid during the call. res_id is "" when the request had none.
    void (*on_resource)(void *ctx, const char *resource, const char *res_id, int status, const uint8_t *data,
                        size_t len);
    void (*on_done)(void *ctx, midici_done_t result);
} midici_initiator_cb_t;

typedef struct {
    char resource[MIDICI_NAME_MAX], res_id[MIDICI_NAME_MAX];
} midici_get_t;

typedef struct {
    midici_initiator_cb_t cb;
    midici_ident_t ident;
    uint32_t muid;
    int state;
    uint32_t deadline;
    // the device
    uint32_t dev;
    bool saw_device_without_pe;
    // the gets still to do; gets[next] is in flight while state is GET
    midici_get_t gets[MIDICI_MAX_GETS];
    int ngets, next;
    bool listed_ctrl_map;  // the ResourceList names CtrlMapList
    uint8_t request_id;
    // the reply being collected
    uint8_t *buf;
    size_t cap, len;
    int status;
    uint16_t next_chunk;
    bool overflow;
    uint8_t out[2 * MIDICI_NAME_MAX + 64];
} midici_initiator_t;

void midici_initiator_init(midici_initiator_t *in, uint32_t muid, const midici_ident_t *ident,
                           const midici_initiator_cb_t *cb, uint8_t *buf, size_t cap);
// Start a scan (abandons one in progress). Sends Discovery.
void midici_initiator_start(midici_initiator_t *in, uint32_t now_ms);
// A MIDI-CI message arrived (SysEx contents). Anything not for this scan is ignored.
void midici_initiator_receive(midici_initiator_t *in, const uint8_t *sx, size_t n, uint32_t now_ms);
// Call at least every 100 ms: timeouts.
void midici_initiator_tick(midici_initiator_t *in, uint32_t now_ms);
bool midici_initiator_busy(const midici_initiator_t *in);
// For a new MUID after MIDICI_DONE_INVALIDATED.
void midici_initiator_set_muid(midici_initiator_t *in, uint32_t muid);

#ifdef __cplusplus
}
#endif
