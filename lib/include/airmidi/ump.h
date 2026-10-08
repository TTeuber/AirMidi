// Universal MIDI Packet (UMP, M2-104): the one format every message has inside airmidi. Transports convert to UMP
// at the edge, the router only sees UMP, and each output converts back. A UMP is 1, 2, 3 or 4 32-bit words; the
// Message Type (MT) in the top 4 bits of word 0 says how many.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t w[4];  // unused words are zero
} ump_t;

// Message Types used here (M2-104 section 2.1.4)
enum {
    UMP_MT_UTILITY = 0x0,  // 32 bit: NOOP, JR clock/timestamp, delta clockstamps
    UMP_MT_SYSTEM = 0x1,   // 32 bit: MIDI 1.0 system real-time and system common
    UMP_MT_M1_CV = 0x2,    // 32 bit: MIDI 1.0 channel voice
    UMP_MT_SYSEX7 = 0x3,   // 64 bit: 7-bit System Exclusive, up to 6 bytes per packet
    UMP_MT_M2_CV = 0x4,    // 64 bit: MIDI 2.0 channel voice
    UMP_MT_DATA128 = 0x5,  // 128 bit: SysEx8, Mixed Data Set
    UMP_MT_FLEX = 0xD,     // 128 bit: Flex Data
    UMP_MT_STREAM = 0xF,   // 128 bit: UMP Stream (endpoint / function block discovery)
};

// SysEx7 packet status (bits 23-20 of word 0): where this packet sits in the whole message.
enum {
    UMP_SYSEX_COMPLETE = 0x0,
    UMP_SYSEX_START = 0x1,
    UMP_SYSEX_CONTINUE = 0x2,
    UMP_SYSEX_END = 0x3,
};

static inline uint8_t ump_mt(const ump_t *u) { return u->w[0] >> 28; }
static inline uint8_t ump_group(const ump_t *u) { return (u->w[0] >> 24) & 0xF; }
// The status byte of MT 0x1/0x2/0x4 messages (for MT 0x4: opcode << 4 | channel).
static inline uint8_t ump_status(const ump_t *u) { return (u->w[0] >> 16) & 0xFF; }
static inline uint8_t ump_byte3(const ump_t *u) { return (u->w[0] >> 8) & 0xFF; }
static inline uint8_t ump_byte4(const ump_t *u) { return u->w[0] & 0xFF; }

// Number of 32-bit words in a UMP with this Message Type (reserved types included, so a receiver can skip them).
int ump_words_for_mt(uint8_t mt);
static inline int ump_words(const ump_t *u) { return ump_words_for_mt(ump_mt(u)); }

bool ump_equal(const ump_t *a, const ump_t *b);

// Builders. Group is 0-15; data bytes are masked to 7 bits.
ump_t ump_system(uint8_t group, uint8_t status, uint8_t d1, uint8_t d2);
ump_t ump_m1_cv(uint8_t group, uint8_t status, uint8_t d1, uint8_t d2);
// One SysEx7 packet: status is UMP_SYSEX_*, n (0-6) data bytes without F0/F7.
ump_t ump_sysex7(uint8_t group, uint8_t status, const uint8_t *data, int n);
// Copies the data bytes of a SysEx7 packet into out[6]; returns how many there are.
int ump_sysex7_data(const ump_t *u, uint8_t out[6]);
static inline uint8_t ump_sysex7_status(const ump_t *u) { return (u->w[0] >> 20) & 0xF; }

// Reassembles a SysEx7 message from its packets into the caller's buffer: the bytes between F0 and F7. A message
// that does not fit is dropped whole (counted in `overflows`); a Start in the middle of a message abandons it.
typedef struct {
    uint8_t *buf;
    size_t cap, len;
    bool active, overflow;
    uint32_t overflows;
} sysex7_rx_t;

void sysex7_rx_init(sysex7_rx_t *r, uint8_t *buf, size_t cap);
// Feed one packet (anything that is not MT 0x3 is ignored). True when a message has just been completed in
// buf[0..len).
bool sysex7_rx_push(sysex7_rx_t *r, const ump_t *u);
// Split a SysEx (contents, no F0/F7) into packets of up to 6 bytes and pass each to emit. Returns how many.
int sysex7_split(uint8_t group, const uint8_t *data, size_t n, void (*emit)(void *ctx, const ump_t *u), void *ctx);
// MIDI 2.0 channel voice: opcode (0x0-0xF), channel, the two index bytes of word 0, and the 32-bit data word.
ump_t ump_m2_cv(uint8_t group, uint8_t opcode, uint8_t channel, uint8_t index1, uint8_t index2, uint32_t data);

// MIDI 1.0 data bytes that follow a status byte (0, 1 or 2); -1 for SysEx (F0) and undefined statuses
// (F4, F5, F9, FD) which carry no defined length.
int midi1_data_len(uint8_t status);

#ifdef __cplusplus
}
#endif
