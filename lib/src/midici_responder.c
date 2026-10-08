#include "airmidi/midici_responder.h"

#include <stdio.h>
#include <string.h>

#include "airmidi/json_scan.h"

bool midici_responder_init(midici_responder_t *r, uint32_t muid, const midici_ident_t *ident,
                           const midici_resource_t *res, int nres, void (*send)(void *ctx, const uint8_t *sx, size_t n),
                           void *ctx) {
    memset(r, 0, sizeof *r);
    r->muid = muid;
    r->ident = *ident;
    r->res = res;
    r->nres = nres;
    r->send = send;
    r->ctx = ctx;
    // ResourceList: each resource name once; a resource with a resId needs one to be asked for (M2-103 14)
    size_t n = 0, cap = sizeof r->list;
    n += (size_t)snprintf(r->list, cap, "[");
    for (int i = 0; i < nres && n < cap; i++) {
        bool seen = false;
        for (int j = 0; j < i; j++) seen |= !strcmp(res[j].resource, res[i].resource);
        if (seen) continue;
        n += (size_t)snprintf(r->list + n, cap - n, "%s{\"resource\":\"%s\"%s}", n > 1 ? "," : "", res[i].resource,
                              res[i].res_id ? ",\"requireId\":true" : "");
    }
    if (n < cap) n += (size_t)snprintf(r->list + n, cap - n, "]");
    r->list_len = n < cap ? n : 0;
    return n < cap;
}

static uint32_t peer_max(const midici_responder_t *r, uint32_t muid) {
    for (int i = 0; i < MIDICI_RESPONDER_PEERS; i++)
        if (r->peers[i].muid == muid && r->peers[i].max_sysex) return r->peers[i].max_sysex;
    return 128;  // every MIDI-CI device takes at least 128 bytes (M2-101 5.5.3)
}

static void remember(midici_responder_t *r, uint32_t muid, uint32_t max_sysex) {
    int slot = -1;
    for (int i = 0; i < MIDICI_RESPONDER_PEERS; i++)
        if (r->peers[i].muid == muid && r->peers[i].max_sysex) slot = i;
    if (slot < 0) slot = (int)(r->next_peer++ % MIDICI_RESPONDER_PEERS);  // the oldest one goes
    r->peers[slot].muid = muid;
    r->peers[slot].max_sysex = max_sysex < 128 ? 128 : max_sysex;
}

static void forget(midici_responder_t *r, uint32_t muid) {
    for (int i = 0; i < MIDICI_RESPONDER_PEERS; i++)
        if (r->peers[i].muid == muid) r->peers[i].max_sysex = 0;
}

static const char *lookup(const midici_responder_t *r, const char *resource, const char *res_id, size_t *len) {
    if (!strcmp(resource, "ResourceList")) {
        *len = r->list_len;
        return r->list;
    }
    for (int i = 0; i < r->nres; i++) {
        const midici_resource_t *e = &r->res[i];
        if (strcmp(e->resource, resource)) continue;
        if ((e->res_id == NULL) != (res_id[0] == 0) || (e->res_id && strcmp(e->res_id, res_id))) continue;
        *len = strlen(e->json);
        return e->json;
    }
    return NULL;
}

// Reply to Get Property Data, in as many chunks as the initiator's max SysEx size needs.
static void reply(midici_responder_t *r, uint32_t dst, uint8_t request_id, const char *hdr, const char *data,
                  size_t len) {
    uint32_t max = peer_max(r, dst);
    if (max > sizeof r->out) max = sizeof r->out;
    size_t room = max - MIDICI_PE_OVERHEAD - strlen(hdr);  // the same for every chunk: simple, a few bytes lost
    uint16_t chunks = len ? (uint16_t)((len + room - 1) / room) : 1;
    for (uint16_t c = 1; c <= chunks; c++) {
        size_t off = (size_t)(c - 1) * room, k = len - off < room ? len - off : room;
        if (!len) k = 0;
        size_t n =
            midici_pe_get_reply(r->out, r->muid, dst, request_id, hdr, chunks, c, (const uint8_t *)data + off, k);
        r->send(r->ctx, r->out, n);
    }
}

static void on_get(midici_responder_t *r, const midici_msg_t *m) {
    midici_pe_t pe;
    if (!midici_parse_pe(m, &pe)) return;
    char resource[MIDICI_NAME_MAX], res_id[MIDICI_NAME_MAX] = "";
    if (!json_find_str(pe.header, pe.header_len, "resource", resource, sizeof resource)) {
        reply(r, m->src, pe.request_id, "{\"status\":400,\"message\":\"no resource\"}", NULL, 0);
        return;
    }
    json_find_str(pe.header, pe.header_len, "resId", res_id, sizeof res_id);
    size_t len;
    const char *data = lookup(r, resource, res_id, &len);
    if (data)
        reply(r, m->src, pe.request_id, "{\"status\":200}", data, len);
    else
        reply(r, m->src, pe.request_id, "{\"status\":404}", NULL, 0);
}

void midici_responder_receive(midici_responder_t *r, const uint8_t *sx, size_t n) {
    midici_msg_t m;
    if (!midici_parse(sx, n, &m) || m.src == r->muid) return;
    if (m.sub2 == MIDICI_DISCOVERY && (m.dst == MIDICI_BROADCAST || m.dst == r->muid)) {
        midici_discovery_t d;
        if (!midici_parse_discovery(&m, &d)) return;
        remember(r, m.src, d.max_sysex);
        size_t k = midici_discovery_reply(r->out, r->muid, m.src, &r->ident, MIDICI_CAT_PE, MIDICI_RESPONDER_OUT,
                                          d.output_path);
        r->send(r->ctx, r->out, k);
        return;
    }
    if (m.sub2 == MIDICI_INVALIDATE_MUID) {
        uint32_t target;
        if (midici_parse_invalidate(&m, &target)) forget(r, target);
        return;
    }
    if (m.dst != r->muid) return;
    size_t k;
    switch (m.sub2) {
        case MIDICI_PE_CAPS:
            k = midici_pe_caps(r->out, MIDICI_PE_CAPS_REPLY, r->muid, m.src, 1);
            r->send(r->ctx, r->out, k);
            break;
        case MIDICI_PE_GET:
            on_get(r, &m);
            break;
        case 0x20:  // Profile Inquiry
        case 0x22:  // Set Profile On
        case 0x23:  // Set Profile Off
        case 0x36:  // Set Property Data
        case 0x38:  // Subscription
        case 0x40:  // Process Inquiry Capabilities
        case 0x41:  // MIDI Message Report
            k = midici_ack_nak(r->out, MIDICI_NAK, r->muid, m.src, m.sub2, MIDICI_NAK_NOT_SUPPORTED, 0,
                               m.sub2 >= 0x30 && m.sub2 < 0x40 && m.len ? m.data[0] : 0);
            r->send(r->ctx, r->out, k);
            break;
        default:  // replies, ACKs, NAKs and what is not defined: never answered, so two devices cannot loop
            break;
    }
}
