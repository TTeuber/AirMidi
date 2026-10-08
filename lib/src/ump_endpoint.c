#include "airmidi/ump_endpoint.h"

#include <string.h>

enum {
    ST_ENDPOINT_DISCOVERY = 0x000,
    ST_ENDPOINT_INFO = 0x001,
    ST_DEVICE_IDENTITY = 0x002,
    ST_ENDPOINT_NAME = 0x003,
    ST_PRODUCT_INSTANCE_ID = 0x004,
    ST_CONFIG_REQUEST = 0x005,
    ST_CONFIG_NOTIFY = 0x006,
    ST_FB_DISCOVERY = 0x010,
    ST_FB_INFO = 0x011,
    ST_FB_NAME = 0x012,
};

static uint32_t head(int format, int status) { return 0xF0000000u | (uint32_t)format << 26 | (uint32_t)status << 16; }

static uint32_t be32(const uint8_t *b) { return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | b[2] << 8 | b[3]; }

// A text field in as many UMPs as it takes: `first` bytes after the status in each (2, or 1 after a block number),
// then the other three words. Format: complete, or start / continue... / end.
static int text(ump_t *out, int max, int status, int prefix, uint8_t prefix_byte, const char *s, size_t limit) {
    size_t len = strlen(s);
    if (len > limit) len = limit;
    int n = 0;
    size_t off = 0;
    do {
        uint8_t b[16] = {0};
        int k = 2;
        if (prefix) b[k++] = prefix_byte;
        size_t take = len - off < (size_t)(16 - k) ? len - off : (size_t)(16 - k);
        memcpy(b + k, s + off, take);
        bool first = off == 0, last = off + take >= len;
        int format = first && last ? 0 : first ? 1 : last ? 3 : 2;
        off += take;
        if (n == max) break;
        ump_t u = {{be32(b), be32(b + 4), be32(b + 8), be32(b + 12)}};
        u.w[0] = (u.w[0] & 0xFFFF) | head(format, status);
        out[n++] = u;
    } while (off < len);
    return n;
}

static ump_t config_notification(const ump_endpoint_t *ep) {
    ump_t u = {{head(0, ST_CONFIG_NOTIFY) | (uint32_t)ep->protocol << 8}};
    return u;
}

int ump_endpoint_handle(ump_endpoint_t *ep, const ump_t *req, ump_t out[UMP_ENDPOINT_MAX_REPLY]) {
    if (ump_mt(req) != UMP_MT_STREAM) return 0;
    int status = (req->w[0] >> 16) & 0x3FF, n = 0;
    switch (status) {
        case ST_ENDPOINT_DISCOVERY: {
            uint8_t filter = req->w[1] & 0xFF;
            if (filter & 0x01) {  // UMP 1.1; static function blocks, 1 of them; protocols; no JR timestamps
                ump_t u = {{head(0, ST_ENDPOINT_INFO) | 0x0101,
                            1u << 31 | 1u << 24 | (ep->midi2_capable ? 1u << 9 : 0) | 1u << 8}};
                out[n++] = u;
            }
            if (filter & 0x02) {
                ump_t u = {{head(0, ST_DEVICE_IDENTITY),
                            (uint32_t)(ep->manufacturer[0] & 0x7F) << 16 | (ep->manufacturer[1] & 0x7F) << 8 |
                                (ep->manufacturer[2] & 0x7F),
                            (uint32_t)(ep->family & 0x7F) << 24 | (uint32_t)((ep->family >> 7) & 0x7F) << 16 |
                                (ep->model & 0x7F) << 8 | ((ep->model >> 7) & 0x7F),
                            (uint32_t)(ep->version[0] & 0x7F) << 24 | (uint32_t)(ep->version[1] & 0x7F) << 16 |
                                (ep->version[2] & 0x7F) << 8 | (ep->version[3] & 0x7F)}};
                out[n++] = u;
            }
            if (filter & 0x04) n += text(out + n, UMP_ENDPOINT_MAX_REPLY - n, ST_ENDPOINT_NAME, 0, 0, ep->name, 98);
            if (filter & 0x08)
                n += text(out + n, UMP_ENDPOINT_MAX_REPLY - n, ST_PRODUCT_INSTANCE_ID, 0, 0, ep->product_instance_id,
                          42);
            if (filter & 0x10) out[n++] = config_notification(ep);
            return n;
        }
        case ST_CONFIG_REQUEST: {
            uint8_t want = (req->w[0] >> 8) & 0xFF;
            if (want == 1 || (want == 2 && ep->midi2_capable)) ep->protocol = want;
            out[0] = config_notification(ep);  // the configuration now in force, granted or not
            return 1;
        }
        case ST_FB_DISCOVERY: {
            uint8_t block = (req->w[0] >> 8) & 0xFF, filter = req->w[0] & 0xFF;
            if (block != 0 && block != 0xFF) return 0;
            if (filter & 0x01) {
                // active, block 0, UI hint sender+receiver (0b11), not a MIDI 1.0 port, bidirectional (0b11);
                // first group 0, 1 group, the MIDI-CI version, no SysEx8 streams
                ump_t u = {{head(0, ST_FB_INFO) | 1u << 15 | 0x3u << 4 | 0x3u,
                            0x00010000 | (uint32_t)(ep->ci_version & 0x7F) << 8}};
                out[n++] = u;
            }
            if (filter & 0x02) n += text(out + n, UMP_ENDPOINT_MAX_REPLY - n, ST_FB_NAME, 1, 0, ep->block_name, 91);
            return n;
        }
        default:
            return 0;  // notifications from the other side, or requests we do not implement
    }
}

int ump_endpoint_announce(ump_endpoint_t *ep, ump_t out[UMP_ENDPOINT_MAX_REPLY]) {
    ump_t all = {{head(0, ST_ENDPOINT_DISCOVERY) | 0x0101, 0x1F}};  // UMP 1.1, every filter bit
    return ump_endpoint_handle(ep, &all, out);
}
