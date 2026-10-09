// The ESP-NOW cable: boxes in the same group find each other and pass MIDI over ESP-NOW (Wi-Fi frames straight from
// radio to radio, no access point), as UMP. Pure logic like lib/nm2: the caller owns the radio, passes in every
// frame received with its sender and the time, and gets frames to send, UMPs to deliver and peer changes through
// callbacks.
//
// Finding each other: every box broadcasts a hello (its group and name) once a second; a hello from the same group
// makes the sender a peer, and a peer not heard from for 5 s is gone. MIDI goes to each peer by unicast, which
// ESP-NOW acknowledges and retries at the radio level; the receiver drops the duplicates those retries make (a lost
// acknowledgement) by sequence number, and counts what never came.
//
// Channels: ESP-NOW needs both boxes on one Wi-Fi channel. A box joined to an access point is on the AP's channel
// and stays there; one that is not looks for its peers: with none, it moves every ESPNOW_HOP_MS or a little more,
// back to channel 1 every other time and in between to the next of the others (6 and 11 first, where most access
// points are), until it hears a hello, and stays on that channel while it has a peer. So a box in Play mode finds one
// in Network mode by itself (within ~25 s), and two that are both looking meet on channel 1; each waits a little
// longer per channel than the other (by its name), so they cannot keep missing each other in step.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "airmidi/ump.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ESPNOW_PEERS 4
#define ESPNOW_FRAME_MAX 250  // ESP-NOW v1 payload
#define ESPNOW_GROUP_MAX 15   // group name: boxes with the same one connect
#define ESPNOW_HELLO_MS 1000
#define ESPNOW_PEER_TIMEOUT_MS 5000
#define ESPNOW_HOP_MS 1200  // a little over one hello period per channel

typedef struct {
    void (*send)(void *ctx, const uint8_t mac[6], const uint8_t *data, size_t len);  // mac NULL: broadcast
    void (*deliver)(void *ctx, const ump_t *u);
    void (*peer_changed)(void *ctx, const uint8_t mac[6], bool up, const char *name);
    void (*set_channel)(void *ctx, uint8_t channel);  // only when not on an access point's channel
} espnow_callbacks_t;

typedef struct {
    uint32_t rx_frames, rx_dups, rx_lost, rx_bad, tx_frames;
    uint32_t rx_hellos, rx_other_group;  // hellos heard (from anyone), and of those from another group
} espnow_stats_t;

typedef struct {
    bool used;
    uint8_t mac[6];
    char name[32];
    uint32_t last_heard_ms;
    bool rx_started;
    uint16_t rx_seq;  // the newest data frame from this peer
} espnow_peer_t;

typedef struct {
    char group[ESPNOW_GROUP_MAX + 1];
    char name[32];
    espnow_peer_t peers[ESPNOW_PEERS];
    uint16_t tx_seq;
    uint32_t last_hello_ms;
    bool fixed_channel;  // on an access point's channel: never hop
    uint8_t channel;
    int hop_index;
    uint32_t channel_since_ms, dwell_ms;
    espnow_stats_t stats;
    espnow_callbacks_t cb;
    void *ctx;
} espnow_link_t;

void espnow_link_init(espnow_link_t *l, const char *group, const char *name, const espnow_callbacks_t *cb, void *ctx,
                      uint32_t now);

// One frame received (from a peer or anyone else).
void espnow_link_receive(espnow_link_t *l, const uint8_t mac[6], const uint8_t *data, size_t len, uint32_t now);

// Send UMPs to every peer (as many frames as they need). Returns the number of peers they went to.
int espnow_link_send(espnow_link_t *l, const ump_t *u, int count, uint32_t now);

// Call every few ms: hellos, peer timeouts, channel hopping.
void espnow_link_tick(espnow_link_t *l, uint32_t now);

// The radio is on an access point's channel (Network mode), or free again (0).
void espnow_link_set_ap_channel(espnow_link_t *l, uint8_t channel, uint32_t now);

int espnow_link_peer_count(const espnow_link_t *l);

#ifdef __cplusplus
}
#endif
