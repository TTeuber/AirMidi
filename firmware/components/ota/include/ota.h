// Firmware images: which slot is running, confirming a new image after a self-test (or rolling back), and the
// core dump a crash left behind. Updates themselves (upload, GitHub Releases) build on this in later steps.
#pragma once

#include <stdbool.h>
#include <stdint.h>

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
