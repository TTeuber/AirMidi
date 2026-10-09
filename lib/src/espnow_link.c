#include "airmidi/espnow_link.h"

#include <stdio.h>
#include <string.h>

// Frame: 'a' 'M', version, type, then the type's body.
//   hello: group (16 bytes, NUL padded), the sender's next data sequence number (u16 BE), name (NUL terminated)
//   data:  sequence number (u16 BE), whole UMPs as big-endian words
enum { TYPE_HELLO = 1, TYPE_DATA = 2 };
#define VERSION 1
#define HEADER 4
#define GROUP_FIELD 16

// Looking for peers, a box alternates between channel 1 (where two looking boxes meet) and the next of the others,
// those most access points use first.
#define HOME 1
static const uint8_t sweep[] = {6, 11, 2, 3, 4, 5, 7, 8, 9, 10, 12, 13};
#define SWEEP ((int)(sizeof sweep / sizeof sweep[0]))

static uint8_t hop_channel(int index) { return index % 2 == 0 ? HOME : sweep[index / 2 % SWEEP]; }

static void header(uint8_t *f, uint8_t type) { f[0] = 'a', f[1] = 'M', f[2] = VERSION, f[3] = type; }

static void send_hello(espnow_link_t *l, uint32_t now) {
    uint8_t f[HEADER + GROUP_FIELD + 2 + sizeof l->name];
    header(f, TYPE_HELLO);
    memset(f + HEADER, 0, GROUP_FIELD);
    memcpy(f + HEADER, l->group, strlen(l->group));
    f[HEADER + GROUP_FIELD] = (uint8_t)(l->tx_seq >> 8), f[HEADER + GROUP_FIELD + 1] = (uint8_t)l->tx_seq;
    size_t n = strlen(l->name) + 1;
    memcpy(f + HEADER + GROUP_FIELD + 2, l->name, n);
    l->cb.send(l->ctx, NULL, f, HEADER + GROUP_FIELD + 2 + n);
    l->last_hello_ms = now;
}

void espnow_link_init(espnow_link_t *l, const char *group, const char *name, const espnow_callbacks_t *cb, void *ctx,
                      uint32_t now) {
    memset(l, 0, sizeof *l);
    snprintf(l->group, sizeof l->group, "%s", group);
    snprintf(l->name, sizeof l->name, "%s", name);
    l->cb = *cb;
    l->ctx = ctx;
    l->channel = hop_channel(0);
    l->channel_since_ms = now;
    uint32_t h = 2166136261u;  // FNV-1a of the name: each box its own pace
    for (const char *c = name; *c; c++) h = (h ^ (uint8_t)*c) * 16777619u;
    l->dwell_ms = ESPNOW_HOP_MS + h % 5 * 200;
    l->last_hello_ms = now - ESPNOW_HELLO_MS;  // the first hello goes out at the first tick
    if (l->cb.set_channel) l->cb.set_channel(l->ctx, l->channel);
}

static espnow_peer_t *find(espnow_link_t *l, const uint8_t mac[6]) {
    for (int i = 0; i < ESPNOW_PEERS; i++)
        if (l->peers[i].used && !memcmp(l->peers[i].mac, mac, 6)) return &l->peers[i];
    return NULL;
}

int espnow_link_peer_count(const espnow_link_t *l) {
    int n = 0;
    for (int i = 0; i < ESPNOW_PEERS; i++) n += l->peers[i].used;
    return n;
}

static void hello(espnow_link_t *l, const uint8_t mac[6], const uint8_t *body, size_t len, uint32_t now) {
    if (len < GROUP_FIELD + 3) return;
    l->stats.rx_hellos++;
    char group[GROUP_FIELD + 1] = {0};
    memcpy(group, body, GROUP_FIELD);
    if (strcmp(group, l->group)) {  // another cable
        l->stats.rx_other_group++;
        return;
    }
    espnow_peer_t *p = find(l, mac);
    if (!p) {
        for (int i = 0; i < ESPNOW_PEERS && !p; i++)
            if (!l->peers[i].used) p = &l->peers[i];
        if (!p) return;  // full
        memset(p, 0, sizeof *p);
        p->used = true;
        memcpy(p->mac, mac, 6);
        // Its next data frame is the one after this: a loss of the very first one counts too.
        p->rx_seq = (uint16_t)((body[GROUP_FIELD] << 8 | body[GROUP_FIELD + 1]) - 1);
        p->rx_started = true;
        const uint8_t *nm = body + GROUP_FIELD + 2;
        size_t n = 0;
        while (n < len - GROUP_FIELD - 2 && n + 1 < sizeof p->name && nm[n]) n++;
        memcpy(p->name, nm, n);
        p->name[n] = 0;
        p->last_heard_ms = now;
        l->cb.peer_changed(l->ctx, mac, true, p->name);
        send_hello(l, now);  // so the new peer knows us at once, not after up to a second
    }
    p->last_heard_ms = now;
}

