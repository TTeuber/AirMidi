#include "ble_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "airmidi/blecfg.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "nvs.h"

static const char *TAG = "blecfg";

#define RX_CAP 8192       // a request with its body; the buffer exists only while a phone uses the service
#define NOTIFY_TRIES 200  // x 5 ms: how long a reply piece waits for NimBLE to have a buffer free

// 9A5C0010-3F2B-4D8E-A4B1-6C2E8F1D0A00 and ...0A11, ...0A12 (NimBLE takes them little-endian), next to
// ble_update's 9A5C0000 family.
#define UUID(b0, b1) \
    BLE_UUID128_INIT(b0, b1, 0x1D, 0x8F, 0x2E, 0x6C, 0xB1, 0xA4, 0x8E, 0x4D, 0x2B, 0x3F, 0x10, 0x00, 0x5C, 0x9A)
static const ble_uuid128_t svc_uuid = UUID(0x00, 0x0A), req_uuid = UUID(0x11, 0x0A), resp_uuid = UUID(0x12, 0x0A);

static uint16_t resp_handle;
static ble_config_handler_t handler;
static QueueHandle_t requests;

// A complete request, from the host task to the worker. status != 0: answer that error without asking the API.
typedef struct {
    uint16_t conn;
    uint8_t id;
    int status;
    size_t len;
    uint8_t data[];
} request_t;

// ---- trusted phones and the pairing window -------------------------------------------------------------------

static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static ble_addr_t phones[BLE_CONFIG_PHONES];
static int n_phones;
static int64_t window_until;  // esp_timer time in us; INT64_MAX = until closed, 0 = closed
static bool phones_loaded;

static void load_phones(void) {
    nvs_handle_t h;
    size_t len = sizeof phones;
    if (nvs_open("blecfg", NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_blob(h, "phones", phones, &len) == ESP_OK) n_phones = len / sizeof phones[0];
        nvs_close(h);
    }
    phones_loaded = true;
}

static void save_phones(void) {
    nvs_handle_t h;
    if (nvs_open("blecfg", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "phones", phones, n_phones * sizeof phones[0]);
    nvs_commit(h);
    nvs_close(h);
}

static bool window_open(void) { return window_until && esp_timer_get_time() < window_until; }

// Host task. True if this phone may use the service: trusted already, or trusted now because the window is open.
static bool allow(const ble_addr_t *id) {
    if (!phones_loaded) load_phones();
    taskENTER_CRITICAL(&lock);
    for (int i = 0; i < n_phones; i++)
        if (!ble_addr_cmp(&phones[i], id)) {
            taskEXIT_CRITICAL(&lock);
            return true;
        }
    bool open = window_open();
    if (open) {  // newest first; the oldest of four falls off
        memmove(phones + 1, phones, (BLE_CONFIG_PHONES - 1) * sizeof phones[0]);
        phones[0] = *id;
        if (n_phones < BLE_CONFIG_PHONES) n_phones++;
    }
    taskEXIT_CRITICAL(&lock);
    if (open) {
        save_phones();
        ESP_LOGI(TAG, "phone %02x:%02x:%02x:%02x:%02x:%02x trusted (%d)", id->val[5], id->val[4], id->val[3],
                 id->val[2], id->val[1], id->val[0], n_phones);
    }
    return open;
}

void ble_config_set_window(uint32_t seconds) {
    taskENTER_CRITICAL(&lock);
    window_until = !seconds                               ? 0
                   : seconds == BLE_CONFIG_WINDOW_FOREVER ? INT64_MAX
                                                          : esp_timer_get_time() + (int64_t)seconds * 1000000;
    taskEXIT_CRITICAL(&lock);
    if (seconds) ESP_LOGI(TAG, "pairing window open%s", seconds == BLE_CONFIG_WINDOW_FOREVER ? "" : " for a while");
}

uint32_t ble_config_window_left(void) {
    if (!window_open()) return 0;
    if (window_until == INT64_MAX) return BLE_CONFIG_WINDOW_FOREVER;
    return (uint32_t)((window_until - esp_timer_get_time()) / 1000000) + 1;
}

int ble_config_phone_count(void) {
    if (!phones_loaded) load_phones();
    return n_phones;
}

void ble_config_forget_phones(void) {
    taskENTER_CRITICAL(&lock);
    n_phones = 0;
    taskEXIT_CRITICAL(&lock);
    phones_loaded = true;
    save_phones();
    ESP_LOGI(TAG, "trusted phones forgotten");
}

// ---- requests in (host task) ---------------------------------------------------------------------------------

static uint8_t *rx_buf;
static blecfg_rx_t rx;
static uint16_t rx_conn = BLE_HS_CONN_HANDLE_NONE;

static void queue_request(uint16_t conn, uint8_t id, int status, const uint8_t *data, size_t len) {
    request_t *r = malloc(sizeof *r + len);
    if (!r) return;
    r->conn = conn, r->id = id, r->status = status, r->len = len;
    if (len) memcpy(r->data, data, len);
    if (xQueueSend(requests, &r, 0) != pdTRUE) {  // a phone sending before its last answer came: refuse quietly
        free(r);
        ESP_LOGW(TAG, "request %u dropped: the last one is still being answered", id);
    }
}

static int req_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg) {
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;
    struct ble_gap_conn_desc d;
    if (ble_gap_conn_find(conn, &d)) return BLE_ATT_ERR_UNLIKELY;
    if (!d.sec_state.encrypted) return BLE_ATT_ERR_INSUFFICIENT_ENC;  // the phone pairs, then writes again
    if (!allow(&d.peer_id_addr)) return BLE_ATT_ERR_INSUFFICIENT_AUTHOR;
    if (!requests) return BLE_ATT_ERR_UNLIKELY;  // not started
    static uint8_t piece[BLE_ATT_ATTR_MAX_LEN];
    uint16_t n = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, piece, sizeof piece, &n)) return BLE_ATT_ERR_UNLIKELY;
    if (!rx_buf && !(rx_buf = malloc(RX_CAP))) return BLE_ATT_ERR_INSUFFICIENT_RES;
    if (conn != rx_conn) {  // another phone took over: start afresh
        blecfg_rx_init(&rx, rx_buf, RX_CAP);
        rx_conn = conn;
    }
    switch (blecfg_rx_feed(&rx, piece, n)) {
        case BLECFG_DONE:
            queue_request(conn, rx.id, 0, rx.buf, rx.len);
            break;
        case BLECFG_TOO_LONG:
            queue_request(conn, rx.id, 413, NULL, 0);
            break;
        case BLECFG_BAD:
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        case BLECFG_MORE:
            break;
    }
    return 0;
}

