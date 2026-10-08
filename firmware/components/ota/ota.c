#include "ota.h"

#include <stdio.h>
#include <string.h>

#include "esp_core_dump.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"

static const char *TAG = "ota";

static bool (*check)(void);
static esp_ota_handle_t update;
static const esp_partition_t *update_part;

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

bool ota_coredump_read(bool (*sink)(void *ctx, const void *data, size_t len), void *ctx) {
    size_t addr = 0, size = 0;
    if (esp_core_dump_image_get(&addr, &size) != ESP_OK) return false;
    static uint8_t buf[1024];
    for (size_t off = 0; off < size; off += sizeof buf) {
        size_t n = size - off < sizeof buf ? size - off : sizeof buf;
        if (esp_flash_read(NULL, buf, addr + off, n) != ESP_OK || !sink(ctx, buf, n)) return false;
    }
    return true;
}

void ota_get_info(ota_info_t *out) {
    memset(out, 0, sizeof *out);
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
    esp_ota_get_state_partition(run, &st);
    snprintf(out->running, sizeof out->running, "%s", run->label);
    out->state = state_name(st);
    const esp_partition_t *bad = esp_ota_get_last_invalid_partition();
    if (bad) snprintf(out->rolled_back, sizeof out->rolled_back, "%s", bad->label);
}

const char *ota_reset_reason(void) {
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON:
            return "power on";
        case ESP_RST_EXT:
            return "external pin";
        case ESP_RST_SW:
            return "software restart";
        case ESP_RST_PANIC:
            return "panic";
        case ESP_RST_INT_WDT:
            return "interrupt watchdog";
        case ESP_RST_TASK_WDT:
            return "task watchdog";
        case ESP_RST_WDT:
            return "other watchdog";
        case ESP_RST_DEEPSLEEP:
            return "deep sleep wake";
        case ESP_RST_BROWNOUT:
            return "brownout";
        case ESP_RST_USB:
            return "USB";
        case ESP_RST_JTAG:
            return "JTAG";
        default:
            return "unknown";
    }
}

esp_err_t ota_begin(void) {
    update_part = esp_ota_get_next_update_partition(NULL);
    if (!update_part) return ESP_ERR_NOT_FOUND;
    ESP_LOGI(TAG, "receiving an update into %s", update_part->label);
    // Erase as the image arrives instead of the whole slot up front (1.75 MB erase would stall the upload).
    return esp_ota_begin(update_part, OTA_WITH_SEQUENTIAL_WRITES, &update);
}

esp_err_t ota_write(const void *data, size_t len) { return esp_ota_write(update, data, len); }

esp_err_t ota_end(void) {
    esp_err_t e = esp_ota_end(update);  // validates the image: format, SHA-256, and the signature if required
    update = 0;
    if (e) {
        ESP_LOGE(TAG, "update rejected: %s", esp_err_to_name(e));
        return e;
    }
    e = esp_ota_set_boot_partition(update_part);
    if (!e) ESP_LOGW(TAG, "update accepted: %s boots next, pending its self-test", update_part->label);
    return e;
}

void ota_abort(void) {
    if (update) esp_ota_abort(update);
    update = 0;
}
