#include "ble_midi.h"

#include <string.h>

#include "airmidi/blemidi.h"
#include "airmidi/midi1.h"
#include "bus.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "host/ble_hs.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "ble";

#define SLOTS 2
#define BACKLOG 16   // notifications waiting for NimBLE buffers, per connection (16 x 244 bytes)
#define ATT_MAX 512  // largest attribute value a central may write, whatever the MTU

// BLE MIDI service 03B80E5A-EDE8-4B33-A751-6CE34EC4C700, characteristic 7772E5DB-3868-4112-A1A9-F2669D106BF3.
// NimBLE takes 128-bit UUIDs little-endian.
static const ble_uuid128_t midi_svc_uuid =
    BLE_UUID128_INIT(0x00, 0xC7, 0xC4, 0x4E, 0xE3, 0x6C, 0x51, 0xA7, 0x33, 0x4B, 0xE8, 0xED, 0x5A, 0x0E, 0xB8, 0x03);
static const ble_uuid128_t midi_chr_uuid =
    BLE_UUID128_INIT(0xF3, 0x6B, 0x10, 0x9D, 0x66, 0xF2, 0xA9, 0xA1, 0x12, 0x41, 0x68, 0x38, 0xDB, 0xE5, 0x72, 0x77);

// Each slot is shared by two tasks: the NimBLE host task owns conn, parser and stats; the bus task owns tx. What
// crosses over (a new MTU) goes through `mtu`, which the bus task applies before its next write.
typedef struct {
    uint16_t conn;  // BLE_HS_CONN_HANDLE_NONE when free
    midi1_in_t parser;
    blemidi_tx_t tx;
    volatile uint16_t mtu;                         // latest ATT MTU (23 until exchanged)
    uint16_t tx_mtu;                               // the MTU tx is sized for
    uint8_t backlog[BACKLOG][BLEMIDI_MAX_PACKET];  // bus task: packets NimBLE had no buffer for yet, oldest first
    uint16_t backlog_len[BACKLOG];
    int n_backlog;
    ble_midi_slot_stats_t stats;
} slot_t;

static slot_t slots[SLOTS];
static uint16_t midi_handle;
static uint8_t own_addr_type;
static char dev_name[32];
static volatile bool slow_adv;  // Wi-Fi is on: advertise rarely, the radio is shared
static volatile bool synced;

static int port_of(int slot) { return slot == 0 ? PORT_BLE_0 : PORT_BLE_1; }

static slot_t *slot_by_conn(uint16_t conn) {
    for (int i = 0; i < SLOTS; i++)
        if (slots[i].conn == conn) return &slots[i];
    return NULL;
}

// ---- receive (NimBLE host task) ------------------------------------------------------------------------------

static int midi_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg) {
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return 0;  // reads return an empty value, as the spec requires
    slot_t *s = slot_by_conn(conn);
    if (!s) return 0;
    static uint8_t pkt[ATT_MAX], bytes[ATT_MAX];  // host task only; static keeps 1 KB off its stack
    uint16_t len = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, pkt, sizeof pkt, &len)) {
        s->stats.rx_bad++;
        return 0;
    }
    int n = blemidi_decode(pkt, len, bytes);
    if (n < 0) {
        s->stats.rx_bad++;
        return 0;
    }
    s->stats.rx_packets++;
    int port = port_of(s - slots);
    for (int i = 0; i < n; i++) {
        ump_t u[2];
        int k = midi1_in_feed(&s->parser, bytes[i], u);
        for (int j = 0; j < k; j++) bus_send(port, &u[j]);
    }
    return 0;
}

static const struct ble_gatt_svc_def services[] = {
    {.type = BLE_GATT_SVC_TYPE_PRIMARY,
     .uuid = &midi_svc_uuid.u,
     .characteristics = (struct ble_gatt_chr_def[]){{.uuid = &midi_chr_uuid.u,
                                                     .access_cb = midi_access,
                                                     .val_handle = &midi_handle,
                                                     .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE_NO_RSP |
                                                              BLE_GATT_CHR_F_NOTIFY},
                                                    {0}}},
    {0}};

// ---- send (bus task) -----------------------------------------------------------------------------------------

static bool notify(slot_t *s, const uint8_t *pkt, size_t len) {
    struct os_mbuf *om = ble_hs_mbuf_from_flat(pkt, len);
    if (!om) return false;
    // ble_gatts_notify_custom() consumes the mbuf whether or not it succeeds.
    return ble_gatts_notify_custom(s->conn, midi_handle, om) == 0;
}

