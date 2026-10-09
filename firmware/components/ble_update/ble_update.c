#include "ble_update.h"

#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "ota.h"

static const char *TAG = "bleota";

#define BUFFER (BLE_UPDATE_WINDOW + 4096)  // the window, plus what may be in flight when progress goes out
#define PROGRESS_EVERY 4096

enum { CMD_BEGIN = 1, CMD_END = 2, CMD_ABORT = 3 };
enum { ST_READY = 0, ST_PROGRESS = 1, ST_ACCEPTED = 2, ST_FAILED = 3, ST_RESEND = 4 };

// 9A5C0000-3F2B-4D8E-A4B1-6C2E8F1D0A00 and its characteristics ...0A01-03 (NimBLE takes them little-endian).
#define UUID(last) \
    BLE_UUID128_INIT(last, 0x0A, 0x1D, 0x8F, 0x2E, 0x6C, 0xB1, 0xA4, 0x8E, 0x4D, 0x2B, 0x3F, 0x00, 0x00, 0x5C, 0x9A)
static const ble_uuid128_t svc_uuid = UUID(0x00), ctrl_uuid = UUID(0x01), data_uuid = UUID(0x02),
                           version_uuid = UUID(0x03);

static uint16_t ctrl_handle;
static StreamBufferHandle_t stream;  // data from the NimBLE host task to the writer task
static TaskHandle_t writer;
static volatile bool active, end_requested, failed;
static volatile uint16_t conn = BLE_HS_CONN_HANDLE_NONE;
static uint32_t size, received;      // received: host task; written: writer task
static uint32_t resend_at, resends;  // the gap last asked about (one request per gap), how many gaps
static volatile uint32_t written;

static void notify(uint8_t status, const void *extra, size_t len) {
    uint8_t b[64] = {status};
    if (len > sizeof b - 1) len = sizeof b - 1;
    memcpy(b + 1, extra, len);
    struct os_mbuf *om = ble_hs_mbuf_from_flat(b, 1 + len);
    if (om && conn != BLE_HS_CONN_HANDLE_NONE) ble_gatts_notify_custom(conn, ctrl_handle, om);
}

static void fail(const char *why) {
    ESP_LOGW(TAG, "upload failed: %s", why);
    failed = true;
    notify(ST_FAILED, why, strlen(why));
}

// Flash writes happen here, so the host task never waits for them.
static void writer_task(void *arg) {
    static uint8_t buf[4096];
    uint32_t next_progress = PROGRESS_EVERY;
    for (;;) {
        size_t n = xStreamBufferReceive(stream, buf, sizeof buf, pdMS_TO_TICKS(100));
        if (!active) break;  // aborted (or the connection went)
        if (n && !failed) {
            esp_err_t e = ota_write(buf, n);
            if (e) fail(esp_err_to_name(e));
            written += n;
            if (written >= next_progress || written == size) {
                notify(ST_PROGRESS, (const void *)&written, 4);  // little-endian, like the chip
                next_progress = written + PROGRESS_EVERY;
            }
        }
        if (failed) break;
        if (end_requested && written == size) {
            esp_err_t e = ota_end();
            if (e == ESP_ERR_OTA_VALIDATE_FAILED) {
                fail("not a valid image for this board, or not signed with our key");
            } else if (e) {
                fail(esp_err_to_name(e));
            } else {
                ESP_LOGW(TAG, "update accepted (%lu bytes, %lu gaps resent): restarting", (unsigned long)size,
                         (unsigned long)resends);
                notify(ST_ACCEPTED, NULL, 0);
                vTaskDelay(pdMS_TO_TICKS(500));  // let the notification go out
                esp_restart();
            }
            break;
        }
    }
    if (failed || !active) ota_abort();
    active = false;  // the stream buffer stays: the host task may still be looking at it (reset at the next begin)
    writer = NULL;
    vTaskDelete(NULL);
}

