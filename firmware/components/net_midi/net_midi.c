#include "net_midi.h"

#include <stdio.h>
#include <string.h>

#include "airmidi/translate.h"
#include "airmidi/ump_endpoint.h"
#include "bus.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "mdns.h"
#include "net.h"
#include "nvs.h"

static const char *TAG = "netmidi";

#define BATCH 32  // UMPs collected per session between bus flushes

typedef struct {
    ump_endpoint_t ep;  // per session: each client chooses its own protocol
    m1_to_m2_t up;      // MIDI 1.0 -> 2.0 for a client that asked for MIDI 2.0
    ump_t batch[BATCH];
    int n_batch;
    bool announce;  // tell the client who we are once the session can carry data
} session_ctx_t;

static nm2_host_t host;
static session_ctx_t ctx[NM2_SESSIONS];
static SemaphoreHandle_t lock;  // the UDP task and the bus task both drive `host`
static int sock = -1;
static char name_buf[99], piid[13];

// The last client that invited us, kept in NVS: after a restart the box takes its session back at once (nm2.h,
// "Resuming"), instead of waiting for the client to send something.
typedef struct {
    uint32_t ip;
    uint16_t port;
    char name[64];
} remembered_t;
static remembered_t last_client;
static bool have_last_client, save_last_client;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
static int port_of(int session) { return session == 0 ? PORT_NET_0 : PORT_NET_1; }

// ---- nm2 callbacks (called with `lock` held) ------------------------------------------------------------------

static void cb_send(void *c, const nm2_addr_t *to, const uint8_t *data, size_t len) {
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(to->port), .sin_addr.s_addr = to->ip};
    sendto(sock, data, len, 0, (struct sockaddr *)&a, sizeof a);
}

static ump_t replies[NM2_SESSIONS][UMP_ENDPOINT_MAX_REPLY];
static int n_replies[NM2_SESSIONS];

static void cb_deliver(void *c, int session, const ump_t *u) {
    if (ump_mt(u) == UMP_MT_STREAM) {
        // Endpoint / function block discovery: answered after nm2_host_receive() returns, not from inside it.
        ump_t out[UMP_ENDPOINT_MAX_REPLY];
        int n = ump_endpoint_handle(&ctx[session].ep, u, out);
        for (int i = 0; i < n && n_replies[session] < UMP_ENDPOINT_MAX_REPLY; i++)
            replies[session][n_replies[session]++] = out[i];
        return;
    }
    bus_send(port_of(session), u);  // MIDI 2.0 messages are translated down by the MIDI 1.0 outputs
}

// Port changes are applied after the lock is released: bus_set_up() may wait for room in the bus queue, and the
// bus task may be waiting for this lock (net_flush).
static int8_t pending_up[NM2_SESSIONS] = {-1, -1};

static void cb_session(void *c, int session, bool up, const char *peer) {
    nm2_session_t *s = &host.s[session];
    if (up && s->resumed && !peer[0] && have_last_client && s->peer.ip == last_client.ip &&
        s->peer.port == last_client.port)
        snprintf(s->peer_name, sizeof s->peer_name, "%s", last_client.name);  // resumed without a name: we know it
    ESP_LOGI(TAG, "session %d %s (\"%s\")%s", session, up ? "up" : "down", s->peer_name,
             up && s->resumed ? ", resumed" : "");
    if (up) {
        ctx[session].ep.protocol = 1;
        ctx[session].announce = true;
        m1_to_m2_init(&ctx[session].up);
        if (!s->resumed && (s->peer.ip != last_client.ip || s->peer.port != last_client.port ||
                            strcmp(s->peer_name, last_client.name))) {
            last_client = (remembered_t){.ip = s->peer.ip, .port = s->peer.port};
            snprintf(last_client.name, sizeof last_client.name, "%s", s->peer_name);
            have_last_client = save_last_client = true;  // written by the UDP task, outside the lock
        }
    }
    pending_up[session] = up;
}

static void load_last_client(void) {
    nvs_handle_t h;
    size_t len = sizeof last_client;
    if (nvs_open("netmidi", NVS_READONLY, &h) != ESP_OK) return;
    have_last_client = nvs_get_blob(h, "client", &last_client, &len) == ESP_OK && len == sizeof last_client;
    nvs_close(h);
}

