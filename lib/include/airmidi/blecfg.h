// Framing for the BLE config service: the box's JSON API (firmware/components/api) carried over two GATT
// characteristics, so a phone can do over BLE what a browser does over Wi-Fi.
//
// A message is split into pieces that fit one ATT write or notification. Each piece: a message id (1 byte), flags
// (1 byte, BLECFG_LAST on the final piece), then the next part of the message. A piece with a new id starts a new
// message, so a client that gave up on one simply sends the next.
//   request  (phone -> box, written):  "GET /api/status\n" then the body, if any
//   response (box -> phone, notified): "200\n" then the JSON reply; same id as its request
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BLECFG_HEADER 2
#define BLECFG_LAST 0x01

typedef struct {
    uint8_t *buf;  // the caller's buffer, cap bytes
    size_t cap, len;
    uint8_t id;
    bool active;    // a message is being assembled
    bool overflow;  // it did not fit: it is answered with an error when its last piece arrives
} blecfg_rx_t;

typedef enum { BLECFG_MORE, BLECFG_DONE, BLECFG_TOO_LONG, BLECFG_BAD } blecfg_result_t;

void blecfg_rx_init(blecfg_rx_t *rx, uint8_t *buf, size_t cap);

// One piece in. DONE: the whole message is in rx->buf[0..rx->len), with id rx->id. TOO_LONG: its last piece came
// but it did not fit. BAD: shorter than a header.
blecfg_result_t blecfg_rx_feed(blecfg_rx_t *rx, const uint8_t *piece, size_t n);

// The piece of msg that starts at *offset, at most piece_max bytes with the header, into out. Advances *offset;
// the piece is the last one once *offset == len. Returns its size. An empty message is one header-only piece.
size_t blecfg_piece(uint8_t id, const uint8_t *msg, size_t len, size_t *offset, size_t piece_max, uint8_t *out);

// A request: "METHOD PATH\n" then the body. Writes the method and the path as strings (truncated to their sizes)
// and points body at the rest. False if there is no newline or no space before it.
bool blecfg_parse_request(const uint8_t *msg, size_t len, char *method, size_t method_size, char *path,
                          size_t path_size, const uint8_t **body, size_t *body_len);

#ifdef __cplusplus
}
#endif
