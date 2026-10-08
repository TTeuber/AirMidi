#include "airmidi/midici.h"

#include <stdio.h>
#include <string.h>

static void put7x4(uint8_t *p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = (v >> (7 * i)) & 0x7F;
}
static uint32_t get7x4(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 7 | (uint32_t)p[2] << 14 | (uint32_t)p[3] << 21;
}
static void put14(uint8_t *p, unsigned v) { p[0] = v & 0x7F, p[1] = (v >> 7) & 0x7F; }
static unsigned get14(const uint8_t *p) { return p[0] | p[1] << 7; }

uint32_t midici_muid(uint32_t r) {
    r &= 0x0FFFFFFF;
    if (r >= 0x0FFFFF00) r &= 0x0FFFFEFF;  // 0x0FFFFF00-0x0FFFFFFE reserved, 0x0FFFFFFF broadcast
    return r;
}

static size_t header(uint8_t *out, uint8_t sub2, uint32_t src, uint32_t dst) {
    out[0] = 0x7E, out[1] = 0x7F, out[2] = 0x0D, out[3] = sub2, out[4] = MIDICI_VERSION;
    put7x4(out + 5, src);
    put7x4(out + 9, dst);
    return MIDICI_HEADER;
}

bool midici_parse(const uint8_t *sx, size_t n, midici_msg_t *out) {
    if (n < MIDICI_HEADER || sx[0] != 0x7E || sx[2] != 0x0D) return false;
    for (size_t i = 0; i < n; i++)
        if (sx[i] & 0x80) return false;  // a status byte has no business inside SysEx contents
    out->device_id = sx[1];
    out->sub2 = sx[3];
    out->version = sx[4];
    out->src = get7x4(sx + 5);
    out->dst = get7x4(sx + 9);
    out->data = sx + MIDICI_HEADER;
    out->len = n - MIDICI_HEADER;
    return true;
}

static size_t ident(uint8_t *p, const midici_ident_t *id, uint8_t categories, uint32_t max_sysex) {
    memcpy(p, id->manufacturer, 3);
    put14(p + 3, id->family);
    put14(p + 5, id->model);
    memcpy(p + 7, id->version, 4);
    p[11] = categories;
    put7x4(p + 12, max_sysex);
    for (int i = 0; i < 16; i++) p[i] &= 0x7F;
    return 16;
}

size_t midici_discovery(uint8_t *out, uint32_t src, const midici_ident_t *id, uint8_t categories, uint32_t max_sysex,
                        uint8_t output_path) {
    size_t n = header(out, MIDICI_DISCOVERY, src, MIDICI_BROADCAST);
    n += ident(out + n, id, categories, max_sysex);
    out[n++] = output_path & 0x7F;
    return n;
}

size_t midici_discovery_reply(uint8_t *out, uint32_t src, uint32_t dst, const midici_ident_t *id, uint8_t categories,
                              uint32_t max_sysex, uint8_t output_path) {
    size_t n = header(out, MIDICI_DISCOVERY_REPLY, src, dst);
    n += ident(out + n, id, categories, max_sysex);
    out[n++] = output_path & 0x7F;
    out[n++] = 0x7F;  // function block: none (a MIDI 1.0 connection)
    return n;
}

bool midici_parse_discovery(const midici_msg_t *m, midici_discovery_t *out) {
    if (m->len < 16) return false;
    const uint8_t *p = m->data;
    memcpy(out->ident.manufacturer, p, 3);
    out->ident.family = get14(p + 3);
    out->ident.model = get14(p + 5);
    memcpy(out->ident.version, p + 7, 4);
    out->categories = p[11];
    out->max_sysex = get7x4(p + 12);
    out->output_path = m->len > 16 ? p[16] : 0;  // added in message version 2
    return true;
}

size_t midici_pe_caps(uint8_t *out, uint8_t sub2, uint32_t src, uint32_t dst, uint8_t simultaneous) {
    size_t n = header(out, sub2, src, dst);
    out[n++] = simultaneous & 0x7F;
    out[n++] = 0, out[n++] = 0;  // Property Exchange version 1.0/1.1
    return n;
}

