// The box's JSON API, apart from how it is carried: the web server (webui, /api/...) and the BLE config service
// (ble_config) both call api_call(), so a phone gets the same answers over BLE in Play mode as a browser over
// Wi-Fi. Firmware uploads and the core dump download stay HTTP only (binary, megabytes).
//
// Requests: a method ("GET", "POST", "DELETE"), a path that may carry a query ("/api/wifi?ssid=home"), and a body
// (JSON, or a control list for POST /api/ci/profile; may be empty). Replies: an HTTP status and JSON text.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "airmidi/settings.h"

typedef struct {
    int status;              // HTTP status code
    char *json;              // reply body, JSON text (malloc'd: free() it), NULL only when out of memory
    void (*then)(int arg);   // run once the reply has gone out (a mode switch takes the network away), or NULL
    int then_arg;
} api_reply_t;

typedef struct {
    void (*set_mode)(airmidi_mode_t mode);  // switch now and remember it
    uint32_t (*web_dropped)(void);          // words the web pages could not keep up with (status), or NULL
} api_hooks_t;

void api_init(const api_hooks_t *hooks);

// Largest request body api_call() takes (a control list profile).
#define API_BODY_MAX 16384

api_reply_t api_call(const char *method, const char *path, const char *body, size_t body_len);

// "400 Bad Request" etc., for the web server.
const char *api_status_line(int status);
