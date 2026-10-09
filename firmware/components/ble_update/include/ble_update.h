// Firmware update over BLE: a GATT service next to BLE MIDI, for when the box is not on Wi-Fi (Play mode). The same
// signed images as the web upload; ota_end() checks the signature, so a stranger in BLE range can start an upload
// but never install anything we did not sign.
//
// Service 9A5C0000-3F2B-4D8E-A4B1-6C2E8F1D0A00:
//   ...0A01 control (write, notify). Writes: 01 + size (u32 LE) begins, 02 ends, 03 aborts. Notifications:
//           00 ready, 01 + bytes written (u32 LE) progress, 02 accepted (the box restarts into it),
//           03 + reason (text) failed, 04 + offset (u32 LE) resend from there.
//   ...0A02 data (write without response): offset (u32 LE) + a piece of the image.
//   ...0A03 version (read): the running firmware's version string.
// Writes without response can be lost on the way into NimBLE when a burst outruns its buffers (measured: an image
// came up short), so each piece says where it belongs: the box takes only the next one it expects and, after a gap,
// asks for everything again from there (go-back-N). The client keeps at most BLE_UPDATE_WINDOW bytes beyond the
// last progress notification (sent every 4 KB written), as flash writes are slower than BLE at times.
#pragma once

#include <stdbool.h>

#include "host/ble_gatt.h"

#define BLE_UPDATE_WINDOW (16 * 1024)

// The service, for ble_gatts_count_cfg() / ble_gatts_add_svcs() while NimBLE is set up.
extern const struct ble_gatt_svc_def ble_update_services[];

// True while an upload is in progress.
bool ble_update_active(void);

// The connection went away: an upload in progress is abandoned.
void ble_update_disconnected(uint16_t conn);
