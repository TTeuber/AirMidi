#include "airmidi/midici_initiator.h"

#include <string.h>

#include "airmidi/json_scan.h"

enum { IDLE, DISCOVERY, CAPS, GET, REPORT };

void midici_initiator_init(midici_initiator_t *in, uint32_t muid, const midici_ident_t *ident,
                           const midici_initiator_cb_t *cb, uint8_t *buf, size_t cap) {
    memset(in, 0, sizeof *in);
    in->muid = muid;
    in->ident = *ident;
    in->cb = *cb;
    in->buf = buf;
    in->cap = cap;
}

void midici_initiator_set_muid(midici_initiator_t *in, uint32_t muid) { in->muid = muid; }

bool midici_initiator_busy(const midici_initiator_t *in) { return in->state != IDLE; }

static void done(midici_initiator_t *in, midici_done_t r) {
    in->state = IDLE;  // first, so on_done may start the next scan
    if (in->cb.on_done) in->cb.on_done(in->cb.ctx, r);
}

static void add_get(midici_initiator_t *in, const char *resource, const char *res_id) {
    for (int i = 0; i < in->ngets; i++)
        if (!strcmp(in->gets[i].resource, resource) && !strcmp(in->gets[i].res_id, res_id)) return;
    if (in->ngets == MIDICI_MAX_GETS || strlen(resource) >= MIDICI_NAME_MAX || strlen(res_id) >= MIDICI_NAME_MAX)
        return;
    midici_get_t *g = &in->gets[in->ngets++];
    strcpy(g->resource, resource);
    strcpy(g->res_id, res_id);
}

static void send_get(midici_initiator_t *in, uint32_t now) {
    const midici_get_t *g = &in->gets[in->next];
    in->request_id = (in->request_id + 1) & 0x7F;
    in->len = 0, in->status = 0, in->next_chunk = 1, in->overflow = false;
    size_t n = midici_pe_get(in->out, in->muid, in->dev, in->request_id, g->resource, g->res_id[0] ? g->res_id : NULL);
    in->state = GET;
    in->deadline = now + MIDICI_REPLY_WAIT_MS;
    in->cb.send(in->cb.ctx, in->out, n);
}

// What a finished resource adds to the list of gets.
static void follow(midici_initiator_t *in, const midici_get_t *g) {
    const char *p = (const char *)in->buf;
    json_scan_t s;
    json_scan_init(&s, p, in->len);
    const char *k;
    size_t kl, v;
    char name[MIDICI_NAME_MAX];
    if (!strcmp(g->resource, "ResourceList")) {
        // [{"resource":"DeviceInfo"},{"resource":"AllCtrlList", ...}, ...]: only the ones the box can use
        while (json_scan_next(&s, &k, &kl, &v)) {
            if (kl != 8 || memcmp(k, "resource", 8) || !json_str_at(p, in->len, v, name, sizeof name)) continue;
            if (!strcmp(name, "DeviceInfo") || !strcmp(name, "AllCtrlList")) add_get(in, name, "");
            if (!strcmp(name, "CtrlMapList")) in->listed_ctrl_map = true;
        }
    } else if (!strcmp(g->resource, "AllCtrlList") && in->listed_ctrl_map) {
        // the value names of valueSelect controls (M2-117 4): one CtrlMapList per "ctrlMapId"
        while (json_scan_next(&s, &k, &kl, &v))
            if (kl == 9 && !memcmp(k, "ctrlMapId", 9) && json_str_at(p, in->len, v, name, sizeof name))
                add_get(in, "CtrlMapList", name);
    }
}

static void finish_get(midici_initiator_t *in, int status, uint32_t now) {
    const midici_get_t *g = &in->gets[in->next];
    bool ok = status == 200;
    if (in->cb.on_resource) in->cb.on_resource(in->cb.ctx, g->resource, g->res_id, status, in->buf, ok ? in->len : 0);
    if (ok) follow(in, g);
    if (status == MIDICI_STATUS_TIMEOUT) {
        done(in, MIDICI_DONE_TIMEOUT);  // the device is gone; asking for more would only wait again
        return;
    }
    if (++in->next < in->ngets)
        send_get(in, now);
    else
        done(in, MIDICI_DONE_OK);
}

static void report_done(midici_initiator_t *in, midici_report_done_t r) {
    in->state = IDLE;
    if (in->cb.on_report) in->cb.on_report(in->cb.ctx, r, in->report_types);
}

bool midici_initiator_can_report(const midici_initiator_t *in) {
    return in->state == IDLE && in->dev && (in->dev_categories & MIDICI_CAT_PROCESS);
}

bool midici_initiator_report(midici_initiator_t *in, uint8_t types, uint32_t now) {
    if (!midici_initiator_can_report(in)) return false;
    in->state = REPORT;
    in->report_types = 0;
    in->deadline = now + MIDICI_REPLY_WAIT_MS;
    size_t n = midici_report(in->out, MIDICI_DEVICE_FUNCTION_BLOCK, in->muid, in->dev, MIDICI_REPORT_FULL, 0, types, 0);
    in->cb.send(in->cb.ctx, in->out, n);
    return true;
}

void midici_initiator_start(midici_initiator_t *in, uint32_t now) {
    in->state = DISCOVERY;
    in->dev = 0;
    in->dev_categories = 0;
    in->saw_device_without_pe = false;
    in->listed_ctrl_map = false;
    in->ngets = in->next = 0;
    in->deadline = now + MIDICI_DISCOVERY_WAIT_MS;
    size_t n = midici_discovery(in->out, in->muid, &in->ident, MIDICI_CAT_PE | MIDICI_CAT_PROCESS, MIDICI_MAX_SYSEX, 0);
    in->cb.send(in->cb.ctx, in->out, n);
}

