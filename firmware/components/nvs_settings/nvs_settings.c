#include "nvs_settings.h"

#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "settings";

#define NAMESPACE "airmidi"
#define SCHEMA 1  // bump when a field changes meaning; add a step to migrate()

static settings_t current;
static SemaphoreHandle_t lock;

static esp_err_t write_all(nvs_handle_t h, const settings_t *s) {
    esp_err_t e = nvs_set_str(h, "name", s->name);
    if (!e) e = nvs_set_u8(h, "mode", (uint8_t)s->mode);
    if (!e) e = nvs_set_u16(h, "sysex_to", s->sysex_timeout_ms);
    if (!e) e = nvs_set_u8(h, "schema", SCHEMA);
    if (!e) e = nvs_commit(h);
    return e;
}

// Bring a store written by an older firmware up to SCHEMA, one step at a time. Schema 1 is the first, so there is
// nothing to do yet; a future change looks like `case 1: <convert>; /* fall through */ case 2: ...`.
static void migrate(nvs_handle_t h, uint8_t from) {
    switch (from) {
        default:
            break;
    }
    nvs_set_u8(h, "schema", SCHEMA);
    nvs_commit(h);
}

esp_err_t nvs_settings_init(void) {
    lock = xSemaphoreCreateMutex();
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition unusable (%s), erasing", esp_err_to_name(e));
        ESP_ERROR_CHECK(nvs_flash_erase());
        e = nvs_flash_init();
    }
    if (e) return e;

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_BASE);
    settings_t s;
    settings_defaults(&s, mac);

    nvs_handle_t h;
    if ((e = nvs_open(NAMESPACE, NVS_READWRITE, &h))) return e;
    uint8_t schema = 0;
    if (nvs_get_u8(h, "schema", &schema) == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "first boot: storing defaults");
        e = write_all(h, &s);
    } else {
        if (schema < SCHEMA) {
            ESP_LOGI(TAG, "migrating settings from schema %u to %u", schema, SCHEMA);
            migrate(h, schema);
        } else if (schema > SCHEMA) {
            ESP_LOGW(TAG, "settings written by newer firmware (schema %u); reading the fields this one knows", schema);
        }
        size_t len = sizeof s.name;
        nvs_get_str(h, "name", s.name, &len);  // missing keys keep their defaults
        uint8_t mode = s.mode;
        nvs_get_u8(h, "mode", &mode);
        s.mode = (airmidi_mode_t)mode;
        nvs_get_u16(h, "sysex_to", &s.sysex_timeout_ms);
        const char *why = settings_check(&s);
        if (why) {
            ESP_LOGW(TAG, "stored settings invalid (%s), using defaults", why);
            settings_defaults(&s, mac);
        }
    }
    nvs_close(h);
    current = s;
    ESP_LOGI(TAG, "name %s, mode %s, SysEx timeout %u ms", s.name, airmidi_mode_name(s.mode), s.sysex_timeout_ms);
    return e;
}

void nvs_settings_get(settings_t *out) {
    xSemaphoreTake(lock, portMAX_DELAY);
    *out = current;
    xSemaphoreGive(lock);
}

esp_err_t nvs_settings_set(const settings_t *s) {
    const char *why = settings_check(s);
    if (why) {
        ESP_LOGW(TAG, "rejected: %s", why);
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t e = nvs_open(NAMESPACE, NVS_READWRITE, &h);
    if (e) return e;
    e = write_all(h, s);
    nvs_close(h);
    if (!e) {
        xSemaphoreTake(lock, portMAX_DELAY);
        current = *s;
        xSemaphoreGive(lock);
    }
    return e;
}
