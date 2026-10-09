#include "rtp_midi.h"

#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>

#include "bus.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "mdns.h"
#include "net.h"
#include "nvs.h"

static const char *TAG = "rtpmidi";

#define BATCH 64  // UMPs collected per session between bus flushes (one packet)

typedef struct {
    ump_t batch[BATCH];
    int n_batch;
} session_ctx_t;

static rtpmidi_t host;
static session_ctx_t ctx[RTPMIDI_SESSIONS];
static SemaphoreHandle_t lock;   // the UDP task and the bus task both drive `host`
static int socks[2] = {-1, -1};  // control, data

// The last client's address and name, kept in NVS: a session taken back after a restart (lib/rtpmidi.h) learns
// neither the name nor anything else about the client, so the name comes from here.
typedef struct {
    uint32_t ip;
    char name[64];
} remembered_t;
static remembered_t last_client;
static bool save_last_client;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
static int port_of(int session) { return session == 0 ? PORT_RTP_0 : PORT_RTP_1; }

// ---- lib callbacks (called with `lock` held) -----------------------------------------------------------------

static void cb_send(void *c, int sock, const rtpmidi_addr_t *to, const uint8_t *data, size_t len) {
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(to->port), .sin_addr.s_addr = to->ip};
    sendto(socks[sock], data, len, 0, (struct sockaddr *)&a, sizeof a);
}

static void cb_deliver(void *c, int session, const ump_t *u) { bus_send(port_of(session), u); }

// Applied after the lock is released (bus_set_up may wait for the bus task, which may wait for the lock).
static int8_t pending_up[RTPMIDI_SESSIONS] = {-1, -1};

static void cb_session(void *c, int session, bool up, const char *peer) {
    rtpmidi_session_t *s = &host.s[session];
    if (up && s->resumed && s->ctrl.ip == last_client.ip)
        snprintf(s->peer_name, sizeof s->peer_name, "%s", last_client.name);  // taken back without a name
    ESP_LOGI(TAG, "session %d %s (\"%s\")%s", session, up ? "up" : "down", s->peer_name,
             up && s->resumed ? ", taken back" : "");
    if (up && !s->resumed && (s->ctrl.ip != last_client.ip || strcmp(s->peer_name, last_client.name))) {
        last_client.ip = s->ctrl.ip;
        snprintf(last_client.name, sizeof last_client.name, "%s", s->peer_name);
        save_last_client = true;  // written by the UDP task, outside the lock
    }
    pending_up[session] = up;
}

static void load_last_client(void) {
    nvs_handle_t h;
    size_t len = sizeof last_client;
    if (nvs_open("rtpmidi", NVS_READONLY, &h) != ESP_OK) return;
    if (nvs_get_blob(h, "client", &last_client, &len) != ESP_OK || len != sizeof last_client)
        memset(&last_client, 0, sizeof last_client);
    nvs_close(h);
}

static void store_last_client(void) {
    nvs_handle_t h;
    if (nvs_open("rtpmidi", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "client", &last_client, sizeof last_client);
    nvs_commit(h);
    nvs_close(h);
}

// ---- bus output (bus task) -----------------------------------------------------------------------------------

static void rtp_write(void *c, const ump_t *u) {
    session_ctx_t *s = c;
    if (s->n_batch < BATCH) s->batch[s->n_batch++] = *u;
}

static void rtp_flush(void *c) {
    session_ctx_t *s = c;
    if (!s->n_batch) return;
    xSemaphoreTake(lock, portMAX_DELAY);
    rtpmidi_send(&host, (int)(s - ctx), s->batch, s->n_batch, now_ms());
    xSemaphoreGive(lock);
    s->n_batch = 0;
}

// A full batch holds the rest in the bus's merge queue until the next flush instead of dropping it.
static size_t rtp_room(void *c) {
    const session_ctx_t *s = c;
    return s->n_batch < BATCH ? MIDI1_OUT_MAX : 0;
}

static void rtp_on_up(void *c) { ((session_ctx_t *)c)->n_batch = 0; }

// A MIDI 1.0 stream like TRS and BLE: the bus merges its sources SysEx-aware.
static const bus_output_ops_t ops = {
    .write = rtp_write, .flush = rtp_flush, .room = rtp_room, .on_up = rtp_on_up, .byte_stream = true};

// ---- UDP task ------------------------------------------------------------------------------------------------

static int open_socket(uint16_t port) {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = INADDR_ANY};
    if (s < 0 || bind(s, (struct sockaddr *)&a, sizeof a) < 0) {
        ESP_LOGE(TAG, "cannot bind UDP %u", port);
        return -1;
    }
    return s;
}