size_t midici_pe_get(uint8_t *out, uint32_t src, uint32_t dst, uint8_t request_id, const char *resource,
                     const char *res_id) {
    if (strlen(resource) >= MIDICI_NAME_MAX || (res_id && strlen(res_id) >= MIDICI_NAME_MAX)) return 0;
    char hdr[2 * MIDICI_NAME_MAX + 32];
    int hl = res_id ? snprintf(hdr, sizeof hdr, "{\"resource\":\"%s\",\"resId\":\"%s\"}", resource, res_id)
                    : snprintf(hdr, sizeof hdr, "{\"resource\":\"%s\"}", resource);
    size_t n = header(out, MIDICI_PE_GET, src, dst);
    out[n++] = request_id & 0x7F;
    put14(out + n, (unsigned)hl), n += 2;
    for (int i = 0; i < hl; i++) out[n++] = hdr[i] & 0x7F;
    put14(out + n, 1), n += 2;  // a Get has no property data: one chunk (M2-101 8.3.2)
    put14(out + n, 1), n += 2;
    put14(out + n, 0), n += 2;
    return n;
}

size_t midici_pe_get_reply(uint8_t *out, uint32_t src, uint32_t dst, uint8_t request_id, const char *hdr,
                           uint16_t chunks, uint16_t this_chunk, const uint8_t *data, size_t len) {
    size_t hl = this_chunk == 1 && hdr ? strlen(hdr) : 0;
    size_t n = header(out, MIDICI_PE_GET_REPLY, src, dst);
    out[n++] = request_id & 0x7F;
    put14(out + n, (unsigned)hl), n += 2;
    for (size_t i = 0; i < hl; i++) out[n++] = hdr[i] & 0x7F;
    put14(out + n, chunks), n += 2;
    put14(out + n, this_chunk), n += 2;
    put14(out + n, (unsigned)len), n += 2;
    for (size_t i = 0; i < len; i++) out[n++] = data[i] & 0x7F;  // property data is 7-bit (JSON is ASCII)
    return n;
}

bool midici_parse_pe(const midici_msg_t *m, midici_pe_t *out) {
    const uint8_t *p = m->data;
    size_t left = m->len;
    if (left < 3) return false;
    out->request_id = p[0];
    out->header_len = get14(p + 1);
    p += 3, left -= 3;
    if (left < out->header_len + 6) return false;
    out->header = (const char *)p;
    p += out->header_len, left -= out->header_len;
    out->chunks = (uint16_t)get14(p);
    out->this_chunk = (uint16_t)get14(p + 2);
    out->data_len = get14(p + 4);
    p += 6, left -= 6;
    if (left < out->data_len) return false;
    out->data = p;
    return true;
}

size_t midici_ack_nak(uint8_t *out, uint8_t sub2, uint32_t src, uint32_t dst, uint8_t original_sub2, uint8_t status,
                      uint8_t status_data, uint8_t request_id) {
    size_t n = header(out, sub2, src, dst);
    out[n++] = original_sub2 & 0x7F;
    out[n++] = status & 0x7F;
    out[n++] = status_data & 0x7F;
    out[n++] = request_id & 0x7F;
    out[n++] = 0, out[n++] = 0, out[n++] = 0, out[n++] = 0;  // chunk number, reserved
    out[n++] = 0, out[n++] = 0;                              // no message text
    return n;
}

size_t midici_invalidate(uint8_t *out, uint32_t src, uint32_t target) {
    size_t n = header(out, MIDICI_INVALIDATE_MUID, src, MIDICI_BROADCAST);
    put7x4(out + n, target);
    return n + 4;
}

void midici_parse_ack(const midici_msg_t *m, midici_ack_t *out) {
    *out = (midici_ack_t){0};
    if (m->len < 8) return;
    out->original_sub2 = m->data[0];
    out->status = m->data[1];
    out->status_data = m->data[2];
    memcpy(out->details, m->data + 3, 5);
}

bool midici_parse_invalidate(const midici_msg_t *m, uint32_t *target) {
    if (m->len < 4) return false;
    *target = get7x4(m->data);
    return true;
}
