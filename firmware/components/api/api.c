#include "api.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "ble_config.h"
#include "ble_midi.h"
#include "board.h"
#include "bus.h"
#include "cJSON.h"
#include "ci.h"
#include "esp_app_desc.h"
#include "esp_err.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "espnow_midi.h"
#include "net.h"
#include "net_midi.h"
#include "nvs_settings.h"
#include "ota.h"
#include "power.h"
#include "rtp_midi.h"
#include "uart_midi.h"

static api_hooks_t hooks;

void api_init(const api_hooks_t *h) { hooks = *h; }

typedef struct {
    const char *query;  // after the '?', "" if none
    const char *body;
    size_t body_len;
} req_t;

// ---- helpers -------------------------------------------------------------------------------------------------

static api_reply_t reply(int status, cJSON *root) {
    api_reply_t r = {.status = status, .json = cJSON_PrintUnformatted(root)};
    cJSON_Delete(root);
    return r;
}

static api_reply_t ok(cJSON *root) { return reply(200, root); }

static api_reply_t error(int status, const char *message) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "error", message);
    return reply(status, root);
}

static api_reply_t empty(int status) { return reply(status, cJSON_CreateObject()); }

// A small JSON request body (settings, wifi, mode); NULL if there is none or it does not parse.
static cJSON *json_body(const req_t *q) {
    if (!q->body || !q->body_len || q->body_len > 1024) return NULL;
    return cJSON_ParseWithLength(q->body, q->body_len);
}

// One query value, URL-decoded. False if the key is missing.
static bool query_value(const char *query, const char *key, char *out, size_t len) {
    size_t kl = strlen(key);
    for (const char *p = query; p && *p;) {
        const char *end = strchr(p, '&');
        if (!end) end = p + strlen(p);
        if ((size_t)(end - p) > kl && !strncmp(p, key, kl) && p[kl] == '=') {
            size_t k = 0;
            for (const char *s = p + kl + 1; s < end && k < len - 1; s++) {
                if (*s == '%' && s + 2 < end) {  // both hex digits are inside this value
                    char hex[3] = {s[1], s[2], 0};
                    out[k++] = (char)strtol(hex, NULL, 16);
                    s += 2;
                } else {
                    out[k++] = *s == '+' ? ' ' : *s;
                }
            }
            out[k] = 0;
            return true;
        }
        p = *end ? end + 1 : end;
    }
    return false;
}

static airmidi_mode_t mode_from_name(const char *s) {
    for (int m = 0; m < AIRMIDI_MODE_COUNT; m++)
        if (s && !strcmp(s, airmidi_mode_name(m))) return m;
    return AIRMIDI_MODE_COUNT;
}

static void restart_soon(void *arg) { esp_restart(); }

static void restart_after(uint32_t ms) {
    const esp_timer_create_args_t a = {.callback = restart_soon, .name = "restart"};
    esp_timer_handle_t t;
    if (esp_timer_create(&a, &t) == ESP_OK) esp_timer_start_once(t, (uint64_t)ms * 1000);
}

// ---- /api/status ---------------------------------------------------------------------------------------------

