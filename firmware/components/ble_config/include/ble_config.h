// The box's JSON API over BLE (lib/blecfg.h has the framing), next to BLE MIDI: the phone app sets the box up and
// changes its settings over BLE in any mode that has BLE, without joining the box's access point.
//
// Service 9A5C0010-3F2B-4D8E-A4B1-6C2E8F1D0A00:
//   ...0A11 request  (write, encrypted): pieces of "METHOD PATH\n" + body
//   ...0A12 response (notify): pieces of "STATUS\n" + JSON, with the request's id
//
// Who may use it: the link must be encrypted (the phone bonds, "just works"), and the phone must be one the box
// trusts. A phone becomes trusted by using the service while the pairing window is open: in Setup mode, for 2 min
// after a power-on, or for 2 min when a trusted phone (or the web page) asks for it to add another. Other phones get
// ATT error 0x08 (insufficient authorization) and the app explains how to open the window. Up to 4 phones, kept
// in NVS; /api/phones forgets them.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "host/ble_gatt.h"

typedef struct {
    int status;             // HTTP status code
    char *json;             // malloc'd; ble_config frees it
    void (*then)(int arg);  // run after the reply has been sent, or NULL
    int then_arg;
} ble_config_reply_t;

typedef ble_config_reply_t (*ble_config_handler_t)(const char *method, const char *path, const char *body,
                                                   size_t body_len);

// The service, for ble_gatts_count_cfg() / ble_gatts_add_svcs() while NimBLE is set up.
extern const struct ble_gatt_svc_def ble_config_services[];

// Start the worker that answers requests with handler (api_call, adapted by main).
void ble_config_start(ble_config_handler_t handler);

// The connection went away (ble_midi's GAP handler).
void ble_config_disconnected(uint16_t conn);

#define BLE_CONFIG_WINDOW_FOREVER UINT32_MAX
// Open the pairing window for seconds (FOREVER: until closed), 0 closes it.
void ble_config_set_window(uint32_t seconds);
// Seconds left, BLE_CONFIG_WINDOW_FOREVER, or 0 when closed.
uint32_t ble_config_window_left(void);

#define BLE_CONFIG_PHONES 4
int ble_config_phone_count(void);
void ble_config_forget_phones(void);
