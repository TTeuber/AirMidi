// The RTP-MIDI recovery journal (RFC 6295 section 5 and appendix A): how RTP-MIDI survives lost packets without
// retransmission. Every packet carries a summary of the stream's recent past (since a "checkpoint" packet the
// receiver has confirmed), and a receiver that finds a gap in the sequence numbers compares that summary with what
// it has delivered and plays the difference: a lost note off is played late instead of hanging forever.
//
// What is covered, per channel: Program Change with bank (chapter P), Control Change (C), Pitch Bend (W), notes on
// and off (N) and Channel Pressure (T). Not covered: RPN/NRPN (chapter M; CC 6, 38 and 96-101 are left out of the
// journal altogether), Poly Pressure (A), note extras (E) and the system journal (SysEx, clock, song position).
// Those are lost with their packet. Received journals are parsed whole and their unsupported chapters skipped.
//
// The sender follows the closed-loop policy (RFC 6295 C.2.2.2): the receiver's feedback (AppleMIDI RS) moves the
// checkpoint forward, so the journal only describes what the receiver may not have seen.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RTPJ_TIMES 32     // send times of recent packets, for the Y ("play it late") bit of recovered notes
#define RTPJ_PLAY_MS 100  // a NoteOn older than this is not worth playing late (RFC 4696 4.2)
#define RTPJ_COUNTED 7    // controllers journalled with the count tool: 120, 121, 123-127
#define RTPJ_SLOTS 4      // channels with unconfirmed history at once

// ---- sending -------------------------------------------------------------------------------------------------

typedef struct {
    // The latest command of each kind on one channel, the packet it went out in, and whether it is still in the
    // checkpoint history (`in` bits): only those go into the journal.
    int8_t ch;          // the channel this slot holds, -1 = free
    uint16_t last_seq;  // newest packet recorded here (which slot to give up when all are taken)
    uint8_t program, bank_msb, bank_lsb;
    bool bank, prog_in;
    uint16_t prog_seq;
    uint8_t ctl_val[128];
    uint16_t ctl_seq[128];
    uint32_t ctl_in[4];
    uint8_t bend_lsb, bend_msb;
    bool bend_in;
    uint16_t bend_seq;
    uint8_t note_vel[128];  // 0 = the latest command for the note was a note off
    uint16_t note_seq[128];
    uint32_t note_in[4];
    uint16_t off_seq;  // the latest packet with a note off on this channel (chapter N's B bit)
    bool any_off;
    uint8_t pressure;
    bool press_in;
    uint16_t press_seq;
} rtpj_chan_t;

// What must outlast the checkpoint history, per channel: the bank for the next Program Change, and the counts.
typedef struct {
    uint8_t cur_msb, cur_lsb;
    bool have_msb, have_lsb;
    uint8_t counts[RTPJ_COUNTED];  // count tool: commands per controller since the start, mod 64
} rtpj_session_chan_t;

typedef struct {
    // History lives in a few slots, not 16 channels: ~800 bytes each, and a stream rarely has more than a few
    // channels busy between two receiver reports. A slot is free again once the receiver has confirmed its history.
    rtpj_chan_t slot[RTPJ_SLOTS];
    rtpj_session_chan_t chan[16];
    uint16_t checkpoint;            // the oldest packet the journal still has to cover
    uint16_t sent_seq[RTPJ_TIMES];  // ring of (seq, time) of packets sent, for the Y bit
    uint32_t sent_ms[RTPJ_TIMES];
    uint32_t overflows;  // history given up unconfirmed: a journal too big for a packet, or a fifth busy channel
} rtpj_tx_t;

// first_seq: the sequence number of the stream's first packet (the first checkpoint).
void rtpj_tx_init(rtpj_tx_t *t, uint16_t first_seq);

// Write the journal for packet `seq` (which covers packets checkpoint .. seq-1) into out. Returns its length, at
// least 3 (an empty journal is still a journal: every packet of a journalled stream carries one). If it does not
// fit in cap, the checkpoint moves up to seq, the journal goes out empty and `overflows` counts it.
size_t rtpj_tx_build(rtpj_tx_t *t, uint16_t seq, uint32_t now_ms, uint8_t *out, size_t cap);

// A MIDI 1.0 command sent in packet `seq` (call after rtpj_tx_build for that packet). Channel messages and System
// Reset (0xFF) matter; everything else is ignored.
void rtpj_tx_record(rtpj_tx_t *t, uint16_t seq, uint32_t now_ms, uint8_t status, uint8_t d1, uint8_t d2);

// Receiver feedback: the receiver has seen everything up to `seq`, so the journal can start after it.
void rtpj_tx_ack(rtpj_tx_t *t, uint16_t seq, uint16_t next_seq);

// True if the journal for the next packet would describe anything (worth sending a journal-only packet when idle).
bool rtpj_tx_pending(const rtpj_tx_t *t);

// ---- receiving -----------------------------------------------------------------------------------------------

#define RTPJ_UNKNOWN 0xFF

typedef struct {
    uint8_t ctl[128];  // latest value delivered, RTPJ_UNKNOWN if none yet
    uint8_t counts[RTPJ_COUNTED];
    uint32_t notes[4];  // notes on
    uint8_t program;    // RTPJ_UNKNOWN if none yet
    uint16_t bend;      // 0xFFFF if none yet
    uint8_t pressure;
} rtpj_rx_chan_t;

typedef struct {
    rtpj_rx_chan_t ch[16];
    uint32_t repairs;  // commands played from journals
} rtpj_rx_t;

typedef void (*rtpj_emit_fn)(void *ctx, uint8_t status, uint8_t d1, uint8_t d2);

void rtpj_rx_init(rtpj_rx_t *r);

// Every channel message delivered to the application (including repairs), and System Reset: keeps the receiver's
// view of the stream that a journal is compared against.
void rtpj_rx_track(rtpj_rx_t *r, uint8_t status, uint8_t d1, uint8_t d2);

// After a loss (and for the first packet of a stream): compare the journal j[0..len) with what was delivered and
// emit the commands that repair the difference (they are tracked too). Returns false if the journal is malformed;
// whatever was repaired before the error stands.
bool rtpj_rx_recover(rtpj_rx_t *r, const uint8_t *j, size_t len, rtpj_emit_fn emit, void *ctx);

#ifdef __cplusplus
}
#endif