static void begin(uint16_t c, uint32_t n) {
    if (active) {
        fail("an upload is already in progress");
        return;
    }
    conn = c;
    esp_err_t e = ota_begin();
    if (e) {
        fail(esp_err_to_name(e));
        return;
    }
    if (!stream) stream = xStreamBufferCreate(BUFFER, 1);  // 20 KB, only once an update is attempted
    if (!stream || writer) {
        ota_abort();
        fail("out of memory");
        return;
    }
    xStreamBufferReset(stream);
    size = n, received = written = resends = 0;
    resend_at = UINT32_MAX;
    end_requested = failed = false;
    active = true;
    // 8 KB: ota_end() checks the RSA-3072 signature (a 3 KB stack overflowed in SHA-512, found on the rig).
    xTaskCreate(writer_task, "bleota", 8192, NULL, 5, &writer);
    ESP_LOGI(TAG, "receiving %lu bytes", (unsigned long)n);
    notify(ST_READY, NULL, 0);
}

static int ctrl_access(uint16_t c, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg) {
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;
    uint8_t b[8];
    uint16_t len = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, b, sizeof b, &len) || len < 1) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    switch (b[0]) {
        case CMD_BEGIN:
            if (len < 5) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            begin(c, (uint32_t)b[1] | (uint32_t)b[2] << 8 | (uint32_t)b[3] << 16 | (uint32_t)b[4] << 24);
            break;
        case CMD_END:
            if (!active) return BLE_ATT_ERR_UNLIKELY;
            if (received != size) {
                fail("the image is shorter than announced");
                active = false;
            }
            end_requested = true;
            break;
        case CMD_ABORT:
            active = false;  // the writer task cleans up
            break;
        default:
            return BLE_ATT_ERR_REQ_NOT_SUPPORTED;
    }
    return 0;
}

static int data_access(uint16_t c, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg) {
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR || !active || failed) return 0;
    static uint8_t piece[BLE_ATT_ATTR_MAX_LEN];  // host task only
    uint16_t len = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, piece, sizeof piece, &len) || len < 4) return 0;
    uint32_t offset =
        (uint32_t)piece[0] | (uint32_t)piece[1] << 8 | (uint32_t)piece[2] << 16 | (uint32_t)piece[3] << 24;
    len -= 4;
    if (offset != received) {  // a piece went missing before this one: ask once for everything from the gap on
        if (offset > received && resend_at != received) {
            resend_at = received;
            resends++;
            notify(ST_RESEND, &received, 4);
        }
        return 0;
    }
    if (received + len > size) {
        fail("more data than announced");
        return 0;
    }
    if (xStreamBufferSend(stream, piece + 4, len, 0) != len) {
        fail("the client sent beyond the window");
        return 0;
    }
    received += len;
    resend_at = UINT32_MAX;
    return 0;
}

static int version_access(uint16_t c, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg) {
    const char *v = esp_app_get_description()->version;
    return os_mbuf_append(ctxt->om, v, strlen(v)) ? BLE_ATT_ERR_INSUFFICIENT_RES : 0;
}

const struct ble_gatt_svc_def ble_update_services[] = {
    {.type = BLE_GATT_SVC_TYPE_PRIMARY,
     .uuid = &svc_uuid.u,
     .characteristics =
         (struct ble_gatt_chr_def[]){
             {.uuid = &ctrl_uuid.u,
              .access_cb = ctrl_access,
              .val_handle = &ctrl_handle,
              .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_NOTIFY},
             {.uuid = &data_uuid.u, .access_cb = data_access, .flags = BLE_GATT_CHR_F_WRITE_NO_RSP},
             {.uuid = &version_uuid.u, .access_cb = version_access, .flags = BLE_GATT_CHR_F_READ},
             {0}}},
    {0}};

bool ble_update_active(void) { return active; }

void ble_update_disconnected(uint16_t c) {
    if (active && c == conn) {
        ESP_LOGW(TAG, "connection lost: upload abandoned");
        active = false;
    }
    if (c == conn) conn = BLE_HS_CONN_HANDLE_NONE;
}