// Send what NimBLE could not take earlier, oldest first; stop at the first one it still cannot take.
static void send_backlog(slot_t *s) {
    int sent = 0;
    while (sent < s->n_backlog && notify(s, s->backlog[sent], s->backlog_len[sent])) sent++;
    if (!sent) return;
    s->stats.tx_packets += sent;
    s->n_backlog -= sent;
    memmove(s->backlog, s->backlog[sent], s->n_backlog * sizeof s->backlog[0]);
    memmove(s->backlog_len, s->backlog_len + sent, s->n_backlog * sizeof s->backlog_len[0]);
}

// Notifications are queued by the host for the next connection events. When it is out of buffers (a burst), the
// packet waits in the backlog and goes out on a later bus tick, in order; only a full backlog drops (counted).
static void emit_packet(void *ctx, const uint8_t *pkt, size_t len) {
    slot_t *s = ctx;
    send_backlog(s);
    if (!s->n_backlog && notify(s, pkt, len)) {
        s->stats.tx_packets++;
        return;
    }
    if (s->n_backlog == BACKLOG) {
        s->stats.tx_failed++;
        return;
    }
    memcpy(s->backlog[s->n_backlog], pkt, len);
    s->backlog_len[s->n_backlog++] = len;
}

static void apply_mtu(slot_t *s) {
    uint16_t mtu = s->mtu;
    if (mtu != s->tx_mtu) blemidi_tx_set_max(&s->tx, mtu - 3), s->tx_mtu = mtu;
}

static void ble_on_up(void *ctx) {
    slot_t *s = ctx;
    s->n_backlog = 0;
    s->tx_mtu = s->mtu;
    blemidi_tx_init(&s->tx, s->tx_mtu - 3, emit_packet, s);
}

static void ble_write(void *ctx, const ump_t *u) {
    slot_t *s = ctx;
    apply_mtu(s);
    blemidi_tx_add(&s->tx, u, (uint32_t)(esp_timer_get_time() / 1000));
}

// Called by the bus task after every batch and at least every 10 ms, so the backlog drains even when idle. While
// the radio is behind (backlog not empty), the packet being built is not sent half empty: it keeps filling up to
// the MTU, so a burst becomes a few full packets instead of many small ones.
static void ble_flush(void *ctx) {
    slot_t *s = ctx;
    apply_mtu(s);
    send_backlog(s);
    if (!s->n_backlog) blemidi_tx_flush(&s->tx);
}

// Room for one more message: the packet being built may have to go to the backlog when it fills.
static size_t ble_room(void *ctx) {
    slot_t *s = ctx;
    return s->n_backlog < BACKLOG ? BLEMIDI_MAX_PACKET : 0;
}

static const bus_output_ops_t ops = {
    .write = ble_write, .flush = ble_flush, .room = ble_room, .on_up = ble_on_up, .byte_stream = true};

// ---- GAP -----------------------------------------------------------------------------------------------------

static void advertise(void);

static int gap_event(struct ble_gap_event *e, void *arg) {
    switch (e->type) {
        case BLE_GAP_EVENT_CONNECT: {
            if (e->connect.status) {
                advertise();
                break;
            }
            slot_t *s = slot_by_conn(BLE_HS_CONN_HANDLE_NONE);
            if (!s) {  // cannot happen with max connections = SLOTS, but never index past the table
                ble_gap_terminate(e->connect.conn_handle, BLE_ERR_CONN_LIMIT);
                break;
            }
            memset(&s->stats, 0, sizeof s->stats);
            s->conn = e->connect.conn_handle;
            s->stats.connected = true;
            midi1_in_init(&s->parser, 0);
            s->mtu = BLE_ATT_MTU_DFLT;
            struct ble_gap_conn_desc d;
            if (ble_gap_conn_find(s->conn, &d) == 0) s->stats.interval_x125 = d.conn_itvl;
            // Apple's accessory guidelines: interval min >= 11.25 ms for MIDI and max >= min + 15 ms, or iOS/macOS
            // reject the request; macOS's own MIDI driver then picks 7.5 ms anyway (spike S2).
            struct ble_gap_upd_params up = {.itvl_min = 9, .itvl_max = 21, .latency = 0, .supervision_timeout = 200};
            ble_gap_update_params(s->conn, &up);
            ESP_LOGI(TAG, "slot %d connected (interval %.2f ms)", (int)(s - slots), s->stats.interval_x125 * 1.25);
            advertise();  // keep advertising while a slot is free
            break;
        }
        case BLE_GAP_EVENT_DISCONNECT: {
            slot_t *s = slot_by_conn(e->disconnect.conn.conn_handle);
            if (s) {
                ESP_LOGI(TAG, "slot %d disconnected (reason 0x%x)", (int)(s - slots), e->disconnect.reason);
                bus_set_up(port_of(s - slots), false);
                s->conn = BLE_HS_CONN_HANDLE_NONE;
                s->stats.connected = s->stats.subscribed = false;
            }
            advertise();
            break;
        }
        case BLE_GAP_EVENT_CONN_UPDATE: {
            slot_t *s = slot_by_conn(e->conn_update.conn_handle);
            struct ble_gap_conn_desc d;
            if (s && ble_gap_conn_find(s->conn, &d) == 0) {
                s->stats.interval_x125 = d.conn_itvl;
                ESP_LOGI(TAG, "slot %d interval %.2f ms", (int)(s - slots), d.conn_itvl * 1.25);
            }
            break;
        }
        case BLE_GAP_EVENT_SUBSCRIBE: {
            slot_t *s = slot_by_conn(e->subscribe.conn_handle);
            if (s && e->subscribe.attr_handle == midi_handle) {
                s->stats.subscribed = e->subscribe.cur_notify;
                bus_set_up(port_of(s - slots), s->stats.subscribed);
            }
            break;
        }
        case BLE_GAP_EVENT_MTU: {
            slot_t *s = slot_by_conn(e->mtu.conn_handle);
            if (s) s->stats.mtu = s->mtu = e->mtu.value;  // the bus task resizes its packets before the next write
            break;
        }
        case BLE_GAP_EVENT_ADV_COMPLETE:
            advertise();
            break;
    }
    return 0;
}

