#include "airmidi/blecfg.h"

#include <string.h>

void blecfg_rx_init(blecfg_rx_t *rx, uint8_t *buf, size_t cap) {
    memset(rx, 0, sizeof *rx);
    rx->buf = buf;
    rx->cap = cap;
}

blecfg_result_t blecfg_rx_feed(blecfg_rx_t *rx, const uint8_t *piece, size_t n) {
    if (n < BLECFG_HEADER) return BLECFG_BAD;
    uint8_t id = piece[0], flags = piece[1];
    if (!rx->active || id != rx->id) {  // a new message (an unfinished one before it is dropped)
        rx->active = true;
        rx->id = id;
        rx->len = 0;
        rx->overflow = false;
    }
    size_t part = n - BLECFG_HEADER;
    if (rx->overflow || rx->len + part > rx->cap) {
        rx->overflow = true;
    } else {
        memcpy(rx->buf + rx->len, piece + BLECFG_HEADER, part);
        rx->len += part;
    }
    if (!(flags & BLECFG_LAST)) return BLECFG_MORE;
    rx->active = false;
    return rx->overflow ? BLECFG_TOO_LONG : BLECFG_DONE;
}

size_t blecfg_piece(uint8_t id, const uint8_t *msg, size_t len, size_t *offset, size_t piece_max, uint8_t *out) {
    size_t room = piece_max > BLECFG_HEADER ? piece_max - BLECFG_HEADER : 0;
    size_t part = len - *offset < room ? len - *offset : room;
    if (part) memcpy(out + BLECFG_HEADER, msg + *offset, part);
    *offset += part;
    out[0] = id;
    out[1] = *offset == len ? BLECFG_LAST : 0;
    return BLECFG_HEADER + part;
}

bool blecfg_parse_request(const uint8_t *msg, size_t len, char *method, size_t method_size, char *path,
                          size_t path_size, const uint8_t **body, size_t *body_len) {
    const uint8_t *nl = memchr(msg, '\n', len);
    if (!nl) return false;
    const uint8_t *sp = memchr(msg, ' ', (size_t)(nl - msg));
    if (!sp || sp == msg || sp + 1 == nl) return false;
    size_t ml = (size_t)(sp - msg), pl = (size_t)(nl - sp - 1);
    if (ml >= method_size) ml = method_size - 1;
    if (pl >= path_size) pl = path_size - 1;
    memcpy(method, msg, ml);
    method[ml] = 0;
    memcpy(path, sp + 1, pl);
    path[pl] = 0;
    *body = nl + 1;
    *body_len = len - (size_t)(nl + 1 - msg);
    return true;
}
