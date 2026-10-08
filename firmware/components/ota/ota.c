#include "ota.h"

#include <stdio.h>
#include <string.h>

#include "esp_core_dump.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"

static const char *TAG = "ota";

static bool (*check)(void);

static const char *state_name(esp_ota_img_states_t s) {
    switch (s) {
        case ESP_OTA_IMG_NEW:
            return "new";
        case ESP_OTA_IMG_PENDING_VERIFY:
            return "pending verify";
        case ESP_OTA_IMG_VALID:
            return "valid";
        case ESP_OTA_IMG_INVALID:
            return "invalid";
        case ESP_OTA_IMG_ABORTED:
            return "aborted";
        default:
            return "undefined (flashed over USB)";
    }
}

bool ota_pending_verify(void) {
    esp_ota_img_states_t st;
    return esp_ota_get_state_partition(esp_ota_get_running_partition(), &st) == ESP_OK &&
           st == ESP_OTA_IMG_PENDING_VERIFY;
}

void ota_coredump_info(ota_coredump_t *out) {
    memset(out, 0, sizeof *out);
    size_t addr = 0, size = 0;
    if (esp_core_dump_image_get(&addr, &size) != ESP_OK) return;
    out->present = true;
    out->size = size;
    esp_core_dump_summary_t sum;
    if (esp_core_dump_get_summary(&sum) == ESP_OK) {
        snprintf(out->task, sizeof out->task, "%s", sum.exc_task);
        out->pc = sum.exc_pc;
    }
}

void ota_boot_report(void) {
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
    esp_ota_get_state_partition(run, &st);
    ESP_LOGI(TAG, "running from %s at 0x%lx, state %s", run->label, (unsigned long)run->address, state_name(st));
    const esp_partition_t *last = esp_ota_get_last_invalid_partition();
    if (last) ESP_LOGW(TAG, "%s holds an image that failed and was rolled back", last->label);
    ota_coredump_t cd;
    ota_coredump_info(&cd);
    if (cd.present)
        ESP_LOGW(TAG, "core dump from an earlier crash: %lu bytes, task \"%s\", pc 0x%08lx", (unsigned long)cd.size,
                 cd.task, (unsigned long)cd.pc);
}

static void decide(void *arg) {
    if (check && check()) {
        ESP_LOGI(TAG, "self-test passed: image confirmed");
        esp_ota_mark_app_valid_cancel_rollback();
    } else {
        ESP_LOGE(TAG, "self-test failed: rolling back to the previous image");
        esp_ota_mark_app_invalid_rollback_and_reboot();
    }
}

void ota_confirm_after(uint32_t ms, bool (*healthy)(void)) {
    if (!ota_pending_verify()) return;
    check = healthy;
    ESP_LOGW(TAG, "new image: confirming in %lu ms if the self-test passes", (unsigned long)ms);
    const esp_timer_create_args_t args = {.callback = decide, .name = "ota_confirm"};
    esp_timer_handle_t t;
    ESP_ERROR_CHECK(esp_timer_create(&args, &t));
    ESP_ERROR_CHECK(esp_timer_start_once(t, (uint64_t)ms * 1000));
}