static api_reply_t get_status(const req_t *q) {
    cJSON *r = cJSON_CreateObject();
    settings_t s;
    nvs_settings_get(&s);
    const esp_app_desc_t *app = esp_app_get_description();
    cJSON_AddStringToObject(r, "name", s.name);
    cJSON_AddStringToObject(r, "version", app->version);
    cJSON_AddStringToObject(r, "board", board()->name);
    cJSON_AddNumberToObject(r, "uptime_ms", (double)(esp_timer_get_time() / 1000));
    cJSON_AddStringToObject(r, "reset_reason", ota_reset_reason());
    cJSON_AddStringToObject(r, "mode", airmidi_mode_name(s.mode));
    cJSON_AddNumberToObject(r, "heap", esp_get_free_heap_size());

    cJSON *w = cJSON_AddObjectToObject(r, "wifi");
    char ip[16], ssid[33];
    net_ip(ip);
    net_ssid(ssid);
    cJSON_AddStringToObject(w, "state", net_state_name(net_state()));
    cJSON_AddStringToObject(w, "ip", ip);
    cJSON_AddStringToObject(w, "ssid", ssid);
    cJSON_AddNumberToObject(w, "rssi", net_rssi());

    cJSON *b = cJSON_AddObjectToObject(r, "battery");
    cJSON_AddBoolToObject(b, "usb", power_on_usb());
    cJSON_AddNumberToObject(b, "mv", power_cell_mv());
    cJSON_AddNumberToObject(b, "pct", power_percent());
    cJSON_AddBoolToObject(b, "low", power_low());

    cJSON *ble = cJSON_AddArrayToObject(r, "ble");
    for (int i = 0; i < 2; i++) {
        ble_midi_slot_stats_t st;
        ble_midi_get_stats(i, &st);
        cJSON *c = cJSON_CreateObject();
        cJSON_AddStringToObject(c, "state", !st.connected ? "idle" : st.subscribed ? "playing" : "connected");
        cJSON_AddNumberToObject(c, "interval_ms", st.connected ? st.interval_x125 * 1.25 : 0);
        cJSON_AddNumberToObject(c, "mtu", st.mtu);
        cJSON_AddItemToArray(ble, c);
    }

    ble_midi_central_info_t bc;
    ble_midi_central_info(&bc);
    cJSON *dv = cJSON_AddObjectToObject(r, "device");
    cJSON_AddStringToObject(dv, "state", ble_midi_central_state_name(bc.state));
    cJSON_AddStringToObject(dv, "target", bc.target);
    cJSON_AddStringToObject(dv, "name", bc.name);
    cJSON_AddNumberToObject(dv, "interval_ms", bc.stats.connected ? bc.stats.interval_x125 * 1.25 : 0);

    uart_midi_stats_t t;
    uart_midi_get_stats(&t);
    cJSON *trs = cJSON_AddObjectToObject(r, "trs");
    cJSON_AddNumberToObject(trs, "rx", t.rx_bytes);
    cJSON_AddNumberToObject(trs, "tx", t.tx_bytes);
    cJSON_AddNumberToObject(trs, "rx_dropped", t.rx_dropped_bytes);
    cJSON_AddNumberToObject(trs, "tx_dropped", t.tx_dropped);

    // The ports this board has (CI is internal).
    static const struct {
        int port;
        const char *name;
    } shown[] = {{PORT_TRS, "trs"},    {PORT_BLE_0, "ble1"}, {PORT_BLE_1, "ble2"},   {PORT_BLE_CENTRAL, "bledev"},
                 {PORT_NET_0, "net1"}, {PORT_NET_1, "net2"}, {PORT_RTP_0, "rtp1"},   {PORT_RTP_1, "rtp2"},
                 {PORT_WEB, "web"},    {PORT_USB, "usb"},    {PORT_ESPNOW, "espnow"}};
    cJSON *ports = cJSON_AddArrayToObject(r, "ports");
    for (size_t i = 0; i < sizeof shown / sizeof shown[0]; i++) {
        bus_port_stats_t ps;
        bus_get_stats(shown[i].port, &ps);
        if (shown[i].port == PORT_WEB && hooks.web_dropped) ps.dropped += hooks.web_dropped();
        cJSON *p = cJSON_CreateObject();
        cJSON_AddStringToObject(p, "name", shown[i].name);
        cJSON_AddNumberToObject(p, "in", ps.in);
        cJSON_AddNumberToObject(p, "out", ps.out);
        cJSON_AddNumberToObject(p, "dropped", ps.dropped);
        cJSON_AddNumberToObject(p, "coalesced", ps.coalesced);
        cJSON_AddBoolToObject(p, "up", bus_is_up(shown[i].port));
        cJSON_AddItemToArray(ports, p);
    }

    cJSON *net = cJSON_AddArrayToObject(r, "net");
    for (int i = 0; i < NM2_SESSIONS; i++) {
        net_midi_session_info_t ni;
        net_midi_get_info(i, &ni);
        cJSON *c = cJSON_CreateObject();
        cJSON_AddBoolToObject(c, "active", ni.active);
        cJSON_AddStringToObject(c, "peer", ni.peer);
        cJSON_AddStringToObject(c, "ip", ni.ip);
        cJSON_AddNumberToObject(c, "protocol", ni.protocol);
        cJSON_AddNumberToObject(c, "rx", ni.stats.rx_cmds);
        cJSON_AddNumberToObject(c, "lost", ni.stats.rx_lost);
        cJSON_AddNumberToObject(c, "recovered", ni.stats.rx_recovered);
        cJSON_AddNumberToObject(c, "tx", ni.stats.tx_cmds);
        cJSON_AddNumberToObject(c, "retransmitted", ni.stats.tx_retransmitted);
        cJSON_AddItemToArray(net, c);
    }

    espnow_midi_info_t en;
    espnow_midi_get_info(&en);
    cJSON *ej = cJSON_AddObjectToObject(r, "espnow");
    cJSON_AddBoolToObject(ej, "on", en.running);
    cJSON_AddNumberToObject(ej, "channel", en.channel);
    cJSON *peers = cJSON_AddArrayToObject(ej, "peers");
    for (int i = 0; i < en.n_peers; i++) cJSON_AddItemToArray(peers, cJSON_CreateString(en.peers[i]));
    cJSON_AddNumberToObject(ej, "rx", en.stats.rx_frames);
    cJSON_AddNumberToObject(ej, "lost", en.stats.rx_lost);
    cJSON_AddNumberToObject(ej, "dups", en.stats.rx_dups);
    cJSON_AddNumberToObject(ej, "tx", en.stats.tx_frames);
    cJSON_AddNumberToObject(ej, "hellos", en.stats.rx_hellos);
    cJSON_AddNumberToObject(ej, "bad", en.stats.rx_bad);

    cJSON *rtp = cJSON_AddArrayToObject(r, "rtp");
    for (int i = 0; i < RTPMIDI_SESSIONS; i++) {
        rtp_midi_session_info_t ri;
        rtp_midi_get_info(i, &ri);
        cJSON *c = cJSON_CreateObject();
        cJSON_AddBoolToObject(c, "active", ri.active);
        cJSON_AddStringToObject(c, "peer", ri.peer);
        cJSON_AddStringToObject(c, "ip", ri.ip);
        cJSON_AddNumberToObject(c, "rx", ri.stats.rx_packets);
        cJSON_AddNumberToObject(c, "lost", ri.stats.rx_lost);
        cJSON_AddNumberToObject(c, "repairs", ri.stats.rx_repairs);
        cJSON_AddBoolToObject(c, "resumed", ri.resumed);
        cJSON_AddBoolToObject(c, "syncing", ri.syncing);
        cJSON_AddNumberToObject(c, "bad", ri.stats.rx_bad);
        cJSON_AddNumberToObject(c, "late", ri.stats.rx_late);
        cJSON_AddNumberToObject(c, "tx", ri.stats.tx_packets);
        cJSON_AddItemToArray(rtp, c);
    }

    ci_status_t cs;
    ci_get_status(&cs);
    cJSON *ci = cJSON_AddObjectToObject(r, "ci");
    cJSON_AddBoolToObject(ci, "scanning", cs.scanning);
    cJSON_AddStringToObject(ci, "result", cs.result);
    cJSON_AddStringToObject(ci, "device", cs.device);
    cJSON_AddStringToObject(ci, "source", cs.source);

    ota_info_t oi;
    ota_get_info(&oi);
    ota_coredump_t cd;
    ota_coredump_info(&cd);
    cJSON *o = cJSON_AddObjectToObject(r, "ota");
    cJSON_AddStringToObject(o, "running", oi.running);
    cJSON_AddStringToObject(o, "state", oi.state);
    cJSON_AddStringToObject(o, "rolled_back", oi.rolled_back);
    ota_fetch_status_t fs;
    ota_fetch_status(&fs);
    cJSON *f = cJSON_AddObjectToObject(o, "fetch");
    cJSON_AddStringToObject(f, "state", fs.state);
    cJSON_AddNumberToObject(f, "percent", fs.percent);
    cJSON_AddStringToObject(f, "error", fs.error);
    cJSON *c = cJSON_AddObjectToObject(o, "coredump");
    cJSON_AddBoolToObject(c, "present", cd.present);
    cJSON_AddNumberToObject(c, "size", cd.size);
    cJSON_AddStringToObject(c, "task", cd.task);
    return ok(r);
}

