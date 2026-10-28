// MIDI-CI (M2-101) messages: building and parsing the ones airmidi uses, Discovery, Property Exchange (M2-103) and
// Process Inquiry's MIDI Message Report (M2-101 9: a device reports its current values as ordinary MIDI messages).
// Messages here are SysEx *contents*: the bytes between F0 and F7, starting with 7E. Multi-byte fields are 7-bit,
// LSB first; a MUID is 28 bits in four bytes.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MIDICI_VERSION 0x02           // message format version of MIDI-CI 1.2
#define MIDICI_BROADCAST 0x0FFFFFFFu  // destination of a Discovery
#define MIDICI_HEADER 13              // 7E, device id, 0D, sub-id 2, version, source MUID (4), destination MUID (4)
#define MIDICI_PE_OVERHEAD (MIDICI_HEADER + 2 + 1 + 2 + 6)  // F0/F7, request id, header length, chunk fields

enum {
    MIDICI_DISCOVERY = 0x70,
    MIDICI_DISCOVERY_REPLY = 0x71,
    MIDICI_ACK = 0x7D,
    MIDICI_INVALIDATE_MUID = 0x7E,
    MIDICI_NAK = 0x7F,
    MIDICI_PE_CAPS = 0x30,
    MIDICI_PE_CAPS_REPLY = 0x31,
    MIDICI_PE_GET = 0x34,
    MIDICI_PE_GET_REPLY = 0x35,
    MIDICI_PI_CAPS = 0x40,        // Inquiry: Process Inquiry Capabilities
    MIDICI_PI_CAPS_REPLY = 0x41,  // Reply to Process Inquiry Capabilities
    MIDICI_REPORT = 0x42,         // Inquiry: MIDI Message Report
    MIDICI_REPORT_REPLY = 0x43,   // Reply to MIDI Message Report (begin)
    MIDICI_REPORT_END = 0x44,     // End of MIDI Message Report
};

// Process Inquiry Supported Features (M2-101 table 42).
enum { MIDICI_PI_MESSAGE_REPORT = 1 << 0 };

// MIDI Message Report: channel controller message types (M2-101 table 43) and Message Data Control (table 44).
enum {
    MIDICI_REPORT_PITCHBEND = 1 << 0,
    MIDICI_REPORT_CC = 1 << 1,
    MIDICI_REPORT_RPN = 1 << 2,
    MIDICI_REPORT_NRPN = 1 << 3,
    MIDICI_REPORT_PROGRAM = 1 << 4,
    MIDICI_REPORT_CHPRESS = 1 << 5,
};
enum { MIDICI_REPORT_NONE = 0x00, MIDICI_REPORT_CHANGED = 0x01, MIDICI_REPORT_FULL = 0x7F };
#define MIDICI_DEVICE_FUNCTION_BLOCK 0x7F  // device id: the whole function block (all channels)

enum { MIDICI_CAT_PROFILES = 1 << 2, MIDICI_CAT_PE = 1 << 3, MIDICI_CAT_PROCESS = 1 << 4 };

// ACK / NAK status codes used here (M2-101 tables 14 and 16).
enum {
    MIDICI_ACK_TIMEOUT_WAIT = 0x10,   // status data = extra wait in 100 ms steps
    MIDICI_NAK_NOT_SUPPORTED = 0x01,  // MIDI-CI message not supported
    MIDICI_NAK_TERMINATE = 0x20,      // status data 0: the responder ends the transaction
};

typedef struct {
    uint8_t manufacturer[3];  // SysEx ID; 7D 00 00 = educational / development
    uint16_t family, model;   // 14 bits each
    uint8_t version[4];
} midici_ident_t;

typedef struct {
    uint8_t sub2, version, device_id;
    uint32_t src, dst;
    const uint8_t *data;  // what follows the header
    size_t len;
} midici_msg_t;

// A random-looking MUID is the caller's job (the spec wants a new one at every power-up); this keeps it in range,
// away from the reserved values and the broadcast address.
uint32_t midici_muid(uint32_t random);

// Parse a SysEx (contents, no F0/F7). False if it is not a MIDI-CI message.
bool midici_parse(const uint8_t *sx, size_t n, midici_msg_t *out);

