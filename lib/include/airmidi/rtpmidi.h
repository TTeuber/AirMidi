// RTP-MIDI (RFC 6295) with Apple's session protocol (AppleMIDI, the "Network" driver in macOS and iOS, rtpMIDI on
// Windows): the second network transport, for clients without Network MIDI 2.0. Pure logic like lib/nm2: the caller
// owns the two sockets (control port P and data port P + 1), passes in every datagram with its source and the time,
// and gets datagrams to send, UMPs to deliver and session changes through callbacks.
//
// Sessions: either side may invite. A client invites on our control port (IN), we accept (OK), it invites again on
// the data port, and the session is up. The inviter keeps the clocks in step (CK, three-way, every 10 s); BY ends a
// session from either side.
//
// Surviving a restart: macOS keeps sending clock syncs to a box that restarted, and ends the session (BY, and the
// box is gone from its list) after three go unanswered. So a clock sync or MIDI from a client we have no session
// with takes the session back: it is set up from that datagram (the client's SSRC, its data port, the control port
// one below) and answered. This only works if our SSRC is the same as before the restart, so the caller derives it
// from something stable (the MAC address), and the session clock must not have started again from 0
// (rtpmidi_set_clock). Our sequence numbers must also carry on from before, or macOS takes
// every packet "behind" its last one for a late one and drops it. So two journal-only packets go out, numbered half
// the range apart: exactly one of them is ahead of whatever the client saw last, and its receiver feedback (RS)
// names that one. MIDI waits for it (~1-2 s, at most 5). Not in the AppleMIDI protocol, which has no way back into
// a session.
//
// MIDI: one RTP packet per batch, MIDI 1.0 commands in it (MIDI 2.0 from the bus is translated down), SysEx split
// across packets when a batch ends inside one. Every packet carries a recovery journal (lib/rtp_journal), so a
// receiver repairs lost packets itself; nothing is retransmitted. The receiver's feedback (RS) keeps the journal
// short. When the data stops, journal-only packets follow at 20 ms, 100 ms and 500 ms, so a lost last packet (a
// NoteOff) is repaired without waiting for the next note.
//
// Received packets: anything older than the newest one is dropped (late is gone: the next packet's journal already
// covered it); after a gap, the packet's journal repairs what was lost, then its commands are delivered. Command
// timestamps are not used: commands are delivered when the packet arrives, as with BLE MIDI.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "airmidi/midi1.h"
#include "airmidi/rtp_journal.h"
#include "airmidi/ump.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RTPMIDI_SESSIONS 2
#define RTPMIDI_MAX_PACKET 1400  // stay under typical MTUs (RFC 6295 2.2)
#define RTPMIDI_LIST_MAX 900     // MIDI list bytes per packet; the rest is left for the journal
#define RTPMIDI_PT 0x61          // RTP payload type 97, as Apple uses it
#define RTPMIDI_INVITE_TRIES 12  // invitations, one a second, before giving up (as macOS does)

enum { RTPMIDI_CONTROL, RTPMIDI_DATA };  // which of the two sockets

typedef struct {
    uint32_t ip;  // as the caller likes it (network order is fine); only compared for equality
    uint16_t port;
} rtpmidi_addr_t;

typedef struct {
    void (*send)(void *ctx, int sock, const rtpmidi_addr_t *to, const uint8_t *data, size_t len);
    void (*deliver)(void *ctx, int session, const ump_t *u);  // every message the client sent, in order
    void (*session_changed)(void *ctx, int session, bool up, const char *peer_name);
} rtpmidi_callbacks_t;

typedef enum {
    RTPMIDI_IDLE,
    RTPMIDI_WAIT_DATA,    // we accepted the control invitation, the data one has not come yet
    RTPMIDI_INVITE_CTRL,  // we are inviting: control port
    RTPMIDI_INVITE_DATA,  // we are inviting: data port
    RTPMIDI_UP,
} rtpmidi_state_t;

typedef struct {
    uint32_t rx_packets, rx_late, rx_lost, rx_repairs, rx_bad;
    uint32_t tx_packets, journal_overflows;
    uint32_t tx_held;  // MIDI not sent while a session taken back waited for its sequence number
} rtpmidi_stats_t;

typedef struct {
    rtpmidi_state_t state;
    bool initiator;
    bool resumed;      // taken back after a restart (see the top of this file): the client's name is not known
    bool seq_unknown;  // taken back, and the client has not said yet which of our two probes it took
    uint16_t probe[2];
    rtpmidi_addr_t ctrl, data;  // the client's two ports
    uint32_t peer_ssrc, token;
    char peer_name[64];
    uint32_t state_ms, last_rx_ms;
    int tries;  // invitations sent (initiator) / clock syncs started
    // sending
    uint16_t tx_seq;  // the next packet's sequence number
    rtpj_tx_t jtx;
    midi1_out_t m1;
    bool sysex_cont;        // a SysEx was cut at the end of the last packet; the next one continues it
    uint32_t last_send_ms;  // last packet with MIDI in it
    int guard;              // journal-only packets sent since then
    uint32_t next_ck_ms;
    // receiving
    bool rx_started;
    uint16_t rx_seq;  // newest packet received
    bool feedback_due;
    uint32_t feedback_ms;
    midi1_in_t parser;
    rtpj_rx_t jrx;
    rtpmidi_stats_t stats;
} rtpmidi_session_t;

typedef struct {
    char name[64];
    uint32_t ssrc;
    uint32_t seed;  // for tokens and first sequence numbers
    rtpmidi_session_t s[RTPMIDI_SESSIONS];
    rtpmidi_callbacks_t cb;
    void *ctx;
    uint64_t clock_base;              // session clock = clock_base + now (100 us units)
    uint8_t pkt[RTPMIDI_MAX_PACKET];  // the packet being built (here, not on the caller's stack)
} rtpmidi_t;

// ssrc and seed: random numbers from the caller (the lib has no entropy source).
void rtpmidi_init(rtpmidi_t *h, const char *name, uint32_t ssrc, uint32_t seed, const rtpmidi_callbacks_t *cb,
                  void *ctx);

// The session clock (RTP timestamps and clock sync) is clock_base + now, in 100 us units; clock_base is 0 unless
// set. It must not run backwards across a restart: macOS maps a client's timestamps through the clock offset it
// measured before, and a box whose clock started again from 0 looks days ahead in 32-bit RTP time, so macOS holds
// its MIDI back (measured: timestamps 10 s ahead never arrived within the test, 10 s behind arrived at once). The
// firmware sets it from wall-clock time once SNTP has it, before handling any datagram.
void rtpmidi_set_clock(rtpmidi_t *h, uint64_t base_100us);

void rtpmidi_receive(rtpmidi_t *h, int sock, const rtpmidi_addr_t *from, const uint8_t *pkt, size_t len, uint32_t now);

// Send UMPs to a session's client (as few packets as they fit in). Returns false if the session is not up.
bool rtpmidi_send(rtpmidi_t *h, int session, const ump_t *u, int count, uint32_t now);

// Call every few ms (10 is right): invitations, clock sync, receiver feedback, journal-only packets.
void rtpmidi_tick(rtpmidi_t *h, uint32_t now);

// Invite a client (its control port) to a session; returns the session slot, or -1 if none is free.
int rtpmidi_invite(rtpmidi_t *h, const rtpmidi_addr_t *ctrl, uint32_t now);

// End a session: BY to the client.
void rtpmidi_end(rtpmidi_t *h, int session);

#ifdef __cplusplus
}
#endif
