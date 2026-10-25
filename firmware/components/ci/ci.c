#include "ci.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "airmidi/midici.h"
#include "airmidi/midici_initiator.h"
#include "bus.h"
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/message_buffer.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "ci";

#define STORE "/store"
#define CACHE STORE "/ci"  // the last device's answers: key.txt, info.json, controls.json, map-<id>.json
#define PROFILE STORE "/profile.json"
#define SCAN_BUF (16 * 1024)  // largest resource accepted (a control list of ~100 entries)
#define PROFILE_MAX (16 * 1024)
#define MAX_MAPS (MIDICI_MAX_GETS - 3)
#define FIRST_SCAN_MS 2000

// What the device told us (or the cache had). Owned by the CI task; read by the web server under `lock`.
typedef struct {
    char *info, *controls;
    int nmaps;
    char map_id[MAX_MAPS][MIDICI_NAME_MAX];
    char *map[MAX_MAPS];
} device_t;

static SemaphoreHandle_t lock;
static device_t found;
static char *profile;
static ci_status_t status = {.result = "", .source = "", .report = ""};
static bool from_cache;

// bus task -> CI task
static MessageBufferHandle_t inbox;
static uint8_t rx_buf[MIDICI_MAX_SYSEX];
static sysex7_rx_t rx;

// CI task only
static midici_initiator_t in;
static midici_ident_t ident = {.manufacturer = {0x7D, 0, 0}, .family = 0, .model = 1};  // 7D: development
static uint8_t *scan_buf;
static bool force_fetch, store_ok;
static uint32_t scan_started;
static volatile int requested;  // 0 none, 1 scan, 2 scan and read everything
static volatile bool report_requested;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

// ---- the bus port --------------------------------------------------------------------------------------------

// Bus task: collect SysEx from TRS; whole MIDI-CI messages go to the CI task. Never blocks.
static void ci_write(void *ctx, const ump_t *u) {
    if (!sysex7_rx_push(&rx, u) || rx.len < MIDICI_HEADER || rx_buf[0] != 0x7E || rx_buf[2] != 0x0D) return;
    if (xMessageBufferSend(inbox, rx_buf, rx.len, 0) != rx.len) status.dropped++;
}

static const bus_output_ops_t ci_ops = {.write = ci_write};

static void emit(void *ctx, const ump_t *u) {
    // The bus queue drains in well under a millisecond; wait a little rather than cut a message in half.
    for (int i = 0; i < 20 && !bus_send(PORT_CI, u); i++) vTaskDelay(1);
}

static void cb_send(void *ctx, const uint8_t *sx, size_t n) { sysex7_split(0, sx, n, emit, NULL); }

// ---- storage -------------------------------------------------------------------------------------------------

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    struct stat st;
    char *s = NULL;
    if (fstat(fileno(f), &st) == 0 && st.st_size > 0 && st.st_size <= PROFILE_MAX && (s = malloc(st.st_size + 1))) {
        if (fread(s, 1, st.st_size, f) == (size_t)st.st_size)
            s[st.st_size] = 0;
        else
            free(s), s = NULL;
    }
    fclose(f);
    return s;
}

static bool write_file(const char *path, const void *data, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, len, f) == len;
    return fclose(f) == 0 && ok;
}

static void clear_cache_dir(void) {
    DIR *d = opendir(CACHE);
    if (!d) return;
    struct dirent *e;
    char path[300];
    while ((e = readdir(d))) {
        snprintf(path, sizeof path, CACHE "/%s", e->d_name);
        unlink(path);
    }
    closedir(d);
}

static void free_device(device_t *d) {
    free(d->info);
    free(d->controls);
    for (int i = 0; i < d->nmaps; i++) free(d->map[i]);
    memset(d, 0, sizeof *d);
}

// ctrlMapIds become file names: letters, digits and _ only (M2-117 allows a-z, 0-9, _).
static bool safe_id(const char *id) {
    if (!*id) return false;
    for (; *id; id++)
        if (!((*id >= 'a' && *id <= 'z') || (*id >= 'A' && *id <= 'Z') || (*id >= '0' && *id <= '9') || *id == '_'))
            return false;
    return true;
}