static void udp_task(void *arg) {
    socks[RTPMIDI_CONTROL] = open_socket(RTP_MIDI_PORT);
    socks[RTPMIDI_DATA] = open_socket(RTP_MIDI_PORT + 1);
    if (socks[0] < 0 || socks[1] < 0) vTaskDelete(NULL);
    static uint8_t buf[1500];
    uint32_t last_tick = 0, start = now_ms();
    // The session clock comes from wall-clock time, so it carries on across a restart (lib/rtpmidi.h): nothing is
    // answered until SNTP has the time, or for 20 s if it never does (no internet: sessions still work, but a
    // restarted box is not taken back).
    bool clock_set = false;
    for (;;) {
        if (!clock_set && (net_time_valid() || now_ms() - start > 20000)) {
            struct timeval tv;
            gettimeofday(&tv, NULL);
            uint64_t wall = net_time_valid() ? (uint64_t)tv.tv_sec * 10000 + tv.tv_usec / 100 : 0;
            xSemaphoreTake(lock, portMAX_DELAY);
            if (wall) rtpmidi_set_clock(&host, wall - (uint64_t)now_ms() * 10);
            xSemaphoreGive(lock);
            clock_set = true;
            ESP_LOGI(TAG, "session clock from %s", wall ? "wall-clock time" : "boot (no wall-clock time)");
        }
        if (save_last_client) save_last_client = false, store_last_client();

        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(socks[0], &rd);
        FD_SET(socks[1], &rd);
        struct timeval tv = {.tv_sec = 0, .tv_usec = 10000};  // wake every 10 ms for rtpmidi_tick
        int ready = select((socks[0] > socks[1] ? socks[0] : socks[1]) + 1, &rd, NULL, NULL, &tv);
        uint32_t now = now_ms();
        xSemaphoreTake(lock, portMAX_DELAY);
        for (int k = 0; ready > 0 && k < 2; k++) {
            if (!FD_ISSET(socks[k], &rd)) continue;
            struct sockaddr_in from;
            socklen_t fl = sizeof from;
            int n = recvfrom(socks[k], buf, sizeof buf, MSG_DONTWAIT, (struct sockaddr *)&from, &fl);
            if (n <= 0 || !clock_set) continue;
            rtpmidi_addr_t src = {.ip = from.sin_addr.s_addr, .port = ntohs(from.sin_port)};
            rtpmidi_receive(&host, k, &src, buf, (size_t)n, now);
        }
        if (now - last_tick >= 10) {
            rtpmidi_tick(&host, now);
            last_tick = now;
        }
        xSemaphoreGive(lock);
        for (int s = 0; s < RTPMIDI_SESSIONS; s++) {
            if (pending_up[s] >= 0) bus_set_up(port_of(s), pending_up[s]), pending_up[s] = -1;
        }
    }
}

void rtp_midi_start(const char *name) {
    lock = xSemaphoreCreateMutex();
    load_last_client();
    for (int i = 0; i < RTPMIDI_SESSIONS; i++) bus_register_output(port_of(i), &ops, &ctx[i]);
    rtpmidi_callbacks_t cb = {.send = cb_send, .deliver = cb_deliver, .session_changed = cb_session};
    // The SSRC must survive a restart (the client takes us back by it), so it comes from the MAC address.
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_BASE);
    uint32_t ssrc = (uint32_t)mac[2] << 24 | (uint32_t)mac[3] << 16 | (uint32_t)mac[4] << 8 | mac[5];
    rtpmidi_init(&host, name, ssrc, esp_random(), &cb, NULL);
    xTaskCreate(udp_task, "rtpmidi", 4096, NULL, 11, NULL);
    mdns_service_add(name, "_apple-midi", "_udp", RTP_MIDI_PORT, NULL, 0);
    ESP_LOGI(TAG, "RTP-MIDI on UDP %d/%d", RTP_MIDI_PORT, RTP_MIDI_PORT + 1);
}

void rtp_midi_get_info(int session, rtp_midi_session_info_t *out) {
    memset(out, 0, sizeof *out);
    xSemaphoreTake(lock, portMAX_DELAY);
    const rtpmidi_session_t *s = &host.s[session];
    out->active = s->state == RTPMIDI_UP;
    if (out->active) {
        snprintf(out->peer, sizeof out->peer, "%s", s->peer_name);
        struct in_addr ia = {.s_addr = s->ctrl.ip};
        snprintf(out->ip, sizeof out->ip, "%s", inet_ntoa(ia));
        out->resumed = s->resumed;
        out->syncing = s->seq_unknown;
        out->stats = s->stats;
    }
    xSemaphoreGive(lock);
}
