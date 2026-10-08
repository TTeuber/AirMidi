// Firmware images: which slot is running, confirming a new image after a self-test (or rolling back), and the
// core dump a crash left behind. Updates themselves (upload, GitHub Releases) build on this in later steps.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

// Log the running slot, its OTA state, and a summary of any core dump. Call early at boot.
void ota_boot_report(void);

// True while the running image has not been confirmed yet (first boot after an update).
bool ota_pending_verify(void);

// If the image is pending verification: after `ms` of uptime, ask `healthy()`; true marks the image valid,
// false (or a crash before then) rolls back to the previous one. Does nothing for a confirmed image.
void ota_confirm_after(uint32_t ms, bool (*healthy)(void));

typedef struct {
    bool present;
    uint32_t size;
    char task[16];
    uint32_t pc;
} ota_coredump_t;

void ota_coredump_info(ota_coredump_t *out);
// Stream the core dump out in chunks (the web UI's download); returns false if there is none.
bool ota_coredump_read(bool (*sink)(void *ctx, const void *data, size_t len), void *ctx);

// Forget the stored core dump (after it has been downloaded).
void ota_coredump_erase(void);

typedef struct {
    char running[17];      // partition label
    const char *state;     // "valid", "pending verify", ...
    char rolled_back[17];  // label of a slot whose image failed, "" if none
} ota_info_t;

void ota_get_info(ota_info_t *out);
const char *ota_reset_reason(void);

// Receiving an update: begin, write the image in pieces as it arrives, end. ota_end() checks the image (and its
// signature, when signed images are required) and makes it the next boot; the caller then restarts.
esp_err_t ota_begin(void);
esp_err_t ota_write(const void *data, size_t len);
esp_err_t ota_end(void);
void ota_abort(void);