static void data(espnow_link_t *l, const uint8_t mac[6], const uint8_t *body, size_t len, uint32_t now) {
    espnow_peer_t *p = find(l, mac);
    if (!p || len < 2) return;  // only from peers (a hello comes first)
    p->last_heard_ms = now;
    uint16_t seq = (uint16_t)(body[0] << 8 | body[1]);
    if (p->rx_started) {
        int16_t d = (int16_t)(seq - p->rx_seq);
        if (d <= 0) {  // a radio-level retry whose acknowledgement was lost: already delivered
            l->stats.rx_dups++;
            return;
        }
        l->stats.rx_lost += (uint32_t)(d - 1);
    }
    p->rx_started = true;
    p->rx_seq = seq;
    l->stats.rx_frames++;
    size_t pos = 2;
    while (pos + 4 <= len) {
        ump_t u = {{0}};
        u.w[0] =
            (uint32_t)body[pos] << 24 | (uint32_t)body[pos + 1] << 16 | (uint32_t)body[pos + 2] << 8 | body[pos + 3];
        int nw = ump_words(&u);
        if (pos + 4 * (size_t)nw > len) {
            l->stats.rx_bad++;
            return;
        }
        for (int k = 1; k < nw; k++) {
            const uint8_t *b = body + pos + 4 * k;
            u.w[k] = (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
        }
        l->cb.deliver(l->ctx, &u);
        pos += 4 * (size_t)nw;
    }
}

void espnow_link_receive(espnow_link_t *l, const uint8_t mac[6], const uint8_t *f, size_t len, uint32_t now) {
    if (len < HEADER || f[0] != 'a' || f[1] != 'M' || f[2] != VERSION) {
        l->stats.rx_bad++;
        return;
    }
    if (f[3] == TYPE_HELLO) hello(l, mac, f + HEADER, len - HEADER, now);
    if (f[3] == TYPE_DATA) data(l, mac, f + HEADER, len - HEADER, now);
}

static void send_frame(espnow_link_t *l, uint8_t *f, size_t len) {
    f[HEADER] = (uint8_t)(l->tx_seq >> 8), f[HEADER + 1] = (uint8_t)l->tx_seq;
    for (int i = 0; i < ESPNOW_PEERS; i++)
        if (l->peers[i].used) l->cb.send(l->ctx, l->peers[i].mac, f, len);
    l->tx_seq++;
    l->stats.tx_frames++;
}

int espnow_link_send(espnow_link_t *l, const ump_t *u, int count, uint32_t now) {
    (void)now;
    int peers = espnow_link_peer_count(l);
    if (!peers) return 0;
    uint8_t f[ESPNOW_FRAME_MAX];
    header(f, TYPE_DATA);
    size_t len = HEADER + 2;
    for (int i = 0; i < count; i++) {
        int nw = ump_words(&u[i]);
        if (len + 4 * (size_t)nw > sizeof f) send_frame(l, f, len), len = HEADER + 2;  // a UMP is never split
        for (int k = 0; k < nw; k++) {
            uint32_t w = u[i].w[k];
            f[len++] = (uint8_t)(w >> 24), f[len++] = (uint8_t)(w >> 16), f[len++] = (uint8_t)(w >> 8);
            f[len++] = (uint8_t)w;
        }
    }
    if (len > HEADER + 2) send_frame(l, f, len);
    return peers;
}

void espnow_link_tick(espnow_link_t *l, uint32_t now) {
    for (int i = 0; i < ESPNOW_PEERS; i++) {
        espnow_peer_t *p = &l->peers[i];
        if (p->used && now - p->last_heard_ms >= ESPNOW_PEER_TIMEOUT_MS) {
            p->used = false;
            l->cb.peer_changed(l->ctx, p->mac, false, p->name);
        }
    }
    // Alone and free to choose: try the next channel.
    if (!l->fixed_channel && !espnow_link_peer_count(l) && now - l->channel_since_ms >= l->dwell_ms) {
        l->hop_index = (l->hop_index + 1) % (2 * SWEEP);
        l->channel = hop_channel(l->hop_index);
        l->channel_since_ms = now;
        if (l->cb.set_channel) l->cb.set_channel(l->ctx, l->channel);
        send_hello(l, now);
    }
    if (now - l->last_hello_ms >= ESPNOW_HELLO_MS) send_hello(l, now);
}

void espnow_link_set_ap_channel(espnow_link_t *l, uint8_t channel, uint32_t now) {
    l->fixed_channel = channel != 0;
    if (channel) l->channel = channel;
    l->channel_since_ms = now;
}
