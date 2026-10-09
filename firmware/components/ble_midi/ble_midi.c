#include "ble_midi.h"

#include <string.h>
#include <strings.h>

#include "airmidi/blemidi.h"
#include "airmidi/midi1.h"
#include "bus.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "ble";

#define PERIPHERAL_SLOTS 2  // centrals connected to us (the Mac, an iPad)
#define CENTRAL_SLOT 2      // the device we connect to ourselves
#define SLOTS 3
#define BACKLOG 16   // packets waiting for NimBLE buffers, per connection (16 x 244 bytes)
#define ATT_MAX 512  // largest attribute value a peer may write, whatever the MTU
#define RETRY_MS 1000

// BLE MIDI service 03B80E5A-EDE8-4B33-A751-6CE34EC4C700, characteristic 7772E5DB-3868-4112-A1A9-F2669D106BF3.
// NimBLE takes 128-bit UUIDs little-endian.
static const ble_uuid128_t midi_svc_uuid =
    BLE_UUID128_INIT(0x00, 0xC7, 0xC4, 0x4E, 0xE3, 0x6C, 0x51, 0xA7, 0x33, 0x4B, 0xE8, 0xED, 0x5A, 0x0E, 0xB8, 0x03);
static const ble_uuid128_t midi_chr_uuid =
    BLE_UUID128_INIT(0xF3, 0x6B, 0x10, 0x9D, 0x66, 0xF2, 0xA9, 0xA1, 0x12, 0x41, 0x68, 0x38, 0xDB, 0xE5, 0x72, 0x77);