static void advertise(void) {
    if (!slot_by_conn(BLE_HS_CONN_HANDLE_NONE) || ble_gap_adv_active()) return;
    // The advertisement (31 bytes) holds the flags and the 128-bit service UUID; the name goes in the scan
    // response.
    struct ble_hs_adv_fields f = {0};
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    f.uuids128 = (ble_uuid128_t *)&midi_svc_uuid;
    f.num_uuids128 = 1;
    f.uuids128_is_complete = 1;
    ble_gap_adv_set_fields(&f);
    struct ble_hs_adv_fields rsp = {0};
    rsp.name = (uint8_t *)dev_name;
    rsp.name_len = strlen(dev_name);
    rsp.name_is_complete = 1;
    ble_gap_adv_rsp_set_fields(&rsp);
    struct ble_gap_adv_params p = {.conn_mode = BLE_GAP_CONN_MODE_UND,
                                   .disc_mode = BLE_GAP_DISC_MODE_GEN,
                                   .itvl_min = slow_adv ? BLE_GAP_ADV_ITVL_MS(1000) : BLE_GAP_ADV_ITVL_MS(100),
                                   .itvl_max = slow_adv ? BLE_GAP_ADV_ITVL_MS(1100) : BLE_GAP_ADV_ITVL_MS(150)};
    int rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, &p, gap_event, NULL);
    if (rc) ESP_LOGW(TAG, "advertising failed: %d", rc);
}

static void on_sync(void) {
    ble_hs_id_infer_auto(0, &own_addr_type);
    advertise();
    synced = true;
    ESP_LOGI(TAG, "advertising as %s", dev_name);
}

static void on_reset(int reason) { ESP_LOGW(TAG, "host reset, reason %d", reason); }

static void host_task(void *p) {
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void ble_midi_start(const char *name) {
    strncpy(dev_name, name, sizeof dev_name - 1);
    for (int i = 0; i < SLOTS; i++) {
        slots[i].conn = BLE_HS_CONN_HANDLE_NONE;
        bus_register_output(port_of(i), &ops, &slots[i]);
    }
    ESP_ERROR_CHECK(nimble_port_init());
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set(dev_name);
    ESP_ERROR_CHECK(ble_gatts_count_cfg(services));
    ESP_ERROR_CHECK(ble_gatts_add_svcs(services));
    nimble_port_freertos_init(host_task);
}

void ble_midi_get_stats(int slot, ble_midi_slot_stats_t *out) { *out = slots[slot].stats; }

bool ble_midi_ready(void) { return synced; }

// Advertising every 100-150 ms costs Wi-Fi airtime on the shared radio: with it, pings to the box averaged 30 ms,
// with advertising stopped 14.5 ms (measured). With Wi-Fi on, advertise about once a second instead: a central
// still finds the box within a second or two.
void ble_midi_set_slow_advertising(bool slow) {
    if (slow == slow_adv) return;
    slow_adv = slow;
    if (synced && ble_gap_adv_active()) {
        ble_gap_adv_stop();
        advertise();
    }
}

bool ble_midi_connected(void) {
    for (int i = 0; i < SLOTS; i++)
        if (slots[i].conn != BLE_HS_CONN_HANDLE_NONE) return true;
    return false;
}
