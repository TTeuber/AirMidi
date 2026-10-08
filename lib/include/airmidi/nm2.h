// Network MIDI 2.0 (UDP), M2-124: the host side of the session protocol. Pure logic: the caller owns the socket,
// passes in every received datagram with the sender's address and the time, and gets datagrams to send and UMPs
// to deliver through callbacks. Time is milliseconds from any monotonic clock (wraps after 49 days, handled).
//
// Liveness: a client we have not heard from for 10 s is pinged every 10 s, but never dropped for not answering
// (macOS 26.5 does not answer pings); a client silent for 60 s gives its slot to a new one when all are taken.
//
// Data integrity:
//   - sending: every UMP Data command goes out with the two previous ones in front (FEC, M2-124 7.2.2); when data
//     stops, zero-length commands follow at 20 and 40 ms (so the last real command is repeated twice more), then
//     at growing intervals; sent commands are kept for Retransmit Requests;
//   - receiving: duplicates (FEC repeats) are skipped; a gap holds the commands after it (up to NM2_HOLD of them)
//     instead of delivering them out of order, asks for a retransmit after 10 ms (then every 20), and gives up after
//     150 ms (Wi-Fi links stall for ~100 ms, measured) or when NM2_HOLD commands are waiting,
//     delivering what it holds and counting the loss. Out-of-order MIDI is worse than late MIDI: a note off ahead
//     of its note on leaves a note hanging.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "airmidi/ump.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NM2_SESSIONS 2
#define NM2_HISTORY 32    // sent UMP Data commands kept for FEC and retransmit
#define NM2_CMD_WORDS 16  // UMP words per data command we build (a 128-bit UMP is 4)
#define NM2_RX_WORDS 32   // a command held behind a gap; longer ones are only taken in order (clients may send 64)
#define NM2_HOLD 48       // commands held behind a gap: 150 ms at 320 commands/s (6.3 KB per session)
#define NM2_FEC 2
#define NM2_MAX_PACKET 1400  // M2-124 5.1.1: stay under typical MTUs

typedef struct {
    uint32_t ip;  // as the caller likes it (network order is fine); only compared for equality
    uint16_t port;
} nm2_addr_t;

typedef struct {
    void (*send)(void *ctx, const nm2_addr_t *to, const uint8_t *data, size_t len);
    void (*deliver)(void *ctx, int session, const ump_t *u);  // every UMP the client sent, in order
    void (*session_changed)(void *ctx, int session, bool up, const char *peer_name);
} nm2_callbacks_t;

typedef struct {  // a command we sent, kept for FEC and retransmit
    uint16_t seq;
    uint8_t n;  // words
    uint32_t w[NM2_CMD_WORDS];
} nm2_cmd_t;

typedef struct {  // a command received ahead of a gap
    uint16_t seq;
    uint8_t n;
    uint32_t w[NM2_RX_WORDS];
} nm2_rxcmd_t;

typedef struct {
    uint32_t rx_cmds, rx_dups, rx_lost, rx_recovered;  // recovered: gaps filled by a retransmit
    uint32_t tx_cmds, tx_retransmitted, retransmit_requests;
} nm2_stats_t;

typedef struct {
    bool active;
    nm2_addr_t peer;
    char peer_name[64];
    // sending
    uint16_t tx_seq;
    nm2_cmd_t hist[NM2_HISTORY];  // ring, oldest first from hist_start
    int hist_start, hist_count;
    uint32_t last_data_ms;  // when the last non-empty command went out
    int idle_step;          // which zero-length command comes next (-1: none due)
    // receiving
    uint16_t rx_next;
    nm2_rxcmd_t hold[NM2_HOLD];  // received after a gap, in sequence order
    int n_hold;
    uint32_t gap_since_ms, next_request_ms;
    // liveness
    uint32_t last_rx_ms, ping_sent_ms;
    uint32_t ping_id;
    int missed_pings;
    nm2_stats_t stats;
} nm2_session_t;

typedef struct {
    char name[99];  // UMP Endpoint Name
    char piid[43];  // Product Instance Id
    nm2_session_t s[NM2_SESSIONS];
    nm2_callbacks_t cb;
    void *ctx;
} nm2_host_t;

void nm2_host_init(nm2_host_t *h, const char *name, const char *piid, const nm2_callbacks_t *cb, void *ctx);

// One received datagram.
void nm2_host_receive(nm2_host_t *h, const nm2_addr_t *from, const uint8_t *pkt, size_t len, uint32_t now);

// Send UMPs to a session's client (one datagram, FEC included). Returns false if the session is not active.
bool nm2_host_send(nm2_host_t *h, int session, const ump_t *u, int count, uint32_t now);

// Call every few ms (10 is right): retransmit requests and give-ups, idle zero-length commands, pings, timeouts.
void nm2_host_tick(nm2_host_t *h, uint32_t now);

// End a session (Bye, reason "power down" / user), e.g. when Wi-Fi goes away.
void nm2_host_end(nm2_host_t *h, int session, uint8_t reason);

#ifdef __cplusplus
}
#endif
