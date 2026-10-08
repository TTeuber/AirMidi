// Settings kept in NVS (namespace "airmidi"), one key per field plus a schema number. Separate keys, not one
// blob, so an older firmware (after an OTA rollback) still reads the fields it knows and ignores newer ones.
#pragma once

#include "airmidi/settings.h"
#include "esp_err.h"

// Initialise NVS (erasing it only if the partition is full or from an incompatible NVS version), then load the
// settings, filling in defaults for missing keys and migrating older schemas. Call once at boot.
esp_err_t nvs_settings_init(void);

// A copy of the current settings (safe from any task).
void nvs_settings_get(settings_t *out);

// Validate and store. Returns ESP_ERR_INVALID_ARG (and logs why) if settings_check() rejects them.
esp_err_t nvs_settings_set(const settings_t *s);