// Builders write the SysEx contents to out and return the length. out must hold the result: the sizes are in the
// comments, a Get is MIDICI_HEADER + 9 + its JSON header, a Get reply the same plus its property data.
size_t midici_discovery(uint8_t *out, uint32_t src, const midici_ident_t *id, uint8_t categories, uint32_t max_sysex,
                        uint8_t output_path);  // 30 bytes
size_t midici_discovery_reply(uint8_t *out, uint32_t src, uint32_t dst, const midici_ident_t *id, uint8_t categories,
                              uint32_t max_sysex, uint8_t output_path);                               // 31 bytes
size_t midici_pe_caps(uint8_t *out, uint8_t sub2, uint32_t src, uint32_t dst, uint8_t simultaneous);  // 16 bytes
// ACK or NAK (sub2 MIDICI_ACK / MIDICI_NAK) about a Property Exchange transaction, no message text. 23 bytes.
size_t midici_ack_nak(uint8_t *out, uint8_t sub2, uint32_t src, uint32_t dst, uint8_t original_sub2, uint8_t status,
                      uint8_t status_data, uint8_t request_id);
size_t midici_invalidate(uint8_t *out, uint32_t src, uint32_t target);  // 17 bytes
// Inquiry: Get Property Data for one resource, header {"resource":"<name>"} or {"resource":"<name>","resId":"<id>"}
// (res_id may be NULL). Names longer than MIDICI_NAME_MAX - 1 are not sent: returns 0.
#define MIDICI_NAME_MAX 40
size_t midici_pe_get(uint8_t *out, uint32_t src, uint32_t dst, uint8_t request_id, const char *resource,
                     const char *res_id);
// One chunk of a Reply to Get Property Data. The header goes in chunk 1 only (M2-101 8.3.1).
size_t midici_pe_get_reply(uint8_t *out, uint32_t src, uint32_t dst, uint8_t request_id, const char *header,
                           uint16_t chunks, uint16_t this_chunk, const uint8_t *data, size_t len);

typedef struct {
    uint8_t request_id;
    const char *header;  // not terminated
    size_t header_len;
    uint16_t chunks, this_chunk;
    const uint8_t *data;
    size_t data_len;
} midici_pe_t;

// Process Inquiry. Device id: a channel 0x00-0x0F, or MIDICI_DEVICE_FUNCTION_BLOCK.
size_t midici_pi_caps(uint8_t *out, uint32_t src, uint32_t dst);                          // 13 bytes
size_t midici_pi_caps_reply(uint8_t *out, uint32_t src, uint32_t dst, uint8_t features);  // 14 bytes
size_t midici_report(uint8_t *out, uint8_t device_id, uint32_t src, uint32_t dst, uint8_t data_control, uint8_t system,
                     uint8_t channel, uint8_t note);  // 18 bytes
size_t midici_report_reply(uint8_t *out, uint8_t device_id, uint32_t src, uint32_t dst, uint8_t system, uint8_t channel,
                           uint8_t note);                                               // 17 bytes
size_t midici_report_end(uint8_t *out, uint8_t device_id, uint32_t src, uint32_t dst);  // 13 bytes

// An Inquiry: MIDI Message Report (data_control set) or its Reply (data_control 0xFF: the reply has none).
typedef struct {
    uint8_t data_control, system, channel, note;
} midici_report_t;

bool midici_parse_report(const midici_msg_t *m, midici_report_t *out);

// The Property Exchange fields of a Get inquiry or reply. False if malformed.
bool midici_parse_pe(const midici_msg_t *m, midici_pe_t *out);

// Discovery / Discovery Reply fields.
typedef struct {
    midici_ident_t ident;
    uint8_t categories;
    uint32_t max_sysex;
    uint8_t output_path;
} midici_discovery_t;

bool midici_parse_discovery(const midici_msg_t *m, midici_discovery_t *out);

// ACK / NAK fields (message version 2; a version 1 NAK has none and parses as status 0).
typedef struct {
    uint8_t original_sub2, status, status_data;
    uint8_t details[5];  // Property Exchange: [0] = request id, [1..2] = chunk number
} midici_ack_t;

void midici_parse_ack(const midici_msg_t *m, midici_ack_t *out);

// Invalidate MUID: the MUID to forget. False if malformed.
bool midici_parse_invalidate(const midici_msg_t *m, uint32_t *target);

#ifdef __cplusplus
}
#endif
