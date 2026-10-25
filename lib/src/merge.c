#include "airmidi/merge.h"

#include <string.h>

void merge_init(merge_t *m, uint16_t timeout_ms) {
    memset(m, 0, sizeof *m);
    m->active = -1;
    m->timeout_ms = timeout_ms;
}

static bool is_real_time(const ump_t *u) { return ump_mt(u) == UMP_MT_SYSTEM && ump_status(u) >= 0xF8; }

// Channel voice messages whose latest value is all that counts. key: what a newer one replaces (status with
// channel, plus the note or controller where there is one). 0 = not coalescable. MIDI 1.0 (MT 2) and MIDI 2.0
// (MT 4) channel voice share the status byte's place in the first word.
static uint32_t value_key(const ump_t *u) {
    uint8_t mt = ump_mt(u);
    if (mt != UMP_MT_M1_CV && mt != UMP_MT_M2_CV) return 0;
    uint32_t w = u->w[0] & 0x0FFFFF00;  // group, status + channel, note / controller
    switch (ump_status(u) & 0xF0) {
        case 0xE0:  // pitch bend
        case 0xD0:  // channel pressure
            return (w & 0x0FFF0000) | 1;
        case 0xA0:  // poly pressure: per note
            return w | 1;
        case 0xB0: {
            uint8_t cc = ump_byte3(u);
            bool sequenced = cc == 0 || cc == 32 || cc == 6 || cc == 38 || (cc >= 96 && cc <= 101) || cc >= 120;
            return sequenced ? 0 : (w | 1);
        }
    }
    return 0;
}

static bool same_channel(const ump_t *a, const ump_t *b) {
    uint8_t ma = ump_mt(a), mb = ump_mt(b);
    bool cv_a = ma == UMP_MT_M1_CV || ma == UMP_MT_M2_CV, cv_b = mb == UMP_MT_M1_CV || mb == UMP_MT_M2_CV;
    return cv_a && cv_b && ((a->w[0] ^ b->w[0]) & 0x0F0F0000) == 0;  // group and channel
}

// Replace the newest queued value with the same key from the same source, if nothing for that channel from that
// source came after it (a note on in between would then start at the newer bend, or a note off lose its place).
static bool coalesce(merge_item_t *q, int n, int src, const ump_t *u) {
    uint32_t key = value_key(u);
    if (!key) return false;
    for (int i = n - 1; i >= 0; i--) {
        if (q[i].src != src || !same_channel(&q[i].u, u)) continue;
        if (value_key(&q[i].u) != key) return false;
        q[i].u = *u;
        return true;
    }
    return false;
}

static void append(merge_t *m, merge_item_t *q, int *n, int src, const ump_t *u) {
    if (coalesce(q, *n, src, u)) {
        m->coalesced++;
        return;
    }
    if (*n == MERGE_QUEUE) {
        m->dropped++;
        return;
    }
    q[*n].u = *u;
    q[*n].src = (uint8_t)src;
    (*n)++;
}

enum { PLACED_READY, PLACED_HELD, PLACED_DROPPED };

// Decide where one message goes: to the output now, to the held queue, or nowhere. Updates the SysEx state.
// must_wait: an earlier message from this source is still held, so this one may not overtake it.
static int place(merge_t *m, int src, const ump_t *u, uint32_t now, bool must_wait) {
    if (is_real_time(u)) {  // the one exception to keeping a source's order: real-time goes first
        append(m, m->ready, &m->n_ready, src, u);
        return PLACED_READY;
    }
    if (ump_mt(u) == UMP_MT_SYSEX7) {
        uint8_t st = ump_sysex7_status(u);
        bool starts = st == UMP_SYSEX_START || st == UMP_SYSEX_COMPLETE;
        uint32_t bit = 1u << (src & 31);
        if (starts) m->aborted &= ~bit;
        if (!starts && (m->aborted & bit)) {  // the rest of a SysEx the timeout already closed
            m->dropped++;
            return PLACED_DROPPED;
        }
        if (!must_wait && (m->active == -1 || m->active == src)) {
            append(m, m->ready, &m->n_ready, src, u);
            if (st == UMP_SYSEX_START) m->active = src, m->active_since = now, m->active_group = ump_group(u);
            if (st == UMP_SYSEX_END || st == UMP_SYSEX_COMPLETE) m->active = -1;
            return PLACED_READY;
        }
        append(m, m->held, &m->n_held, src, u);
        return PLACED_HELD;
    }
    if (!must_wait && m->active == -1) {
        append(m, m->ready, &m->n_ready, src, u);
        return PLACED_READY;
    }
    append(m, m->held, &m->n_held, src, u);  // even from the active source: it would cut its own SysEx
    return PLACED_HELD;
}

// Once no SysEx is in progress, give the held messages another go, in order. One of them may start a new SysEx;
// the ones after it that still have to wait stay held, in their order. (The copy is 1.3 KB of stack.)
// Within a pass, once a source has a message held again, its later messages are held too, so nothing overtakes
// an earlier message from the same source (a note off its note on, a SysEx CONTINUE its START). A pass can end
// with no SysEx in progress but messages held (a held SysEx started and ended within the pass): then go again.
// Every pass moves at least its first message on, so this ends.
static void release_held(merge_t *m, uint32_t now) {
    while (m->active == -1 && m->n_held) {
        merge_item_t pending[MERGE_QUEUE];
        int n = m->n_held;
        memcpy(pending, m->held, n * sizeof pending[0]);
        m->n_held = 0;
        uint32_t waiting = 0;  // bit per source with a message held in this pass
        for (int i = 0; i < n; i++) {
            uint32_t bit = 1u << (pending[i].src & 31);
            if (place(m, pending[i].src, &pending[i].u, now, waiting & bit) == PLACED_HELD) waiting |= bit;
        }
    }
}

void merge_push(merge_t *m, int src, const ump_t *u, uint32_t now_ms) {
    // Either no SysEx is in progress, and then nothing is held (release_held empties the queue), or one is, and then
    // only the active source's SysEx packets pass, overtaking that source's own held messages by design.
    place(m, src, u, now_ms, false);
    release_held(m, now_ms);
}

bool merge_pop(merge_t *m, ump_t *out, uint32_t now_ms) {
    if (m->active != -1 && now_ms - m->active_since >= m->timeout_ms) {
        // Close the stuck SysEx with an empty END from its source, so the output writes F7.
        ump_t end = ump_sysex7(m->active_group, UMP_SYSEX_END, NULL, 0);
        append(m, m->ready, &m->n_ready, m->active, &end);
        m->aborted |= 1u << (m->active & 31);
        m->active = -1;
        m->timeouts++;
        release_held(m, now_ms);
    }
    if (!m->n_ready) return false;
    *out = m->ready[0].u;
    m->n_ready--;
    memmove(m->ready, m->ready + 1, m->n_ready * sizeof m->ready[0]);
    return true;
}