static void add_map(device_t *d, const char *id, char *text) {
    if (d->nmaps == MAX_MAPS) {
        free(text);
        return;
    }
    snprintf(d->map_id[d->nmaps], MIDICI_NAME_MAX, "%s", id);
    d->map[d->nmaps++] = text;
}

// Load the cached answers if they belong to this device.
static bool load_cache(const char *key, device_t *d) {
    char *k = read_file(CACHE "/key.txt");
    bool same = k && !strcmp(k, key);
    free(k);
    if (!same) return false;
    d->controls = read_file(CACHE "/controls.json");
    if (!d->controls) return false;
    d->info = read_file(CACHE "/info.json");
    DIR *dir = opendir(CACHE);
    struct dirent *e;
    char path[300], id[MIDICI_NAME_MAX];
    while (dir && (e = readdir(dir))) {
        size_t n = strlen(e->d_name);
        if (strncmp(e->d_name, "map-", 4) || n < 10 || n - 9 >= sizeof id || strcmp(e->d_name + n - 5, ".json"))
            continue;
        memcpy(id, e->d_name + 4, n - 9);
        id[n - 9] = 0;
        snprintf(path, sizeof path, CACHE "/%s", e->d_name);
        char *t = read_file(path);
        if (t) add_map(d, id, t);
    }
    if (dir) closedir(dir);
    return true;
}

// ---- initiator callbacks (CI task) ---------------------------------------------------------------------------

static char *copy_text(const uint8_t *data, size_t len) {
    char *s = malloc(len + 1);
    if (s) memcpy(s, data, len), s[len] = 0;
    return s;
}

static bool cb_device(void *ctx, uint32_t muid, const midici_discovery_t *d) {
    char key[32];
    const midici_ident_t *i = &d->ident;
    snprintf(key, sizeof key, "%02x%02x%02x-%04x-%04x-%02x%02x%02x%02x", i->manufacturer[0], i->manufacturer[1],
             i->manufacturer[2], i->family, i->model, i->version[0], i->version[1], i->version[2], i->version[3]);
    ESP_LOGI(TAG, "device %s (MUID %07lX, max SysEx %lu)", key, (unsigned long)muid, (unsigned long)d->max_sysex);
    device_t cached = {0};
    bool use_cache = !force_fetch && store_ok && load_cache(key, &cached);
    xSemaphoreTake(lock, portMAX_DELAY);
    free_device(&found);
    if (use_cache) found = cached;
    from_cache = use_cache;
    status.device_muid = muid;
    snprintf(status.device, sizeof status.device, "%s", key);
    xSemaphoreGive(lock);
    if (use_cache) {
        ESP_LOGI(TAG, "known device: control list from the cache");
        return false;
    }
    if (store_ok) {
        clear_cache_dir();  // replaced by what comes now
        write_file(CACHE "/key.txt", key, strlen(key));
    }
    return true;
}

static void cb_resource(void *ctx, const char *resource, const char *res_id, int st, const uint8_t *data, size_t len) {
    ESP_LOGI(TAG, "%s%s%s: status %d, %u bytes", resource, *res_id ? " " : "", res_id, st, (unsigned)len);
    if (st != 200 || !strcmp(resource, "ResourceList")) return;
    // Only valid JSON is kept, so the web UI can splice it into its answer as it is.
    cJSON *j = cJSON_ParseWithLength((const char *)data, len);
    bool valid = j != NULL;
    cJSON_Delete(j);
    if (!valid) {
        ESP_LOGW(TAG, "%s is not valid JSON, ignored", resource);
        return;
    }
    char *text = copy_text(data, len);
    if (!text) return;
    char path[96];
    path[0] = 0;
    xSemaphoreTake(lock, portMAX_DELAY);
    if (!strcmp(resource, "DeviceInfo")) {
        free(found.info), found.info = text;
        snprintf(path, sizeof path, CACHE "/info.json");
    } else if (!strcmp(resource, "AllCtrlList")) {
        free(found.controls), found.controls = text;
        snprintf(path, sizeof path, CACHE "/controls.json");
    } else if (!strcmp(resource, "CtrlMapList") && safe_id(res_id)) {
        add_map(&found, res_id, text);
        snprintf(path, sizeof path, CACHE "/map-%s.json", res_id);
    } else {
        free(text);
    }
    xSemaphoreGive(lock);
    if (store_ok && path[0]) write_file(path, data, len);
}