static void store_last_client(void) {
    nvs_handle_t h;
    if (nvs_open("netmidi", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "client", &last_client, sizeof last_client);
    nvs_commit(h);
    nvs_close(h);
}

// ---- bus output (bus task) -----------------------------------------------------------------------------------

static void net_write(void *c, const ump_t *u) {
    session_ctx_t *s = c;
    ump_t out[2];
    int n = 1;
    out[0] = *u;
    if (s->ep.protocol == 2 && ump_mt(u) == UMP_MT_M1_CV) n = m1_to_m2(&s->up, u, out);
    for (int i = 0; i < n && s->n_batch < BATCH; i++) s->batch[s->n_batch++] = out[i];
}

static void net_flush(void *c) {
    session_ctx_t *s = c;
    if (!s->n_batch) return;
    xSemaphoreTake(lock, portMAX_DELAY);
    nm2_host_send(&host, (int)(s - ctx), s->batch, s->n_batch, now_ms());
    xSemaphoreGive(lock);
    s->n_batch = 0;
}

static const bus_output_ops_t ops = {.write = net_write, .flush = net_flush};

// ---- UDP task ------------------------------------------------------------------------------------------------

static void udp_task(void *arg) {
    sock = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(NET_MIDI_PORT), .sin_addr.s_addr = INADDR_ANY};
    if (bind(sock, (struct sockaddr *)&a, sizeof a) < 0) {
        ESP_LOGE(TAG, "cannot bind UDP %d", NET_MIDI_PORT);
        vTaskDelete(NULL);
    }
    struct timeval tv = {.tv_sec = 0, .tv_usec = 10000};  // wake every 10 ms for nm2_host_tick
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    static uint8_t buf[1500];
    uint32_t last_tick = 0;
    bool was_connected = false;
    for (;;) {
        // Joined (again): take back the session with the last client, if it still has one with us.
        bool connected = net_state() == NET_CONNECTED;
        if (connected && !was_connected && have_last_client) {
            nm2_addr_t a = {.ip = last_client.ip, .port = last_client.port};
            xSemaphoreTake(lock, portMAX_DELAY);
            nm2_host_resume(&host, &a, last_client.name, now_ms());
            xSemaphoreGive(lock);
        }
        was_connected = connected;
        if (save_last_client) save_last_client = false, store_last_client();

        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        int n = recvfrom(sock, buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
        uint32_t now = now_ms();
        xSemaphoreTake(lock, portMAX_DELAY);
        if (n > 0) {
            nm2_addr_t src = {.ip = from.sin_addr.s_addr, .port = ntohs(from.sin_port)};
            nm2_host_receive(&host, &src, buf, n, now);
            for (int s = 0; s < NM2_SESSIONS; s++) {
                if (n_replies[s]) nm2_host_send(&host, s, replies[s], n_replies[s], now);
                n_replies[s] = 0;
            }
        }
        if (now - last_tick >= 10) {
            nm2_host_tick(&host, now);
            last_tick = now;
        }
        for (int s = 0; s < NM2_SESSIONS; s++) {
            // Only once the client has spoken on the session: UMP Data right behind Invitation Accepted made macOS
            // answer Bye "Session Not Established" in 1 of 4 connects. And not before a resumed session's Session
            // Reset is answered: nm2 sends no data until then.
            if (!ctx[s].announce || !host.s[s].active || !host.s[s].heard || host.s[s].reset_pending) continue;
            ump_t out[UMP_ENDPOINT_MAX_REPLY];
            nm2_host_send(&host, s, out, ump_endpoint_announce(&ctx[s].ep, out), now);
            ctx[s].announce = false;
        }
        xSemaphoreGive(lock);
        for (int s = 0; s < NM2_SESSIONS; s++) {
            if (pending_up[s] >= 0) bus_set_up(port_of(s), pending_up[s]), pending_up[s] = -1;
        }
    }
}

void net_midi_start(const char *name) {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_BASE);
    snprintf(piid, sizeof piid, "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    snprintf(name_buf, sizeof name_buf, "%s", name);
    lock = xSemaphoreCreateMutex();
    load_last_client();
    for (int i = 0; i < NM2_SESSIONS; i++) {
        ctx[i].ep = (ump_endpoint_t){.name = name_buf,
                                     .product_instance_id = piid,
                                     .manufacturer = {0x7D, 0, 0},
                                     .family = 0,
                                     .model = 1,
                                     .block_name = name_buf,
                                     .midi2_capable = true,
                                     .ci_version = 2,  // the TRS device's MIDI-CI passes through the box
                                     .protocol = 1};
        bus_register_output(port_of(i), &ops, &ctx[i]);
    }
    nm2_callbacks_t cb = {.send = cb_send, .deliver = cb_deliver, .session_changed = cb_session};
    nm2_host_init(&host, name_buf, piid, &cb, NULL);
    xTaskCreate(udp_task, "netmidi", 6144, NULL, 11, NULL);
    mdns_txt_item_t txt[] = {{"UMPEndpointName", name_buf}, {"ProductInstanceId", piid}};
    mdns_service_add(name_buf, "_midi2", "_udp", NET_MIDI_PORT, txt, 2);
    ESP_LOGI(TAG, "Network MIDI 2.0 host on UDP %d", NET_MIDI_PORT);
}

void net_midi_get_info(int session, net_midi_session_info_t *out) {
    memset(out, 0, sizeof *out);
    xSemaphoreTake(lock, portMAX_DELAY);
    const nm2_session_t *s = &host.s[session];
    out->active = s->active;
    if (s->active) {
        snprintf(out->peer, sizeof out->peer, "%s", s->peer_name);
        struct in_addr ia = {.s_addr = s->peer.ip};
        snprintf(out->ip, sizeof out->ip, "%s", inet_ntoa(ia));
        out->protocol = ctx[session].ep.protocol;
        out->stats = s->stats;
    }
    xSemaphoreGive(lock);
}
