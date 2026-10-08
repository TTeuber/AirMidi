// MIDI 1.0 <-> MIDI 2.0 Protocol translation, the Default Translation of M2-104 Appendix D. Both sides are UMP:
// MIDI 1.0 Protocol is MT 0x2 (channel voice), MIDI 2.0 Protocol is MT 0x4. System messages (MT 0x1) and SysEx7
// (MT 0x3) are the same in both protocols and are not touched here.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "airmidi/ump.h"

#ifdef __cplusplus
extern "C" {
#endif

// Min-Center-Max upscaling (M2-104 D.1.3): minimum, center and maximum map to minimum, center and maximum, and
// values above the center are filled with a repeating bit pattern so the range ends exactly at full scale.
// Requires 1 < src_bits < dst_bits <= 32.
uint32_t scale_up(uint32_t value, int src_bits, int dst_bits);
// Downscaling (D.1.4): drop the low bits.
static inline uint32_t scale_down(uint32_t value, int src_bits, int dst_bits) { return value >> (src_bits - dst_bits); }

// MIDI 2.0 -> MIDI 1.0 (D.2). Stateless. Writes up to 4 MT 0x2 UMPs (an RPN/NRPN becomes 4 Control Changes, a
// Program Change with a valid bank 3 messages) and returns how many; 0 for messages with no MIDI 1.0 equivalent
// (per-note controllers and pitch bend, per-note management, relative RPN/NRPN). Anything that is not MT 0x4 is
// copied through unchanged (returns 1).
int m2_to_m1(const ump_t *in, ump_t out[4]);

// MIDI 1.0 -> MIDI 2.0 (D.3). Stateful per group and channel: Bank Select (CC 0/32) is held and attached to the
// next Program Change, and the CC 6/38/98-101 sequences become single RPN/NRPN messages.
typedef struct {
    uint8_t bank_msb, bank_lsb;
    bool bank_valid;          // a CC 0 was seen
    uint8_t rpn[2], nrpn[2];  // latest CC 101/100 and CC 99/98 (MSB, LSB); 0x7F 0x7F = null function
    uint8_t kind;             // which pair was written last: 0 none, 2 RPN, 3 NRPN (the MT 0x4 opcodes)
    uint8_t entry_msb;        // latest CC 6
    bool entry_valid;         // a CC 6 was seen for the current parameter
    bool entry_pending;       // that CC 6 has not gone out yet (waiting for CC 38 or the next trigger)
} m1_to_m2_channel_t;

typedef struct {
    m1_to_m2_channel_t ch[16][16];  // [group][channel]
} m1_to_m2_t;

void m1_to_m2_init(m1_to_m2_t *t);

// Translate one UMP. Writes 0, 1 or 2 UMPs to out and returns how many: 0 while CCs that belong to an RPN/NRPN or
// bank select are being collected, 2 when a held RPN/NRPN has to go out before the message that ended it.
// Anything that is not MT 0x2 is copied through unchanged.
int m1_to_m2(m1_to_m2_t *t, const ump_t *in, ump_t out[2]);

// Emit a held RPN/NRPN Data Entry MSB (CC 6 without its CC 38) for one channel. Devices that never send CC 38
// would otherwise wait for the next trigger; the caller decides when to give up (D.3.3 suggests a short timeout).
int m1_to_m2_flush(m1_to_m2_t *t, uint8_t group, uint8_t channel, ump_t out[1]);

#ifdef __cplusplus
}
#endif