static void cb_done(void *ctx, midici_done_t r) {
    static const char *names[] = {"ok", "cached", "no device", "no property exchange", "timeout", "invalidated"};
    xSemaphoreTake(lock, portMAX_DELAY);
    status.result = names[r];
    status.scan_ms = now_ms() - scan_started;
    if (r == MIDICI_DONE_NO_DEVICE || r == MIDICI_DONE_NO_PE) {
        // nothing (or nothing with a control list) on the jacks now: the profile takes over
        free_device(&found);
        status.device[0] = 0;
        status.device_muid = 0;
    }
    xSemaphoreGive(lock);
    if (r == MIDICI_DONE_INVALIDATED) {
        status.muid = midici_muid(esp_random());  // M2-101 5.9: someone uses our MUID, or the device left
        midici_initiator_set_muid(&in, status.muid);
    }
    ESP_LOGI(TAG, "scan: %s in %lu ms", names[r], (unsigned long)status.scan_ms);
}

static void cb_report(void *ctx, midici_report_done_t r, uint8_t types) {
    static const char *names[] = {"ok", "refused", "timeout"};
    status.report = names[r];
    ESP_LOGI(TAG, "MIDI Message Report: %s (types %02x)", names[r], types);
}

// ---- the task ------------------------------------------------------------------------------------------------

static void start_scan(bool force) {
    if (!scan_buf && !(scan_buf = malloc(SCAN_BUF))) {
        status.result = "no memory";
        return;
    }
    midici_initiator_cb_t cb = {.send = cb_send,
                                .on_device = cb_device,
                                .on_resource = cb_resource,
                                .on_done = cb_done,
                                .on_report = cb_report};
    midici_initiator_init(&in, status.muid, &ident, &cb, scan_buf, SCAN_BUF);
    force_fetch = force;
    scan_started = now_ms();
    ESP_LOGI(TAG, "scan%s, our MUID %07lX", force ? " (read everything)" : "", (unsigned long)status.muid);
    midici_initiator_start(&in, scan_started);
}

static void ci_task(void *arg) {
    static uint8_t msg[MIDICI_MAX_SYSEX];
    esp_task_wdt_add(NULL);
    uint32_t first_scan = now_ms() + FIRST_SCAN_MS;
    for (;;) {
        size_t n = xMessageBufferReceive(inbox, msg, sizeof msg, pdMS_TO_TICKS(50));
        uint32_t now = now_ms();
        if (n) midici_initiator_receive(&in, msg, n, now);
        midici_initiator_tick(&in, now);
        if (first_scan && (int32_t)(now - first_scan) >= 0) first_scan = 0, requested = requested ? requested : 1;
        if (requested && !midici_initiator_busy(&in)) {
            int r = requested;
            requested = 0;
            start_scan(r == 2);
        }
        if (report_requested && !midici_initiator_busy(&in)) {  // after a scan asked for at the same time
            report_requested = false;
            uint8_t types = MIDICI_REPORT_PITCHBEND | MIDICI_REPORT_CC | MIDICI_REPORT_PROGRAM | MIDICI_REPORT_CHPRESS;
            if (midici_initiator_report(&in, types, now))
                status.report = "asking";
            else
                status.report = in.dev ? "not supported" : "no device";
        }
        status.scanning = midici_initiator_busy(&in);
        if (!status.scanning && scan_buf) free(scan_buf), scan_buf = NULL;  // 16 KB back between scans
        esp_task_wdt_reset();
    }
}

// ---- API -----------------------------------------------------------------------------------------------------