static void on_chunk(midici_initiator_t *in, const midici_msg_t *m, uint32_t now) {
    midici_pe_t pe;
    if (!midici_parse_pe(m, &pe) || pe.request_id != in->request_id) return;
    if (pe.this_chunk == 1) {
        int64_t st;
        // a reply without a status is taken as a success (lenient: the data decides)
        in->status = json_find_int(pe.header, pe.header_len, "status", &st) ? (int)st : 200;
    }
    if (pe.this_chunk != in->next_chunk) {
        // 0 = the responder gave up and does not know if what it sent is complete (M2-101 8.3)
        finish_get(in, MIDICI_STATUS_BAD_DATA, now);
        return;
    }
    if (in->len + pe.data_len > in->cap)
        in->overflow = true;
    else
        memcpy(in->buf + in->len, pe.data, pe.data_len), in->len += pe.data_len;
    in->deadline = now + MIDICI_REPLY_WAIT_MS;
    if (pe.chunks != 0 && pe.this_chunk >= pe.chunks)
        finish_get(in, in->overflow && in->status == 200 ? MIDICI_STATUS_TOO_LARGE : in->status, now);
    else
        in->next_chunk++;
}

void midici_initiator_receive(midici_initiator_t *in, const uint8_t *sx, size_t n, uint32_t now) {
    midici_msg_t m;
    if (in->state == IDLE || !midici_parse(sx, n, &m) || m.src == in->muid) return;
    if (m.sub2 == MIDICI_INVALIDATE_MUID) {
        uint32_t target;
        if (midici_parse_invalidate(&m, &target) &&
            (target == in->muid || (in->state != DISCOVERY && target == in->dev)))
            done(in, MIDICI_DONE_INVALIDATED);
        return;
    }
    // Everything else must be addressed to us: macOS sends its own Discovery on hardware ports, and a CI host
    // behind the box may be talking to the same device.
    if (m.dst != in->muid) return;
    if (in->state == DISCOVERY) {
        midici_discovery_t d;
        if (m.sub2 != MIDICI_DISCOVERY_REPLY || !midici_parse_discovery(&m, &d)) return;
        if (!(d.categories & MIDICI_CAT_PE)) {
            in->saw_device_without_pe = true;  // keep waiting: another device on the link may have PE
            return;
        }
        // the first device with Property Exchange wins: on a MIDI 1.0 cable there is one device
        in->dev = m.src;
        in->dev_categories = d.categories;
        if (in->cb.on_device && !in->cb.on_device(in->cb.ctx, m.src, &d)) {
            done(in, MIDICI_DONE_CACHED);
            return;
        }
        in->state = CAPS;
        in->deadline = now + MIDICI_REPLY_WAIT_MS;
        size_t k = midici_pe_caps(in->out, MIDICI_PE_CAPS, in->muid, in->dev, 1);
        in->cb.send(in->cb.ctx, in->out, k);
        return;
    }
    if (m.src != in->dev) return;
    midici_ack_t a;
    if (in->state == REPORT) {
        midici_report_t rep;
        if (m.sub2 == MIDICI_REPORT_REPLY && midici_parse_report(&m, &rep)) {
            in->report_types = rep.channel;
            in->deadline = now + MIDICI_REPLY_WAIT_MS;  // the values may take a while at 31250 baud
        } else if (m.sub2 == MIDICI_REPORT_END) {
            report_done(in, MIDICI_REPORT_OK);
        } else if (m.sub2 == MIDICI_NAK) {
            midici_parse_ack(&m, &a);
            if (a.original_sub2 == MIDICI_REPORT) report_done(in, MIDICI_REPORT_REFUSED);
        }
        return;
    }
    if (in->state == CAPS) {
        if (m.sub2 == MIDICI_PE_CAPS_REPLY) {
            add_get(in, "ResourceList", "");
            send_get(in, now);
        } else if (m.sub2 == MIDICI_NAK) {
            done(in, MIDICI_DONE_NO_PE);
        }
        return;
    }
    // GET
    switch (m.sub2) {
        case MIDICI_PE_GET_REPLY:
            on_chunk(in, &m, now);
            break;
        case MIDICI_NAK:
            midici_parse_ack(&m, &a);
            if (a.original_sub2 == MIDICI_PE_GET && a.details[0] == in->request_id)
                finish_get(in, MIDICI_STATUS_REFUSED, now);
            break;
        case MIDICI_ACK:
            midici_parse_ack(&m, &a);
            if (a.status == MIDICI_ACK_TIMEOUT_WAIT && a.details[0] == in->request_id)
                in->deadline += (uint32_t)a.status_data * 100;  // the responder asks for more time (M2-103 12.2.1)
            break;
    }
}

void midici_initiator_tick(midici_initiator_t *in, uint32_t now) {
    if (in->state == IDLE || (int32_t)(now - in->deadline) < 0) return;
    switch (in->state) {
        case DISCOVERY:
            done(in, in->saw_device_without_pe ? MIDICI_DONE_NO_PE : MIDICI_DONE_NO_DEVICE);
            break;
        case CAPS:
            done(in, MIDICI_DONE_TIMEOUT);
            break;
        case GET:
            finish_get(in, MIDICI_STATUS_TIMEOUT, now);
            break;
        case REPORT:
            report_done(in, MIDICI_REPORT_TIMEOUT);
            break;
    }
}