// Each slot is shared by two tasks: the NimBLE host task owns conn, parser and stats; the bus task owns tx. What
// crosses over (a new MTU) goes through `mtu`, which the bus task applies before its next write.
typedef struct {
    uint16_t conn;         // BLE_HS_CONN_HANDLE_NONE when free
    bool central;          // we connected (we write to the device's characteristic) rather than it to us (we notify)
    uint16_t peer_handle;  // central: the device's MIDI characteristic value
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

static int port_of(int slot) { return slot == 0 ? PORT_BLE_0 : slot == 1 ? PORT_BLE_1 : PORT_BLE_CENTRAL; }

static slot_t *slot_by_conn(uint16_t conn) {
    for (int i = 0; i < SLOTS; i++)
        if (slots[i].conn == conn) return &slots[i];
    return NULL;
}

static slot_t *free_peripheral_slot(void) {
    for (int i = 0; i < PERIPHERAL_SLOTS; i++)
        if (slots[i].conn == BLE_HS_CONN_HANDLE_NONE) return &slots[i];
    return NULL;
}

static void slot_connected(slot_t *s, uint16_t conn) {
    memset(&s->stats, 0, sizeof s->stats);
    s->conn = conn;
    s->stats.connected = true;
    midi1_in_init(&s->parser, 0);
    s->mtu = BLE_ATT_MTU_DFLT;
    struct ble_gap_conn_desc d;
    if (ble_gap_conn_find(conn, &d) == 0) s->stats.interval_x125 = d.conn_itvl;
}

static void slot_disconnected(slot_t *s) {
    bus_set_up(port_of(s - slots), false);
    s->conn = BLE_HS_CONN_HANDLE_NONE;
    s->stats.connected = s->stats.subscribed = false;
}

// ---- receive (NimBLE host task) ------------------------------------------------------------------------------

// One BLE MIDI packet from a peer, written to us (peripheral) or notified to us (central).
static void receive_packet(slot_t *s, struct os_mbuf *om) {
    static uint8_t pkt[ATT_MAX], bytes[ATT_MAX];  // host task only; static keeps 1 KB off its stack
    uint16_t len = 0;
    if (ble_hs_mbuf_to_flat(om, pkt, sizeof pkt, &len)) {
        s->stats.rx_bad++;
        return;
    }
    int n = blemidi_decode(pkt, len, bytes);
    if (n < 0) {
        s->stats.rx_bad++;
        return;
    }
    s->stats.rx_packets++;
    int port = port_of(s - slots);
    for (int i = 0; i < n; i++) {
        ump_t u[2];
        int k = midi1_in_feed(&s->parser, bytes[i], u);
        for (int j = 0; j < k; j++) bus_send(port, &u[j]);
    }
}

static int midi_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg) {
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return 0;  // reads return an empty value, as the spec requires
    slot_t *s = slot_by_conn(conn);
    if (s && !s->central) receive_packet(s, ctxt->om);
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

// A notification to a central, or a write without response to the device we connected to.
static bool send_one(slot_t *s, const uint8_t *pkt, size_t len) {
    if (s->central) return ble_gattc_write_no_rsp_flat(s->conn, s->peer_handle, pkt, len) == 0;
    struct os_mbuf *om = ble_hs_mbuf_from_flat(pkt, len);
    if (!om) return false;
    // ble_gatts_notify_custom() consumes the mbuf whether or not it succeeds.
    return ble_gatts_notify_custom(s->conn, midi_handle, om) == 0;
}

// Send what NimBLE could not take earlier, oldest first; stop at the first one it still cannot take.
static void send_backlog(slot_t *s) {
    int sent = 0;
    while (sent < s->n_backlog && send_one(s, s->backlog[sent], s->backlog_len[sent])) sent++;
    if (!sent) return;
    s->stats.tx_packets += sent;
    s->n_backlog -= sent;
    memmove(s->backlog, s->backlog[sent], s->n_backlog * sizeof s->backlog[0]);
    memmove(s->backlog_len, s->backlog_len + sent, s->n_backlog * sizeof s->backlog_len[0]);
}

// Packets are queued by the host for the next connection events. When it is out of buffers (a burst), the packet
// waits in the backlog and goes out on a later bus tick, in order; only a full backlog drops (counted).
static void emit_packet(void *ctx, const uint8_t *pkt, size_t len) {
    slot_t *s = ctx;
    send_backlog(s);
    if (!s->n_backlog && send_one(s, pkt, len)) {
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

// ---- central: finding and connecting to a BLE MIDI device (host task) -----------------------------------------
//
// With a target set (settings: "*" for any BLE MIDI device, or a name), the box scans until it sees one, connects,
// finds the MIDI characteristic, subscribes to it and exchanges the MTU; then the device is PORT_BLE_CENTRAL. If
// the device asks for pairing (an encrypted characteristic), it is paired "just works" and bonded. Lost, the box
// scans again.

static char target[SETTINGS_DEVICE_MAX + 1];
static ble_midi_central_state_t central_state = BLE_CENTRAL_OFF;
static ble_midi_found_t seen[BLE_MIDI_FOUND_MAX];  // devices heard while scanning, for the web page and matching
static int n_seen;
static bool listing;   // a scan the web page asked for is running
static uint16_t cccd;  // the device's Client Characteristic Configuration descriptor
static uint16_t svc_end;
static bool secured;  // encryption was started once for this connection
static struct ble_npl_callout retry_timer;
static char central_name[SETTINGS_DEVICE_MAX + 1];

static int central_event(struct ble_gap_event *e, void *arg);
static void central_scan(void);
static void on_control(struct ble_npl_event *ev);
static struct ble_npl_event control_ev;

static void retry_later(void) { ble_npl_callout_reset(&retry_timer, ble_npl_time_ms_to_ticks32(RETRY_MS)); }
static void on_retry(struct ble_npl_event *ev) { central_scan(); }

static void central_fail(const char *why, int rc) {
    ESP_LOGW(TAG, "device: %s (%d)", why, rc);
    if (slots[CENTRAL_SLOT].conn != BLE_HS_CONN_HANDLE_NONE)
        ble_gap_terminate(slots[CENTRAL_SLOT].conn, BLE_ERR_REM_USER_CONN_TERM);  // retried on the disconnect
    else
        retry_later();
}

static int subscribed_cb(uint16_t conn, const struct ble_gatt_error *err, struct ble_gatt_attr *attr, void *arg) {
    slot_t *s = &slots[CENTRAL_SLOT];
    int st = err->status;
    if (st == BLE_HS_ERR_ATT_BASE + BLE_ATT_ERR_INSUFFICIENT_AUTHEN ||
        st == BLE_HS_ERR_ATT_BASE + BLE_ATT_ERR_INSUFFICIENT_ENC) {
        if (!secured) {  // the device wants pairing first; the subscription is retried once encrypted
            secured = true;
            ESP_LOGI(TAG, "device asks for pairing");
            int rc = ble_gap_security_initiate(conn);
            if (rc) central_fail("pairing could not start", rc);
            return 0;
        }
    }
    if (st) {
        central_fail("subscribing failed", st);
        return 0;
    }
    s->stats.subscribed = true;
    central_state = BLE_CENTRAL_CONNECTED;
    ESP_LOGI(TAG, "device \"%s\" connected (interval %.2f ms)", central_name, s->stats.interval_x125 * 1.25);
    bus_set_up(PORT_BLE_CENTRAL, true);
    return 0;
}

static void subscribe(uint16_t conn) {
    static const uint8_t on[2] = {0x01, 0x00};  // notifications
    int rc = ble_gattc_write_flat(conn, cccd, on, sizeof on, subscribed_cb, NULL);
    if (rc) central_fail("subscribing failed", rc);
}

static int dsc_cb(uint16_t conn, const struct ble_gatt_error *err, uint16_t chr, const struct ble_gatt_dsc *dsc,
                  void *arg) {
    if (err->status == 0) {
        if (ble_uuid_cmp(&dsc->uuid.u, BLE_UUID16_DECLARE(BLE_GATT_DSC_CLT_CFG_UUID16)) == 0) cccd = dsc->handle;
        return 0;
    }
    if (err->status != BLE_HS_EDONE || !cccd)
        central_fail("no notifications on its MIDI characteristic", err->status);
    else
        subscribe(conn);
    return 0;
}

static int chr_cb(uint16_t conn, const struct ble_gatt_error *err, const struct ble_gatt_chr *chr, void *arg) {
    slot_t *s = &slots[CENTRAL_SLOT];
    if (err->status == 0) {
        s->peer_handle = chr->val_handle;
        return 0;
    }
    int rc = err->status;
    if (rc == BLE_HS_EDONE && s->peer_handle) rc = ble_gattc_disc_all_dscs(conn, s->peer_handle, svc_end, dsc_cb, NULL);
    if (rc) central_fail("no MIDI characteristic", rc);
    return 0;
}

static int svc_cb(uint16_t conn, const struct ble_gatt_error *err, const struct ble_gatt_svc *svc, void *arg) {
    static uint16_t start;
    if (err->status == 0) {
        start = svc->start_handle, svc_end = svc->end_handle;
        return 0;
    }
    int rc = err->status;
    if (rc == BLE_HS_EDONE && start)
        rc = ble_gattc_disc_chrs_by_uuid(conn, start, svc_end, &midi_chr_uuid.u, chr_cb, NULL);
    if (rc) central_fail("no MIDI service", rc);
    start = 0;
    return 0;
}

static void discover(uint16_t conn) {
    int rc = ble_gattc_disc_svc_by_uuid(conn, &midi_svc_uuid.u, svc_cb, NULL);
    if (rc) central_fail("service discovery failed", rc);
}

static int mtu_cb(uint16_t conn, const struct ble_gatt_error *err, uint16_t mtu, void *arg) {
    if (err->status == 0) slots[CENTRAL_SLOT].stats.mtu = slots[CENTRAL_SLOT].mtu = mtu;
    discover(conn);  // with the default MTU if the exchange failed
    return 0;
}

// Does this device match the target: any, its name, or its address? The name may still be empty (it often comes in
// the scan response) or never come (macOS's own peripheral advertises none).
static bool wanted(const ble_midi_found_t *f) {
    return f->midi &&
           (!strcmp(target, "*") || (f->name[0] && !strcmp(target, f->name)) || !strcasecmp(target, f->address));
}

static void connect_to(const ble_midi_found_t *f) {
    ble_gap_disc_cancel();
    listing = false;
    // BLE MIDI: 7.5-15 ms interval (the device may ask for more), 2 s supervision timeout.
    struct ble_gap_conn_params cp = {.scan_itvl = 16,
                                     .scan_window = 16,
                                     .itvl_min = 6,
                                     .itvl_max = 12,
                                     .latency = 0,
                                     .supervision_timeout = 200,
                                     .min_ce_len = 0,
                                     .max_ce_len = 0};
    snprintf(central_name, sizeof central_name, "%s", f->name[0] ? f->name : f->address);
    int rc = ble_gap_connect(own_addr_type, &f->addr, 5000, &cp, central_event, NULL);
    if (rc) {
        central_fail("cannot connect", rc);
        return;
    }
    central_state = BLE_CENTRAL_CONNECTING;
    ESP_LOGI(TAG, "connecting to \"%s\"", central_name);
}

// A scan result: remember it (name, RSSI, whether it offers BLE MIDI), and connect if it is the one.
static void seen_device(const struct ble_gap_disc_desc *d) {
    struct ble_hs_adv_fields f;
    if (ble_hs_adv_parse_fields(&f, d->data, d->length_data)) return;
    bool midi = false;
    for (int i = 0; i < f.num_uuids128; i++) midi |= ble_uuid_cmp(&f.uuids128[i].u, &midi_svc_uuid.u) == 0;
    ble_midi_found_t *e = NULL;
    for (int i = 0; i < n_seen && !e; i++)
        if (!ble_addr_cmp(&seen[i].addr, &d->addr)) e = &seen[i];
    if (!e) {
        if (!midi) return;  // only BLE MIDI devices are listed (their names arrive with or after this)
        e = &seen[n_seen < BLE_MIDI_FOUND_MAX ? n_seen++ : BLE_MIDI_FOUND_MAX - 1];
        memset(e, 0, sizeof *e);
        e->addr = d->addr;
        const uint8_t *a = d->addr.val;  // little-endian: printed the usual way round
        snprintf(e->address, sizeof e->address, "%02x:%02x:%02x:%02x:%02x:%02x", a[5], a[4], a[3], a[2], a[1], a[0]);
    }
    e->midi |= midi;
    e->rssi = d->rssi;
    e->seen_ms = (uint32_t)(esp_timer_get_time() / 1000);
    if (f.name_len) {
        size_t n = f.name_len < SETTINGS_DEVICE_MAX ? f.name_len : SETTINGS_DEVICE_MAX;
        memcpy(e->name, f.name, n);
        e->name[n] = 0;
    }
    if (target[0] && central_state == BLE_CENTRAL_SCANNING && wanted(e)) connect_to(e);
}

static int scan_event(struct ble_gap_event *e, void *arg) {
    if (e->type == BLE_GAP_EVENT_DISC) seen_device(&e->disc);
    if (e->type == BLE_GAP_EVENT_DISC_COMPLETE) {
        listing = false;
        if (central_state == BLE_CENTRAL_SCANNING) central_scan();  // keep looking for the target
    }
    return 0;
}

// Scan for the target: 30 ms in every 100 (a third of the radio), until found. Active, for names in scan responses.
static void central_scan(void) {
    if (!synced || ble_gap_disc_active() || slots[CENTRAL_SLOT].conn != BLE_HS_CONN_HANDLE_NONE) return;
    if (!target[0] && !listing) {
        central_state = BLE_CENTRAL_OFF;
        return;
    }
    if (target[0]) central_state = BLE_CENTRAL_SCANNING;
    struct ble_gap_disc_params p = {.itvl = 160, .window = 48, .filter_duplicates = 0, .passive = 0};
    int rc = ble_gap_disc(own_addr_type, listing ? 5000 : BLE_HS_FOREVER, &p, scan_event, NULL);
    if (rc) {
        ESP_LOGW(TAG, "scan failed: %d", rc);
        retry_later();
    }
}

static int central_event(struct ble_gap_event *e, void *arg) {
    slot_t *s = &slots[CENTRAL_SLOT];
    switch (e->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (e->connect.status) {
                central_state = BLE_CENTRAL_SCANNING;
                central_fail("connection failed", e->connect.status);
                break;
            }
            slot_connected(s, e->connect.conn_handle);
            s->peer_handle = cccd = svc_end = 0;
            secured = false;
            // One ATT request at a time: the MTU first, the discovery from its answer.
            if (ble_gattc_exchange_mtu(s->conn, mtu_cb, NULL)) discover(s->conn);
            break;
        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGI(TAG, "device \"%s\" disconnected (reason 0x%x)", central_name, e->disconnect.reason);
            slot_disconnected(s);
            central_state = target[0] ? BLE_CENTRAL_SCANNING : BLE_CENTRAL_OFF;
            retry_later();
            break;
        case BLE_GAP_EVENT_NOTIFY_RX:
            if (e->notify_rx.conn_handle == s->conn && e->notify_rx.attr_handle == s->peer_handle)
                receive_packet(s, e->notify_rx.om);
            break;
        case BLE_GAP_EVENT_ENC_CHANGE:
            if (e->enc_change.status == 0 && cccd)
                subscribe(s->conn);
            else if (e->enc_change.status)
                central_fail("pairing failed", e->enc_change.status);
            break;
        case BLE_GAP_EVENT_REPEAT_PAIRING: {
            // The device lost its bond with us (reset, paired elsewhere): forget ours and pair again.
            struct ble_gap_conn_desc d;
            if (ble_gap_conn_find(e->repeat_pairing.conn_handle, &d) == 0) ble_store_util_delete_peer(&d.peer_id_addr);
            return BLE_GAP_REPEAT_PAIRING_RETRY;
        }
        case BLE_GAP_EVENT_CONN_UPDATE: {
            struct ble_gap_conn_desc d;
            if (ble_gap_conn_find(s->conn, &d) == 0) s->stats.interval_x125 = d.conn_itvl;
            ESP_LOGI(TAG, "device: interval %.2f ms", s->stats.interval_x125 * 1.25);
            break;
        }
        case BLE_GAP_EVENT_CONN_UPDATE_REQ: {
            // The device may ask for a slower connection once it is idle (macOS as a peripheral does, measured: one
            // message per ~54 ms afterwards, a backlog of seconds at 50 messages/s). BLE MIDI wants 7.5-15 ms; the
            // central has the last word, so it keeps to that.
            struct ble_gap_upd_params *p = e->conn_update_req.self_params;
            if (p->itvl_max > 12) p->itvl_max = 12;
            if (p->itvl_min > p->itvl_max) p->itvl_min = p->itvl_max;
            p->latency = 0;
            break;
        }
        case BLE_GAP_EVENT_MTU:
            s->stats.mtu = s->mtu = e->mtu.value;
            break;
    }
    return 0;
}

// ---- peripheral: centrals connecting to us (host task) --------------------------------------------------------

static void advertise(void);

static int gap_event(struct ble_gap_event *e, void *arg) {
    switch (e->type) {
        case BLE_GAP_EVENT_CONNECT: {
            if (e->connect.status) {
                advertise();
                break;
            }
            slot_t *s = free_peripheral_slot();
            if (!s) {  // cannot happen while advertising stops with both slots taken, but never index past them
                ble_gap_terminate(e->connect.conn_handle, BLE_ERR_CONN_LIMIT);
                break;
            }
            slot_connected(s, e->connect.conn_handle);
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
                slot_disconnected(s);
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
    if (!free_peripheral_slot() || ble_gap_adv_active()) return;
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
    central_scan();
}

static void on_reset(int reason) { ESP_LOGW(TAG, "host reset, reason %d", reason); }

static void host_task(void *p) {
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void ble_store_config_init(void);

void ble_midi_start(const char *name, const char *device) {
    strncpy(dev_name, name, sizeof dev_name - 1);
    strncpy(target, device, sizeof target - 1);
    for (int i = 0; i < SLOTS; i++) {
        slots[i].conn = BLE_HS_CONN_HANDLE_NONE;
        slots[i].central = i == CENTRAL_SLOT;
        bus_register_output(port_of(i), &ops, &slots[i]);
    }
    ESP_ERROR_CHECK(nimble_port_init());
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    // Pairing for devices that ask for it: "just works" (no display, no keyboard), bonded and kept in NVS.
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set(dev_name);
    ESP_ERROR_CHECK(ble_gatts_count_cfg(services));
    ESP_ERROR_CHECK(ble_gatts_add_svcs(services));
    ble_store_config_init();
    ble_npl_callout_init(&retry_timer, nimble_port_get_dflt_eventq(), on_retry, NULL);
    ble_npl_event_init(&control_ev, on_control, NULL);
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

// ---- central: control from other tasks -----------------------------------------------------------------------
// These post to the host task (the callout), so the scan state is only ever touched there.

static char next_target[SETTINGS_DEVICE_MAX + 1];
static volatile bool target_changed, list_requested;

static void on_control(struct ble_npl_event *ev) {
    if (target_changed) {
        target_changed = false;
        if (strcmp(target, next_target)) {
            strcpy(target, next_target);
            ESP_LOGI(TAG, "device: %s", target[0] ? target : "none");
            if (slots[CENTRAL_SLOT].conn != BLE_HS_CONN_HANDLE_NONE)
                ble_gap_terminate(slots[CENTRAL_SLOT].conn, BLE_ERR_REM_USER_CONN_TERM);
            else if (central_state == BLE_CENTRAL_CONNECTING)
                ble_gap_conn_cancel();
            ble_gap_disc_cancel();
            central_state = target[0] ? BLE_CENTRAL_SCANNING : BLE_CENTRAL_OFF;
        }
    }
    if (list_requested) {
        list_requested = false;
        n_seen = 0;
        if (slots[CENTRAL_SLOT].conn == BLE_HS_CONN_HANDLE_NONE && central_state != BLE_CENTRAL_CONNECTING) {
            ble_gap_disc_cancel();
            listing = true;
        }
    }
    central_scan();
}

static void post_control(void) {
    if (synced && !ble_npl_event_is_queued(&control_ev)) ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &control_ev);
}

void ble_midi_set_device(const char *device) {
    snprintf(next_target, sizeof next_target, "%s", device);
    target_changed = true;
    if (!synced) strcpy(target, next_target);  // applied at sync
    post_control();
}

void ble_midi_scan_list(void) {
    list_requested = true;
    post_control();
}

int ble_midi_found(ble_midi_found_t *out, int max) {
    int n = n_seen < max ? n_seen : max;
    memcpy(out, seen, n * sizeof *out);  // a copy: a name may be mid-update, which only shows as a stale name
    return n;
}

void ble_midi_central_info(ble_midi_central_info_t *out) {
    memset(out, 0, sizeof *out);
    out->state = central_state;
    snprintf(out->target, sizeof out->target, "%s", target);
    snprintf(out->name, sizeof out->name, "%s", central_name);
    out->listing = listing;
    out->stats = slots[CENTRAL_SLOT].stats;
}

const char *ble_midi_central_state_name(ble_midi_central_state_t s) {
    static const char *names[] = {"off", "scanning", "connecting", "connected"};
    return s < sizeof names / sizeof names[0] ? names[s] : "?";
}