static int resp_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg) {
    return BLE_ATT_ERR_READ_NOT_PERMITTED;  // notifications only
}

const struct ble_gatt_svc_def ble_config_services[] = {
    {.type = BLE_GATT_SVC_TYPE_PRIMARY,
     .uuid = &svc_uuid.u,
     .characteristics =
         (struct ble_gatt_chr_def[]){
             {.uuid = &req_uuid.u, .access_cb = req_access, .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC},
             {.uuid = &resp_uuid.u,
              .access_cb = resp_access,
              .val_handle = &resp_handle,
              .flags = BLE_GATT_CHR_F_NOTIFY},
             {0}}},
    {0}};

void ble_config_disconnected(uint16_t conn) {
    if (conn != rx_conn) return;
    rx_conn = BLE_HS_CONN_HANDLE_NONE;
    free(rx_buf);
    rx_buf = NULL;
}

// ---- replies out (worker task) -------------------------------------------------------------------------------

static bool notify_piece(uint16_t conn, const uint8_t *p, size_t n) {
    for (int t = 0; t < NOTIFY_TRIES; t++) {
        struct os_mbuf *om = ble_hs_mbuf_from_flat(p, n);
        if (om) {
            int rc = ble_gatts_notify_custom(conn, resp_handle, om);  // takes om either way
            if (rc == 0) return true;
            if (rc != BLE_HS_ENOMEM) return false;  // gone, or not subscribed
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return false;
}

static void send_reply(uint16_t conn, uint8_t id, int status, const char *json) {
    size_t jl = json ? strlen(json) : 0;
    char head[8];
    int hl = snprintf(head, sizeof head, "%d\n", status);
    uint8_t *msg = malloc(hl + jl);
    if (!msg) return;
    memcpy(msg, head, hl);
    if (jl) memcpy(msg + hl, json, jl);
    size_t len = hl + jl, off = 0, max = ble_att_mtu(conn) - 3;
    if (max < BLECFG_HEADER + 1 || max > BLE_ATT_ATTR_MAX_LEN) max = 20;
    uint8_t piece[BLE_ATT_ATTR_MAX_LEN];
    do {
        size_t n = blecfg_piece(id, msg, len, &off, max, piece);
        if (!notify_piece(conn, piece, n)) {
            ESP_LOGW(TAG, "reply %u not sent (connection gone or not subscribed)", id);
            break;
        }
    } while (off < len);
    free(msg);
}

static void worker(void *arg) {
    for (;;) {
        request_t *r;
        xQueueReceive(requests, &r, portMAX_DELAY);
        char method[8], path[160];
        const uint8_t *body;
        size_t body_len;
        if (r->status) {
            send_reply(r->conn, r->id, r->status, "{\"error\":\"request too long\"}");
        } else if (!blecfg_parse_request(r->data, r->len, method, sizeof method, path, sizeof path, &body, &body_len)) {
            send_reply(r->conn, r->id, 400, "{\"error\":\"expected METHOD PATH, a newline, then the body\"}");
        } else {
            ble_config_reply_t rep = handler(method, path, (const char *)body, body_len);
            send_reply(r->conn, r->id, rep.json ? rep.status : 500,
                       rep.json ? rep.json : "{\"error\":\"out of memory\"}");
            free(rep.json);
            if (rep.then) {
                vTaskDelay(pdMS_TO_TICKS(150));  // the last notification on its way before BLE may stop
                rep.then(rep.then_arg);
            }
        }
        free(r);
    }
}

void ble_config_start(ble_config_handler_t h) {
    handler = h;
    requests = xQueueCreate(2, sizeof(request_t *));
    xTaskCreate(worker, "blecfg", 6144, NULL, 4, NULL);
}