// ---- /api/ble: BLE MIDI devices around, for choosing the one to connect to ----------------------------------

static api_reply_t get_ble(const req_t *q) {
    ble_midi_central_info_t ci;
    ble_midi_central_info(&ci);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "state", ble_midi_central_state_name(ci.state));
    cJSON_AddStringToObject(r, "target", ci.target);
    cJSON_AddStringToObject(r, "name", ci.name);
    cJSON_AddBoolToObject(r, "scanning", ci.listing);
    cJSON *list = cJSON_AddArrayToObject(r, "found");
    ble_midi_found_t f[BLE_MIDI_FOUND_MAX];
    int n = ble_midi_found(f, BLE_MIDI_FOUND_MAX);
    for (int i = 0; i < n; i++) {
        cJSON *d = cJSON_CreateObject();
        cJSON_AddStringToObject(d, "name", f[i].name);
        cJSON_AddStringToObject(d, "address", f[i].address);
        cJSON_AddNumberToObject(d, "rssi", f[i].rssi);
        cJSON_AddItemToArray(list, d);
    }
    return ok(r);
}

static api_reply_t post_ble_scan(const req_t *q) {
    if (!ble_midi_ready()) return error(409, "Bluetooth is off in this mode");
    ble_midi_scan_list();
    return empty(200);
}

