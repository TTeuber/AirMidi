#include "espnow_midi.h"

#include <string.h>

#include "airmidi/midi1.h"
#include "bus.h"
#include "esp_log.h"
#include "esp_now.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "net.h"

static const char *TAG = "espnow";

#define BATCH 64  // UMPs collected between bus flushes
#define RX_QUEUE 16

typedef struct {
    uint8_t mac[6];
    uint8_t len;
    uint8_t data[ESPNOW_FRAME_MAX];
} rx_frame_t;

static espnow_link_t link;
static SemaphoreHandle_t lock;  // the ESP-NOW task and the bus task both drive `link`
static QueueHandle_t rxq;       // frames from the Wi-Fi task's receive callback
static ump_t batch[BATCH];
static int n_batch;
static volatile bool running;
static volatile int pending_up = -1;  // applied outside the lock (bus_set_up may wait for the bus task)
static uint32_t tx_failed;
static int last_tx_error;
static volatile uint32_t tx_acked, tx_unacked;  // unicast frames the peer's radio acknowledged, or never did

static const uint8_t broadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

// ---- lib callbacks (called with `lock` held) -----------------------------------------------------------------

static void cb_send(void *c, const uint8_t mac[6], const uint8_t *data, size_t len) {
    // Unicast is acknowledged and retried by the radio; broadcast (hellos) is not, and need not be.
    esp_err_t e = esp_now_send(mac ? mac : broadcast, data, len);
    if (e) tx_failed++, last_tx_error = e;
}

static void cb_deliver(void *c, const ump_t *u) { bus_send(PORT_ESPNOW, u); }

static void cb_peer(void *c, const uint8_t mac[6], bool up, const char *name) {
    ESP_LOGI(TAG, "%s \"%s\" (%02x:%02x:%02x:%02x:%02x:%02x)", up ? "found" : "lost", name, mac[0], mac[1], mac[2],
             mac[3], mac[4], mac[5]);
    if (up) {
        esp_now_peer_info_t p = {.channel = 0, .ifidx = WIFI_IF_STA, .encrypt = false};  // channel 0: the current
        memcpy(p.peer_addr, mac, 6);
        esp_now_add_peer(&p);
    } else {
        esp_now_del_peer(mac);
    }
    pending_up = espnow_link_peer_count(&link) > 0;
}

static void cb_channel(void *c, uint8_t ch) {
    esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);  // refused while joined to an access point: then it is fixed
}

// ---- send results (Wi-Fi task) -------------------------------------------------------------------------------

static void on_sent(const esp_now_send_info_t *info, esp_now_send_status_t status) {
    if (!memcmp(info->des_addr, broadcast, 6)) return;  // broadcast: nobody acknowledges
    status == ESP_NOW_SEND_SUCCESS ? tx_acked++ : tx_unacked++;
}

// ---- receive (Wi-Fi task) --------------------------------------------------------------------------------------

static void on_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
    if (len <= 0 || len > ESPNOW_FRAME_MAX) return;
    rx_frame_t f = {.len = (uint8_t)len};
    memcpy(f.mac, info->src_addr, 6);
    memcpy(f.data, data, (size_t)len);
    xQueueSend(rxq, &f, 0);  // full: dropped, and counted by the sequence numbers
}

// ---- bus output (bus task) -------------------------------------------------------------------------------------

static void espnow_write(void *c, const ump_t *u) {
    if (n_batch < BATCH) batch[n_batch++] = *u;
}

static void espnow_flush(void *c) {
    if (!n_batch) return;
    xSemaphoreTake(lock, portMAX_DELAY);
    espnow_link_send(&link, batch, n_batch, now_ms());
    xSemaphoreGive(lock);
    n_batch = 0;
}

static size_t espnow_room(void *c) { return n_batch < BATCH ? MIDI1_OUT_MAX : 0; }

static const bus_output_ops_t ops = {.write = espnow_write, .flush = espnow_flush, .room = espnow_room};

// ---- task ----------------------------------------------------------------------------------------------------

static void espnow_task(void *arg) {
    uint8_t ap_channel = 0;
    uint32_t last_tick = 0;
    for (;;) {
        static rx_frame_t f;
        bool got = xQueueReceive(rxq, &f, pdMS_TO_TICKS(10));
        uint32_t now = now_ms();
        xSemaphoreTake(lock, portMAX_DELAY);
        if (got) espnow_link_receive(&link, f.mac, f.data, f.len, now);
        if (now - last_tick >= 10) {
            uint8_t ap = net_ap_channel();  // joined: the access point's channel, and no hopping
            if (ap != ap_channel) espnow_link_set_ap_channel(&link, ap, now), ap_channel = ap;
            espnow_link_tick(&link, now);
            last_tick = now;
        }
        xSemaphoreGive(lock);
        int up = pending_up;
        if (up >= 0) pending_up = -1, bus_set_up(PORT_ESPNOW, up);
    }
}

void espnow_midi_start(const char *name, const char *group) {
    if (running) return;
    lock = xSemaphoreCreateMutex();
    rxq = xQueueCreate(RX_QUEUE, sizeof(rx_frame_t));
    esp_err_t e = esp_now_init();
    if (e) {
        ESP_LOGE(TAG, "ESP-NOW: %s", esp_err_to_name(e));
        return;
    }
    esp_now_register_recv_cb(on_recv);
    esp_now_register_send_cb(on_sent);
    esp_now_peer_info_t b = {.channel = 0, .ifidx = WIFI_IF_STA, .encrypt = false};
    memcpy(b.peer_addr, broadcast, 6);
    esp_now_add_peer(&b);
    bus_register_output(PORT_ESPNOW, &ops, NULL);
    espnow_callbacks_t cb = {
        .send = cb_send, .deliver = cb_deliver, .peer_changed = cb_peer, .set_channel = cb_channel};
    xSemaphoreTake(lock, portMAX_DELAY);
    espnow_link_init(&link, group, name, &cb, NULL, now_ms());
    xSemaphoreGive(lock);
    running = true;
    xTaskCreate(espnow_task, "espnow", 4096, NULL, 11, NULL);
    ESP_LOGI(TAG, "ESP-NOW cable, group \"%s\"", group);
}

void espnow_midi_get_info(espnow_midi_info_t *out) {
    memset(out, 0, sizeof *out);
    if (!running) return;
    xSemaphoreTake(lock, portMAX_DELAY);
    out->running = true;
    out->channel = link.channel;
    out->on_ap_channel = link.fixed_channel;
    for (int i = 0; i < ESPNOW_PEERS; i++)
        if (link.peers[i].used) strcpy(out->peers[out->n_peers++], link.peers[i].name);
    out->stats = link.stats;
    out->tx_failed = tx_failed;
    out->last_tx_error = last_tx_error;
    out->tx_acked = tx_acked;
    out->tx_unacked = tx_unacked;
    xSemaphoreGive(lock);
}