void ci_start(void) {
    lock = xSemaphoreCreateMutex();
    esp_vfs_littlefs_conf_t fs = {.base_path = STORE, .partition_label = "storage", .format_if_mount_failed = true};
    esp_err_t e = esp_vfs_littlefs_register(&fs);
    store_ok = e == ESP_OK;
    if (store_ok) {
        mkdir(CACHE, 0755);
        size_t total = 0, used = 0;
        esp_littlefs_info("storage", &total, &used);
        ESP_LOGI(TAG, "storage: %u of %u KB used", (unsigned)(used / 1024), (unsigned)(total / 1024));
        profile = read_file(PROFILE);
    } else {
        ESP_LOGE(TAG, "storage not mounted (%s): nothing is cached", esp_err_to_name(e));
    }
    int v[3] = {0};
    sscanf(esp_app_get_description()->version, "v%d.%d.%d", &v[0], &v[1], &v[2]);
    for (int i = 0; i < 3; i++) ident.version[i] = v[i] & 0x7F;
    status.muid = midici_muid(esp_random());  // a new one at every power-up (M2-101 3.3)
    sysex7_rx_init(&rx, rx_buf, sizeof rx_buf);
    inbox = xMessageBufferCreate(4096);
    bus_register_output(PORT_CI, &ci_ops, NULL);
    bus_set_up(PORT_CI, true);
    xTaskCreate(ci_task, "ci", 4096, NULL, 6, NULL);
}

void ci_scan(bool force) { requested = force ? 2 : (requested ? requested : 1); }

void ci_report(void) { report_requested = true; }

void ci_get_status(ci_status_t *out) {
    xSemaphoreTake(lock, portMAX_DELAY);
    *out = status;
    out->source = found.controls ? (from_cache ? "cache" : "midi-ci") : profile ? "profile" : "";
    xSemaphoreGive(lock);
}

// Appends to a growing heap string; false (and the string freed) if out of memory.
typedef struct {
    char *s;
    size_t len, cap;
} text_t;

static bool put(text_t *t, const char *s) {
    size_t n = strlen(s);
    if (t->len + n + 1 > t->cap) {
        size_t cap = (t->len + n + 1) * 3 / 2;
        char *p = realloc(t->s, cap);
        if (!p) return false;
        t->s = p, t->cap = cap;
    }
    memcpy(t->s + t->len, s, n + 1);
    t->len += n;
    return true;
}

bool ci_write_device(bool (*sink)(void *ctx, const char *text, size_t len), void *ctx) {
    // Copied under the lock, sent without it: a slow page must not hold up the CI task.
    text_t t = {0};
    bool ok = true;
    xSemaphoreTake(lock, portMAX_DELAY);
    ok &= put(&t, "{\"info\":") && put(&t, found.info ? found.info : "null");
    ok &= put(&t, ",\"controls\":") && put(&t, found.controls ? found.controls : "null");
    ok &= put(&t, ",\"maps\":{");
    for (int i = 0; ok && i < found.nmaps; i++)
        ok &= put(&t, i ? ",\"" : "\"") && put(&t, found.map_id[i]) && put(&t, "\":") && put(&t, found.map[i]);
    ok &= put(&t, "},\"profile\":") && put(&t, profile ? profile : "null") && put(&t, "}");
    xSemaphoreGive(lock);
    ok = ok && sink(ctx, t.s, t.len);
    free(t.s);
    return ok;
}

esp_err_t ci_set_profile(const char *json, size_t len) {
    char *text = NULL;
    if (json && len) {
        if (len > PROFILE_MAX) return ESP_ERR_INVALID_SIZE;
        cJSON *j = cJSON_ParseWithLength(json, len);
        bool shape = cJSON_IsArray(j) || cJSON_IsArray(cJSON_GetObjectItem(j, "controls"));
        cJSON_Delete(j);
        if (!shape) return ESP_ERR_INVALID_ARG;
        if (!(text = copy_text((const uint8_t *)json, len))) return ESP_ERR_NO_MEM;
        if (store_ok && !write_file(PROFILE, json, len)) {
            free(text);
            return ESP_FAIL;
        }
    } else if (store_ok) {
        unlink(PROFILE);
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    free(profile);
    profile = text;
    xSemaphoreGive(lock);
    return ESP_OK;
}