// ---- /api/settings, /api/mode --------------------------------------------------------------------------------

static cJSON *settings_json(const settings_t *s) {
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "name", s->name);
    cJSON_AddStringToObject(r, "mode", airmidi_mode_name(s->mode));
    cJSON_AddNumberToObject(r, "sysex_timeout_ms", s->sysex_timeout_ms);
    cJSON_AddBoolToObject(r, "ble_in_network", s->ble_in_network);
    cJSON_AddStringToObject(r, "ble_device", s->ble_device);
    cJSON_AddBoolToObject(r, "espnow", s->espnow);
    cJSON_AddStringToObject(r, "espnow_group", s->espnow_group);
    return r;
}

static api_reply_t get_settings(const req_t *q) {
    settings_t s;
    nvs_settings_get(&s);
    return ok(settings_json(&s));
}

// Takes the full settings object; changing the name restarts the box (BLE name, mDNS host, access point).
static api_reply_t post_settings(const req_t *q) {
    cJSON *in = json_body(q);
    if (!in) return error(400, "expected a JSON object");
    settings_t old, s;
    nvs_settings_get(&old);
    s = old;
    const cJSON *name = cJSON_GetObjectItem(in, "name");
    const cJSON *timeout = cJSON_GetObjectItem(in, "sysex_timeout_ms");
    if (cJSON_IsString(name)) {
        memset(s.name, 0, sizeof s.name);
        strncpy(s.name, name->valuestring, sizeof s.name - 1);
        if (strlen(name->valuestring) > SETTINGS_NAME_MAX) s.name[0] = 0;  // fails the check below
    }
    if (cJSON_IsNumber(timeout)) s.sysex_timeout_ms = timeout->valuedouble > 65535 ? 0 : timeout->valueint;
    const cJSON *ble_net = cJSON_GetObjectItem(in, "ble_in_network");
    if (cJSON_IsBool(ble_net)) s.ble_in_network = cJSON_IsTrue(ble_net);
    const cJSON *dev = cJSON_GetObjectItem(in, "ble_device");
    if (cJSON_IsString(dev)) {
        memset(s.ble_device, 0, sizeof s.ble_device);
        strncpy(s.ble_device, dev->valuestring, sizeof s.ble_device - 1);
        if (strlen(dev->valuestring) > SETTINGS_DEVICE_MAX) s.ble_device[0] = 1;  // fails the check below
    }
    const cJSON *espnow = cJSON_GetObjectItem(in, "espnow");
    if (cJSON_IsBool(espnow)) s.espnow = cJSON_IsTrue(espnow);
    const cJSON *group = cJSON_GetObjectItem(in, "espnow_group");
    if (cJSON_IsString(group)) {
        memset(s.espnow_group, 0, sizeof s.espnow_group);
        strncpy(s.espnow_group, group->valuestring, sizeof s.espnow_group - 1);
        if (strlen(group->valuestring) > SETTINGS_GROUP_MAX) s.espnow_group[0] = 0;  // fails the check below
    }
    cJSON_Delete(in);
    const char *why = settings_check(&s);
    if (why) return error(400, why);
    if (nvs_settings_set(&s) != ESP_OK) return error(500, "could not store");
    if (strcmp(s.ble_device, old.ble_device)) ble_midi_set_device(s.ble_device);  // applies without a restart
    bool restart = strcmp(s.name, old.name) != 0 || s.sysex_timeout_ms != old.sysex_timeout_ms ||
                   s.espnow != old.espnow || strcmp(s.espnow_group, old.espnow_group) ||
                   (s.mode == AIRMIDI_MODE_NETWORK && s.ble_in_network != old.ble_in_network);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddItemToObject(r, "settings", settings_json(&s));
    cJSON_AddBoolToObject(r, "restart", restart);
    if (restart) restart_after(500);  // the reply goes out first
    return ok(r);
}

