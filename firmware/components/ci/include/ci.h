// MIDI-CI over TRS: the box asks the device on its jacks who it is and what it can be controlled with
// (lib/midici_initiator), keeps the answers in flash keyed by the device's identity, and hands them to the web UI,
// which draws the device's own controls. Devices without MIDI-CI get the same from a hand-written profile.
//
// The initiator is a bus port of its own (PORT_CI) that only exchanges SysEx with PORT_TRS. The device's replies
// also reach every other port, so a MIDI-CI host behind the box (macOS) can talk to the device as well.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    bool scanning;
    const char *result;  // of the last scan: "" before the first, "ok", "cached", "no device", "no property
                         // exchange", "timeout", "invalidated"
    uint32_t scan_ms;    // how long the last scan took
    uint32_t muid, device_muid;
    char device[32];     // identity key of the device found ("" if none): manufacturer-family-model-version, hex
    const char *source;  // where the control list comes from: "midi-ci", "cache", "profile" or "" (none)
    uint32_t dropped;    // MIDI-CI messages lost between the bus and the CI task
    const char *report;  // of the last MIDI Message Report: "" before the first, "asking", "ok", "refused",
                         // "timeout", "not supported" (the device does not do Process Inquiry), "no device"
} ci_status_t;

// Mount the storage partition, register PORT_CI and start the CI task; the first scan runs 2 s after boot.
void ci_start(void);

// Ask the device again (button short press, web UI). force: read everything even if the device is cached.
void ci_scan(bool force);

// Ask the device for a MIDI Message Report (M2-101 9): its current values come back over TRS as ordinary MIDI
// messages, which the bus routes like any others (to the web page, a phone, ...). Runs once no scan is in progress.
void ci_report(void);

void ci_get_status(ci_status_t *out);

// The parts of the device description the web UI needs, as JSON text that was valid when it was stored:
// "info" (DeviceInfo), "controls" (AllCtrlList), "maps" ({"<ctrlMapId>": CtrlMapList, ...}). Writes them through
// sink as one JSON object {"info":...,"controls":...,"maps":...} (null where missing). Returns false if sink failed.
bool ci_write_device(bool (*sink)(void *ctx, const char *text, size_t len), void *ctx);

// The hand-written profile: an AllCtrlList array, or {"info":{...},"controls":[...],"maps":{...}}. Validated as
// JSON (ESP_ERR_INVALID_ARG if not), stored in flash, used when no MIDI-CI device answers. NULL/0 deletes it.
esp_err_t ci_set_profile(const char *json, size_t len);
