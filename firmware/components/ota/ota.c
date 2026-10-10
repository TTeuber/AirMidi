#include "ota.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_core_dump.h"
#include "esp_crt_bundle.h"
#include "esp_flash.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ota";

static bool (*check)(void);
static esp_ota_handle_t update;
static const esp_partition_t *update_part;
static size_t update_len;

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

void ota_coredump_erase(void) { esp_core_dump_image_erase(); }

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
    update_len = 0;
    // Erase as the image arrives instead of the whole slot up front (1.75 MB erase would stall the upload).
    return esp_ota_begin(update_part, OTA_WITH_SEQUENTIAL_WRITES, &update);
}

esp_err_t ota_write(const void *data, size_t len) {
    update_len += len;
    return esp_ota_write(update, data, len);
}

esp_err_t ota_end(void) {
    // Sequential writes erase only what they write. The signature block is looked for in the 4 KB sector after the
    // image's data, so an unsigned upload would find whatever an earlier upload left there; found on the rig, when
    // an unsigned copy of an image uploaded just before verified against that image's leftover signature. Erase
    // that sector, so only a signature that came with this upload can count.
    size_t sig = (update_len + 0xFFF) & ~(size_t)0xFFF;
    if (sig + 0x1000 <= update_part->size) esp_partition_erase_range(update_part, sig, 0x1000);
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

// ---- fetching a release ----------------------------------------------------------------------------------------

// The image a box takes from a release's manifest: per board, not per chip (the S3 dev kit has other pins than the
// XIAO S3). Same names as build.sh's.
#if CONFIG_AIRMIDI_BOARD_XIAO_C6
#define BOARD_KEY "c6"
#elif CONFIG_AIRMIDI_BOARD_XIAO_S3
#define BOARD_KEY "s3"
#elif CONFIG_AIRMIDI_BOARD_DEVKITC_S3
#define BOARD_KEY "s3kit"
#endif

static char fetch_base[160];
static volatile int fetch_percent;
static const char *volatile fetch_state = "idle";
static char fetch_error[64];

static esp_http_client_handle_t open_url(const char *url) {
    esp_http_client_config_t c = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,  // HTTPS against the usual public CAs (GitHub's included)
        .max_redirection_count = 5,                  // github.com/.../latest/download -> the release asset
        .buffer_size = 4096,
        .buffer_size_tx = 2048,  // the redirect target is a long signed URL
        .timeout_ms = 15000,
    };
    esp_http_client_handle_t h = esp_http_client_init(&c);
    if (!h) return NULL;
    if (esp_http_client_open(h, 0) != ESP_OK) {
        esp_http_client_cleanup(h);
        return NULL;
    }
    // follow redirects by hand: esp_http_client_fetch_headers + set_redirection for a streamed GET
    for (int i = 0; i < 5; i++) {
        esp_http_client_fetch_headers(h);
        int code = esp_http_client_get_status_code(h);
        if (code < 300 || code >= 400) break;
        esp_http_client_set_redirection(h);
        esp_http_client_close(h);
        if (esp_http_client_open(h, 0) != ESP_OK) {
            esp_http_client_cleanup(h);
            return NULL;
        }
    }
    return h;
}

static void base_or_default(const char *base, char *out, size_t len) {
    snprintf(out, len, "%s", base && *base ? base : CONFIG_AIRMIDI_UPDATE_BASE_URL);
}

esp_err_t ota_check(const char *base, ota_manifest_t *out, char *err, size_t err_len) {
    memset(out, 0, sizeof *out);
    char b[160], url[200];
    base_or_default(base, b, sizeof b);
    snprintf(url, sizeof url, "%s/manifest.json", b);
    esp_http_client_handle_t h = open_url(url);
    if (!h) return snprintf(err, err_len, "cannot reach %s", url), ESP_FAIL;
    char body[1024];
    int n = 0, got;
    while (n < (int)sizeof body - 1 && (got = esp_http_client_read(h, body + n, sizeof body - 1 - n)) > 0) n += got;
    body[n] = 0;
    int code = esp_http_client_get_status_code(h);
    esp_http_client_cleanup(h);
    if (code != 200) return snprintf(err, err_len, "manifest: HTTP %d", code), ESP_FAIL;
    cJSON *m = cJSON_Parse(body);
    const char *ver = cJSON_GetStringValue(cJSON_GetObjectItem(m, "version"));
    const char *file = cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetObjectItem(m, "images"), BOARD_KEY));
    esp_err_t e = ESP_OK;
    if (!ver || !file) {
        snprintf(err, err_len, "manifest has no image for this board");
        e = ESP_FAIL;
    } else {
        snprintf(out->version, sizeof out->version, "%s", ver);
        snprintf(out->file, sizeof out->file, "%s", file);
        out->newer = strcmp(ver, esp_app_get_description()->version) != 0;
    }
    cJSON_Delete(m);
    return e;
}

static void fail(const char *why) {
    snprintf(fetch_error, sizeof fetch_error, "%s", why);
    fetch_state = "failed";
    ESP_LOGE(TAG, "update download failed: %s", why);
}

static void fetch_task(void *arg) {
    ota_manifest_t m;
    char err[64], url[240];
    if (ota_check(fetch_base, &m, err, sizeof err) != ESP_OK) {
        fail(err);
        vTaskDelete(NULL);
    }
    snprintf(url, sizeof url, "%s/%s", fetch_base, m.file);
    ESP_LOGI(TAG, "downloading %s (%s)", m.version, url);
    esp_http_client_handle_t h = open_url(url);
    int64_t total = h ? esp_http_client_get_content_length(h) : 0;
    if (!h || esp_http_client_get_status_code(h) != 200 || total <= 0) {
        if (h) esp_http_client_cleanup(h);
        fail("image not found");
        vTaskDelete(NULL);
    }
    if (ota_begin() != ESP_OK) {
        esp_http_client_cleanup(h);
        fail("no slot to write to");
        vTaskDelete(NULL);
    }
    static char buf[4096];
    int64_t done = 0;
    int n;
    while ((n = esp_http_client_read(h, buf, sizeof buf)) > 0) {
        if (ota_write(buf, n) != ESP_OK) break;
        done += n;
        fetch_percent = (int)(100 * done / total);
    }
    esp_http_client_cleanup(h);
    if (done != total) {
        ota_abort();
        fail("download interrupted");
        vTaskDelete(NULL);
    }
    if (ota_end() != ESP_OK) {
        fail("image did not verify");
        vTaskDelete(NULL);
    }
    fetch_state = "installed";
    vTaskDelay(pdMS_TO_TICKS(1500));  // let the web UI see it
    esp_restart();
}

esp_err_t ota_install_start(const char *base) {
    if (!strcmp(fetch_state, "downloading")) return ESP_ERR_INVALID_STATE;
    base_or_default(base, fetch_base, sizeof fetch_base);
    fetch_state = "downloading";
    fetch_percent = 0;
    fetch_error[0] = 0;
    if (xTaskCreate(fetch_task, "ota_fetch", 8192, NULL, 4, NULL) != pdPASS)
        return fetch_state = "idle", ESP_ERR_NO_MEM;
    return ESP_OK;
}

void ota_fetch_status(ota_fetch_status_t *out) {
    out->state = fetch_state;
    out->percent = fetch_percent;
    snprintf(out->error, sizeof out->error, "%s", fetch_error);
}