static void switch_mode(int m) { hooks.set_mode((airmidi_mode_t)m); }

static api_reply_t post_mode(const req_t *q) {
    cJSON *in = json_body(q);
    airmidi_mode_t m = mode_from_name(in ? cJSON_GetStringValue(cJSON_GetObjectItem(in, "mode")) : NULL);
    cJSON_Delete(in);
    if (m == AIRMIDI_MODE_COUNT) return error(400, "mode is play, network or setup");
    api_reply_t r = empty(200);
    r.then = switch_mode;  // after the answer: the network it travels on may go away
    r.then_arg = m;
    return r;
}

// ---- /api/wifi -----------------------------------------------------------------------------------------------

static api_reply_t get_wifi(const req_t *q) {
    char ssids[NET_MAX_NETWORKS][33];
    int n = net_networks(ssids, NET_MAX_NETWORKS);
    cJSON *r = cJSON_CreateObject();
    cJSON *a = cJSON_AddArrayToObject(r, "networks");
    for (int i = 0; i < n; i++) cJSON_AddItemToArray(a, cJSON_CreateString(ssids[i]));
    return ok(r);
}

static api_reply_t post_wifi(const req_t *q) {
    cJSON *in = json_body(q);
    const char *ssid = in ? cJSON_GetStringValue(cJSON_GetObjectItem(in, "ssid")) : NULL;
    const char *pass = in ? cJSON_GetStringValue(cJSON_GetObjectItem(in, "pass")) : NULL;
    esp_err_t e = ssid ? net_add_network(ssid, pass ? pass : "") : ESP_ERR_INVALID_ARG;
    cJSON_Delete(in);
    if (e == ESP_ERR_INVALID_ARG) return error(400, "network name 1-32, password 0-64 bytes");
    if (e) return error(500, esp_err_to_name(e));
    return get_wifi(q);
}

static api_reply_t delete_wifi(const req_t *q) {
    char ssid[100];
    if (!query_value(q->query, "ssid", ssid, sizeof ssid)) return error(400, "?ssid= missing");
    if (net_remove_network(ssid) != ESP_OK) return error(404, "no such network");
    return get_wifi(q);
}

// ---- /api/wifi/scan, /api/wifi/try: setup from a phone (net.h) ----------------------------------------------

static api_reply_t wifi_busy_error(esp_err_t e) {
    if (e == ESP_ERR_INVALID_STATE)
        return error(409, net_state() == NET_JOINING || net_state() == NET_CONNECTED
                              ? "the box is in Network mode: switch to Play or Setup to scan or try a network"
                              : "a scan or a try is already running");
    return error(500, esp_err_to_name(e));
}

static api_reply_t post_wifi_scan(const req_t *q) {
    esp_err_t e = net_scan_start();
    return e ? wifi_busy_error(e) : empty(202);
}

static void add_probe(cJSON *r) {
    net_probe_t p;
    net_probe(&p);
    cJSON_AddStringToObject(r, "state", net_probe_state_name(p.state));
    cJSON_AddStringToObject(r, "ssid", p.ssid);
    cJSON_AddStringToObject(r, "ip", p.ip);
    cJSON_AddStringToObject(r, "reason", p.reason);
}

static api_reply_t get_wifi_scan(const req_t *q) {
    net_found_t f[NET_SCAN_MAX];
    int n = net_scan_results(f, NET_SCAN_MAX);
    cJSON *r = cJSON_CreateObject();
    add_probe(r);
    cJSON *a = cJSON_AddArrayToObject(r, "networks");
    for (int i = 0; i < n; i++) {
        cJSON *c = cJSON_CreateObject();
        cJSON_AddStringToObject(c, "ssid", f[i].ssid);
        cJSON_AddNumberToObject(c, "rssi", f[i].rssi);
        cJSON_AddBoolToObject(c, "secure", f[i].secure);
        cJSON_AddItemToArray(a, c);
    }
    return ok(r);
}

// {"ssid", "pass"}: join once; on success the network is stored. Poll GET /api/wifi/try for the outcome.
static api_reply_t post_wifi_try(const req_t *q) {
    cJSON *in = json_body(q);
    const char *ssid = in ? cJSON_GetStringValue(cJSON_GetObjectItem(in, "ssid")) : NULL;
    const char *pass = in ? cJSON_GetStringValue(cJSON_GetObjectItem(in, "pass")) : NULL;
    esp_err_t e = net_try_network(ssid, pass ? pass : "");
    cJSON_Delete(in);
    if (e == ESP_ERR_INVALID_ARG) return error(400, "network name 1-32, password 0-64 bytes");
    return e ? wifi_busy_error(e) : empty(202);
}

static api_reply_t get_wifi_try(const req_t *q) {
    cJSON *r = cJSON_CreateObject();
    add_probe(r);
    return ok(r);
}

// ---- /api/phones: the phones the BLE config service trusts (ble_config.h) ----------------------------------------

static api_reply_t get_phones(const req_t *q) {
    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "trusted", ble_config_phone_count());
    cJSON_AddNumberToObject(r, "max", BLE_CONFIG_PHONES);
    uint32_t left = ble_config_window_left();
    cJSON_AddStringToObject(r, "window", !left ? "closed" : left == BLE_CONFIG_WINDOW_FOREVER ? "setup" : "open");
    cJSON_AddNumberToObject(r, "window_s", left == BLE_CONFIG_WINDOW_FOREVER ? -1 : (double)left);
    return ok(r);
}

// Open the window for another phone (2 min): asked by a phone already trusted, or by the web page.
static api_reply_t post_phones_window(const req_t *q) {
    if (ble_config_window_left() != BLE_CONFIG_WINDOW_FOREVER) ble_config_set_window(120);
    return get_phones(q);
}

static api_reply_t delete_phones(const req_t *q) {
    ble_config_forget_phones();
    return get_phones(q);
}

// ---- /api/update (check, install), /api/coredump -------------------------------------------------------------

// {"base": "..."} optional: where to look instead of the configured release URL (tests use a local server).
static void read_base(const req_t *q, char *base, size_t len) {
    base[0] = 0;
    cJSON *in = json_body(q);
    const char *b = in ? cJSON_GetStringValue(cJSON_GetObjectItem(in, "base")) : NULL;
    if (b) snprintf(base, len, "%s", b);
    cJSON_Delete(in);
}

static api_reply_t post_update_check(const req_t *q) {
    char base[160], err[64] = "";
    read_base(q, base, sizeof base);
    ota_manifest_t m;
    if (ota_check(base, &m, err, sizeof err) != ESP_OK) return error(502, err);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "current", esp_app_get_description()->version);
    cJSON_AddStringToObject(r, "latest", m.version);
    cJSON_AddBoolToObject(r, "newer", m.newer);
    return ok(r);
}

static api_reply_t post_update_install(const req_t *q) {
    char base[160];
    read_base(q, base, sizeof base);
    esp_err_t e = ota_install_start(base);
    if (e == ESP_ERR_INVALID_STATE) return error(409, "a download is already running");
    if (e) return error(500, esp_err_to_name(e));
    return empty(202);
}

static api_reply_t delete_coredump(const req_t *q) {
    ota_coredump_erase();
    return empty(200);
}

// ---- /api/ci: MIDI-CI ----------------------------------------------------------------------------------------

static api_reply_t get_ci(const req_t *q) {
    ci_status_t cs;
    ci_get_status(&cs);
    cJSON *r = cJSON_CreateObject();
    char muid[8];
    cJSON_AddBoolToObject(r, "scanning", cs.scanning);
    cJSON_AddStringToObject(r, "result", cs.result);
    cJSON_AddNumberToObject(r, "scan_ms", cs.scan_ms);
    snprintf(muid, sizeof muid, "%07lX", (unsigned long)cs.muid);
    cJSON_AddStringToObject(r, "muid", muid);
    snprintf(muid, sizeof muid, "%07lX", (unsigned long)cs.device_muid);
    cJSON_AddStringToObject(r, "device_muid", cs.device[0] ? muid : "");
    cJSON_AddStringToObject(r, "device", cs.device);
    cJSON_AddStringToObject(r, "source", cs.source);
    cJSON_AddNumberToObject(r, "dropped", cs.dropped);
    return ok(r);
}

typedef struct {
    char *buf;
    size_t len, cap;
} text_t;

static bool text_sink(void *ctx, const char *s, size_t n) {
    text_t *t = ctx;
    if (t->len + n + 1 > t->cap) {
        size_t cap = t->cap ? t->cap : 2048;
        while (cap < t->len + n + 1) cap *= 2;
        char *b = realloc(t->buf, cap);
        if (!b) return false;
        t->buf = b, t->cap = cap;
    }
    memcpy(t->buf + t->len, s, n);
    t->len += n;
    t->buf[t->len] = 0;
    return true;
}

// {"info": DeviceInfo, "controls": AllCtrlList, "maps": {id: CtrlMapList}, "profile": the uploaded profile}
static api_reply_t get_ci_device(const req_t *q) {
    text_t t = {0};
    if (!ci_write_device(text_sink, &t)) {
        free(t.buf);
        return error(500, "out of memory");
    }
    return (api_reply_t){.status = 200, .json = t.buf ? t.buf : strdup("{}")};
}

static api_reply_t post_ci_scan(const req_t *q) {
    cJSON *in = json_body(q);
    ci_scan(cJSON_IsTrue(cJSON_GetObjectItem(in, "force")));
    cJSON_Delete(in);
    return empty(202);
}

static api_reply_t post_ci_profile(const req_t *q) {
    if (!q->body_len || q->body_len > API_BODY_MAX) return error(413, "16 KB at most");
    esp_err_t e = ci_set_profile(q->body, q->body_len);
    if (e == ESP_ERR_INVALID_ARG) return error(400, "expected a control list: [...] or {\"controls\": [...]}");
    if (e) return error(500, esp_err_to_name(e));
    return empty(200);
}

static api_reply_t delete_ci_profile(const req_t *q) {
    ci_set_profile(NULL, 0);
    return empty(200);
}

// ---- routing -------------------------------------------------------------------------------------------------

static const struct {
    const char *method, *path;
    api_reply_t (*fn)(const req_t *q);
} routes[] = {
    {"GET", "/api/status", get_status},
    {"GET", "/api/settings", get_settings},
    {"POST", "/api/settings", post_settings},
    {"POST", "/api/mode", post_mode},
    {"GET", "/api/wifi", get_wifi},
    {"POST", "/api/wifi", post_wifi},
    {"DELETE", "/api/wifi", delete_wifi},
    {"POST", "/api/wifi/scan", post_wifi_scan},
    {"GET", "/api/wifi/scan", get_wifi_scan},
    {"POST", "/api/wifi/try", post_wifi_try},
    {"GET", "/api/wifi/try", get_wifi_try},
    {"POST", "/api/update/check", post_update_check},
    {"POST", "/api/update/install", post_update_install},
    {"DELETE", "/api/coredump", delete_coredump},
    {"GET", "/api/phones", get_phones},
    {"POST", "/api/phones/window", post_phones_window},
    {"DELETE", "/api/phones", delete_phones},
    {"GET", "/api/ble", get_ble},
    {"POST", "/api/ble/scan", post_ble_scan},
    {"GET", "/api/ci", get_ci},
    {"GET", "/api/ci/device", get_ci_device},
    {"POST", "/api/ci/scan", post_ci_scan},
    {"POST", "/api/ci/profile", post_ci_profile},
    {"DELETE", "/api/ci/profile", delete_ci_profile},
};

api_reply_t api_call(const char *method, const char *path, const char *body, size_t body_len) {
    const char *qm = strchr(path, '?');
    size_t plen = qm ? (size_t)(qm - path) : strlen(path);
    req_t q = {.query = qm ? qm + 1 : "", .body = body, .body_len = body_len};
    bool path_known = false;
    for (size_t i = 0; i < sizeof routes / sizeof routes[0]; i++) {
        if (strlen(routes[i].path) != plen || strncmp(routes[i].path, path, plen)) continue;
        path_known = true;
        if (!strcmp(routes[i].method, method)) return routes[i].fn(&q);
    }
    return path_known ? error(405, "method not allowed") : error(404, "no such endpoint");
}

const char *api_status_line(int status) {
    switch (status) {
        case 200: return "200 OK";
        case 202: return "202 Accepted";
        case 400: return "400 Bad Request";
        case 403: return "403 Forbidden";
        case 404: return "404 Not Found";
        case 405: return "405 Method Not Allowed";
        case 409: return "409 Conflict";
        case 413: return "413 Payload Too Large";
        case 502: return "502 Bad Gateway";
        default: return "500 Internal Server Error";
    }
}
