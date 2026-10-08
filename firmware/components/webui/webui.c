#include "webui.h"

#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "ble_midi.h"
#include "board.h"
#include "bus.h"
#include "cJSON.h"
#include "ci.h"
#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net.h"
#include "net_midi.h"
#include "nvs_settings.h"
#include "ota.h"
#include "power.h"
#include "uart_midi.h"

static const char *TAG = "web";

static webui_hooks_t hooks;
static httpd_handle_t server;

// ---- static files (gzipped at build time, see CMakeLists.txt) -----------------------------------------------

#define EMBEDDED(sym)                                                 \
    extern const uint8_t sym##_start[] asm("_binary_" #sym "_start"); \
    extern const uint8_t sym##_end[] asm("_binary_" #sym "_end");

EMBEDDED(index_html_gz)
EMBEDDED(app_js_gz)
EMBEDDED(style_css_gz)
EMBEDDED(geist_medium_woff2_gz)

typedef struct {
    const char *path, *type;
    const uint8_t *start, *end;
} static_file_t;

static const static_file_t files[] = {
    {"/", "text/html", index_html_gz_start, index_html_gz_end},
    {"/app.js", "text/javascript", app_js_gz_start, app_js_gz_end},
    {"/style.css", "text/css", style_css_gz_start, style_css_gz_end},
    {"/geist-medium.woff2", "font/woff2", geist_medium_woff2_gz_start, geist_medium_woff2_gz_end},
};

static esp_err_t serve_file(httpd_req_t *req) {
    const static_file_t *f = req->user_ctx;
    httpd_resp_set_type(req, f->type);
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    // The files are part of the firmware: they change only with an update, which restarts the box.
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, (const char *)f->start, f->end - f->start);
}

// ---- helpers -------------------------------------------------------------------------------------------------

static esp_err_t send_json(httpd_req_t *req, cJSON *root) {
    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    esp_err_t e = httpd_resp_sendstr(req, text);
    free(text);
    return e;
}

static esp_err_t send_error(httpd_req_t *req, const char *status, const char *message) {
    httpd_resp_set_status(req, status);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "error", message);
    return send_json(req, root);
}

// Read a small JSON request body (settings, wifi, mode).
static cJSON *read_json(httpd_req_t *req) {
    if (req->content_len <= 0 || req->content_len > 1024) return NULL;
    char buf[1025];
    int got = 0;
    while (got < req->content_len) {
        int n = httpd_req_recv(req, buf + got, req->content_len - got);
        if (n <= 0) return NULL;
        got += n;
    }
    buf[got] = 0;
    return cJSON_Parse(buf);
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

static volatile uint32_t ws_dropped;  // words for the pages that did not fit (one that cannot keep up)

// ---- /api/status ---------------------------------------------------------------------------------------------

static esp_err_t get_status(httpd_req_t *req) {
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

    uart_midi_stats_t t;
    uart_midi_get_stats(&t);
    cJSON *trs = cJSON_AddObjectToObject(r, "trs");
    cJSON_AddNumberToObject(trs, "rx", t.rx_bytes);
    cJSON_AddNumberToObject(trs, "tx", t.tx_bytes);
    cJSON_AddNumberToObject(trs, "rx_dropped", t.rx_dropped_bytes);
    cJSON_AddNumberToObject(trs, "tx_dropped", t.tx_dropped);

    // The ports this firmware has so far (net, usb and espnow come in later phases).
    static const struct {
        int port;
        const char *name;
    } shown[] = {{PORT_TRS, "trs"},    {PORT_BLE_0, "ble1"}, {PORT_BLE_1, "ble2"},
                 {PORT_NET_0, "net1"}, {PORT_NET_1, "net2"}, {PORT_WEB, "web"}};
    cJSON *ports = cJSON_AddArrayToObject(r, "ports");
    for (size_t i = 0; i < sizeof shown / sizeof shown[0]; i++) {
        bus_port_stats_t ps;
        bus_get_stats(shown[i].port, &ps);
        if (shown[i].port == PORT_WEB) ps.dropped += ws_dropped;
        cJSON *p = cJSON_CreateObject();
        cJSON_AddStringToObject(p, "name", shown[i].name);
        cJSON_AddNumberToObject(p, "in", ps.in);
        cJSON_AddNumberToObject(p, "out", ps.out);
        cJSON_AddNumberToObject(p, "dropped", ps.dropped);
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
    return send_json(req, r);
}

// ---- /api/settings, /api/mode --------------------------------------------------------------------------------

static cJSON *settings_json(const settings_t *s) {
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "name", s->name);
    cJSON_AddStringToObject(r, "mode", airmidi_mode_name(s->mode));
    cJSON_AddNumberToObject(r, "sysex_timeout_ms", s->sysex_timeout_ms);
    cJSON_AddBoolToObject(r, "ble_in_network", s->ble_in_network);
    return r;
}

static esp_err_t get_settings(httpd_req_t *req) {
    settings_t s;
    nvs_settings_get(&s);
    return send_json(req, settings_json(&s));
}

// Takes the full settings object; changing the name restarts the box (BLE name, mDNS host, access point).
static esp_err_t post_settings(httpd_req_t *req) {
    cJSON *in = read_json(req);
    if (!in) return send_error(req, "400 Bad Request", "expected a JSON object");
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
    cJSON_Delete(in);
    const char *why = settings_check(&s);
    if (why) return send_error(req, "400 Bad Request", why);
    if (nvs_settings_set(&s) != ESP_OK) return send_error(req, "500 Internal Server Error", "could not store");
    bool restart = strcmp(s.name, old.name) != 0 || s.sysex_timeout_ms != old.sysex_timeout_ms ||
                   (s.mode == AIRMIDI_MODE_NETWORK && s.ble_in_network != old.ble_in_network);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddItemToObject(r, "settings", settings_json(&s));
    cJSON_AddBoolToObject(r, "restart", restart);
    if (restart) restart_after(500);  // the reply goes out first
    return send_json(req, r);
}

static esp_err_t post_mode(httpd_req_t *req) {
    cJSON *in = read_json(req);
    airmidi_mode_t m = mode_from_name(in ? cJSON_GetStringValue(cJSON_GetObjectItem(in, "mode")) : NULL);
    cJSON_Delete(in);
    if (m == AIRMIDI_MODE_COUNT) return send_error(req, "400 Bad Request", "mode is play, network or setup");
    httpd_resp_sendstr(req, "{}");  // answer before the network goes away
    hooks.set_mode(m);
    return ESP_OK;
}

// ---- /api/wifi -----------------------------------------------------------------------------------------------

static esp_err_t get_wifi(httpd_req_t *req) {
    char ssids[NET_MAX_NETWORKS][33];
    int n = net_networks(ssids, NET_MAX_NETWORKS);
    cJSON *r = cJSON_CreateObject();
    cJSON *a = cJSON_AddArrayToObject(r, "networks");
    for (int i = 0; i < n; i++) cJSON_AddItemToArray(a, cJSON_CreateString(ssids[i]));
    return send_json(req, r);
}

static esp_err_t post_wifi(httpd_req_t *req) {
    cJSON *in = read_json(req);
    const char *ssid = in ? cJSON_GetStringValue(cJSON_GetObjectItem(in, "ssid")) : NULL;
    const char *pass = in ? cJSON_GetStringValue(cJSON_GetObjectItem(in, "pass")) : NULL;
    esp_err_t e = ssid ? net_add_network(ssid, pass ? pass : "") : ESP_ERR_INVALID_ARG;
    cJSON_Delete(in);
    if (e == ESP_ERR_INVALID_ARG) return send_error(req, "400 Bad Request", "network name 1-32, password 0-64 bytes");
    if (e) return send_error(req, "500 Internal Server Error", esp_err_to_name(e));
    return get_wifi(req);
}

static esp_err_t delete_wifi(httpd_req_t *req) {
    char query[128], ssid[100];
    if (httpd_req_get_url_query_str(req, query, sizeof query) != ESP_OK ||
        httpd_query_key_value(query, "ssid", ssid, sizeof ssid) != ESP_OK)
        return send_error(req, "400 Bad Request", "?ssid= missing");
    // query values arrive URL-encoded
    char plain[sizeof ssid];
    size_t k = 0;
    for (size_t i = 0; ssid[i] && k < sizeof plain - 1; i++) {
        if (ssid[i] == '%' && ssid[i + 1] && ssid[i + 2]) {
            char hex[3] = {ssid[i + 1], ssid[i + 2], 0};
            plain[k++] = (char)strtol(hex, NULL, 16);
            i += 2;
        } else {
            plain[k++] = ssid[i] == '+' ? ' ' : ssid[i];
        }
    }
    plain[k] = 0;
    if (net_remove_network(plain) != ESP_OK) return send_error(req, "404 Not Found", "no such network");
    return get_wifi(req);
}

// ---- /api/update, /api/coredump ------------------------------------------------------------------------------

// The image streams straight into the other OTA slot; nothing larger than 4 KB is held in RAM.
static esp_err_t post_update(httpd_req_t *req) {
    if (req->content_len <= 0) return send_error(req, "400 Bad Request", "empty upload");
    esp_err_t e = ota_begin();
    if (e) return send_error(req, "500 Internal Server Error", esp_err_to_name(e));
    static char buf[4096];
    int left = req->content_len;
    while (left > 0) {
        int n = httpd_req_recv(req, buf, left < (int)sizeof buf ? left : (int)sizeof buf);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (n <= 0 || (e = ota_write(buf, n)) != ESP_OK) {
            ota_abort();
            return send_error(req, "400 Bad Request", n <= 0 ? "upload interrupted" : esp_err_to_name(e));
        }
        left -= n;
    }
    e = ota_end();
    if (e == ESP_ERR_OTA_VALIDATE_FAILED)
        return send_error(req, "400 Bad Request", "not a valid image for this board, or not signed with our key");
    if (e) return send_error(req, "500 Internal Server Error", esp_err_to_name(e));
    httpd_resp_sendstr(req, "{\"ok\":true}");
    restart_after(500);
    return ESP_OK;
}

// {"base": "..."} optional: where to look instead of the configured release URL (tests use a local server).
static void read_base(httpd_req_t *req, char *base, size_t len) {
    base[0] = 0;
    cJSON *in = read_json(req);
    const char *b = in ? cJSON_GetStringValue(cJSON_GetObjectItem(in, "base")) : NULL;
    if (b) snprintf(base, len, "%s", b);
    cJSON_Delete(in);
}

static esp_err_t post_update_check(httpd_req_t *req) {
    char base[160], err[64] = "";
    read_base(req, base, sizeof base);
    ota_manifest_t m;
    if (ota_check(base, &m, err, sizeof err) != ESP_OK) return send_error(req, "502 Bad Gateway", err);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "current", esp_app_get_description()->version);
    cJSON_AddStringToObject(r, "latest", m.version);
    cJSON_AddBoolToObject(r, "newer", m.newer);
    return send_json(req, r);
}

static esp_err_t post_update_install(httpd_req_t *req) {
    char base[160];
    read_base(req, base, sizeof base);
    esp_err_t e = ota_install_start(base);
    if (e == ESP_ERR_INVALID_STATE) return send_error(req, "409 Conflict", "a download is already running");
    if (e) return send_error(req, "500 Internal Server Error", esp_err_to_name(e));
    httpd_resp_set_status(req, "202 Accepted");
    return httpd_resp_sendstr(req, "{}");
}

static bool chunk_sink(void *ctx, const void *data, size_t len) {
    return httpd_resp_send_chunk(ctx, data, len) == ESP_OK;
}

static esp_err_t get_coredump(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"airmidi-coredump.elf\"");
    if (!ota_coredump_read(chunk_sink, req)) return send_error(req, "404 Not Found", "no core dump");
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t delete_coredump(httpd_req_t *req) {
    ota_coredump_erase();
    return httpd_resp_sendstr(req, "{}");
}

// ---- /api/ci: MIDI-CI ----------------------------------------------------------------------------------------

static esp_err_t get_ci(httpd_req_t *req) {
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
    return send_json(req, r);
}

static bool send_sink(void *ctx, const char *text, size_t len) { return httpd_resp_send(ctx, text, len) == ESP_OK; }

// {"info": DeviceInfo, "controls": AllCtrlList, "maps": {id: CtrlMapList}, "profile": the uploaded profile}
static esp_err_t get_ci_device(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    if (!ci_write_device(send_sink, req)) return send_error(req, "500 Internal Server Error", "out of memory");
    return ESP_OK;
}

static esp_err_t post_ci_scan(httpd_req_t *req) {
    cJSON *in = read_json(req);
    ci_scan(cJSON_IsTrue(cJSON_GetObjectItem(in, "force")));
    cJSON_Delete(in);
    httpd_resp_set_status(req, "202 Accepted");
    return httpd_resp_sendstr(req, "{}");
}

static esp_err_t post_ci_profile(httpd_req_t *req) {
    if (req->content_len <= 0 || req->content_len > 16384)
        return send_error(req, "413 Payload Too Large", "16 KB at most");
    char *buf = malloc(req->content_len);
    if (!buf) return send_error(req, "500 Internal Server Error", "out of memory");
    int got = 0;
    while (got < req->content_len) {
        int n = httpd_req_recv(req, buf + got, req->content_len - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (n <= 0) break;
        got += n;
    }
    esp_err_t e = got == req->content_len ? ci_set_profile(buf, got) : ESP_FAIL;
    free(buf);
    if (e == ESP_ERR_INVALID_ARG)
        return send_error(req, "400 Bad Request", "expected a control list: [...] or {\"controls\": [...]}");
    if (e) return send_error(req, "500 Internal Server Error", esp_err_to_name(e));
    return httpd_resp_sendstr(req, "{}");
}

static esp_err_t delete_ci_profile(httpd_req_t *req) {
    ci_set_profile(NULL, 0);
    return httpd_resp_sendstr(req, "{}");
}

// ---- /ws: the control surface's MIDI, as UMP ----------------------------------------------------------------
// Binary frames, each a run of UMP words, big-endian. From the browser they go on the bus as PORT_WEB; what the
// bus routes to PORT_WEB collects in ws_out and goes to every connected page from the server's task, one frame
// with everything collected since the last one. At most one send is queued at a time: the server's work queue is
// a UDP socket with a 6-message mailbox, and queuing a frame per bus batch overflowed it as soon as SysEx came in
// from TRS (a batch every 2 ms), losing frames.

#define WS_CLIENTS 4
static int ws_fds[WS_CLIENTS];
static int n_ws;
static portMUX_TYPE ws_lock = portMUX_INITIALIZER_UNLOCKED;  // clients, ws_out, ws_queued: bus and httpd tasks
static uint32_t ws_out[1024];                                // a 788-byte control list is ~260 words
static int n_ws_out;
static bool ws_queued;  // a send is waiting in the server's queue

static void ws_add(int fd) {
    bool first = false, known = false;
    taskENTER_CRITICAL(&ws_lock);
    for (int i = 0; i < n_ws; i++) known |= ws_fds[i] == fd;
    if (!known && n_ws < WS_CLIENTS) first = n_ws == 0, ws_fds[n_ws++] = fd;
    taskEXIT_CRITICAL(&ws_lock);
    if (first) bus_set_up(PORT_WEB, true);
}

// ESP-IDF 5.5 does not call the URI handler for the handshake itself; this callback runs right after it.
static esp_err_t ws_on_connect(httpd_req_t *req) {
    ws_add(httpd_req_to_sockfd(req));
    return ESP_OK;
}

static void ws_remove(int fd) {
    bool last = false;
    taskENTER_CRITICAL(&ws_lock);
    for (int i = 0; i < n_ws; i++) {
        if (ws_fds[i] == fd) {
            ws_fds[i] = ws_fds[--n_ws];
            last = n_ws == 0;
            break;
        }
    }
    taskEXIT_CRITICAL(&ws_lock);
    if (last) bus_set_up(PORT_WEB, false);
}

static void on_close(httpd_handle_t hd, int fd) {
    ws_remove(fd);
    close(fd);
}

static esp_err_t ws_handler(httpd_req_t *req) {
    ws_add(httpd_req_to_sockfd(req));  // normally already known from ws_on_connect
    httpd_ws_frame_t f = {0};
    if (httpd_ws_recv_frame(req, &f, 0) != ESP_OK) return ESP_FAIL;
    if (f.type != HTTPD_WS_TYPE_BINARY || f.len == 0 || f.len > 512 || f.len % 4) {
        uint8_t skip[64];  // anything else: read and drop it
        f.payload = skip;
        return f.len <= sizeof skip ? httpd_ws_recv_frame(req, &f, f.len) : ESP_OK;
    }
    uint8_t buf[512];
    f.payload = buf;
    if (httpd_ws_recv_frame(req, &f, f.len) != ESP_OK) return ESP_FAIL;
    int words = f.len / 4;
    for (int k = 0; k < words;) {
        ump_t u = {{0}};
        int nw = ump_words_for_mt(buf[4 * k] >> 4);
        if (k + nw > words) break;
        for (int j = 0; j < nw; j++)
            u.w[j] = (uint32_t)buf[4 * (k + j)] << 24 | (uint32_t)buf[4 * (k + j) + 1] << 16 |
                     (uint32_t)buf[4 * (k + j) + 2] << 8 | buf[4 * (k + j) + 3];
        bus_send(PORT_WEB, &u);
        k += nw;
    }
    return ESP_OK;
}

// Bus task.
static void web_write(void *ctx, const ump_t *u) {
    int nw = ump_words(u);
    taskENTER_CRITICAL(&ws_lock);
    if (n_ws_out + nw <= (int)(sizeof ws_out / sizeof ws_out[0])) {
        for (int j = 0; j < nw; j++) ws_out[n_ws_out++] = u->w[j];
    } else {
        ws_dropped += nw;
    }
    taskEXIT_CRITICAL(&ws_lock);
}

// Server task: send everything collected so far to every page.
static void ws_send_work(void *arg) {
    static uint8_t frame[4 * sizeof ws_out / sizeof ws_out[0]];
    int fds[WS_CLIENTS], n, words;
    taskENTER_CRITICAL(&ws_lock);
    words = n_ws_out;
    for (int k = 0; k < words; k++)
        frame[4 * k] = ws_out[k] >> 24, frame[4 * k + 1] = ws_out[k] >> 16, frame[4 * k + 2] = ws_out[k] >> 8,
                  frame[4 * k + 3] = ws_out[k];
    n_ws_out = 0;
    ws_queued = false;
    n = n_ws;
    memcpy(fds, ws_fds, sizeof fds);
    taskEXIT_CRITICAL(&ws_lock);
    httpd_ws_frame_t f = {.type = HTTPD_WS_TYPE_BINARY, .payload = frame, .len = 4 * words, .final = true};
    for (int i = 0; i < n && words; i++) httpd_ws_send_frame_async(server, fds[i], &f);
}

// Bus task, after a batch: queue one send unless one is already waiting (it will take these words too).
static void web_flush(void *ctx) {
    bool queue = false;
    taskENTER_CRITICAL(&ws_lock);
    if (n_ws_out && !ws_queued) queue = ws_queued = true;
    taskEXIT_CRITICAL(&ws_lock);
    if (queue && httpd_queue_work(server, ws_send_work, NULL) != ESP_OK) {
        taskENTER_CRITICAL(&ws_lock);
        ws_queued = false;  // try again after the next batch or bus tick
        taskEXIT_CRITICAL(&ws_lock);
    }
}

static const bus_output_ops_t web_ops = {.write = web_write, .flush = web_flush};

// ---- server --------------------------------------------------------------------------------------------------

void webui_start(const webui_hooks_t *h) {
    hooks = *h;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 28;
    cfg.stack_size = 6144;
    cfg.lru_purge_enable = true;  // a phone that left without closing must not lock others out
    cfg.recv_wait_timeout = 10;
    cfg.close_fn = on_close;
    if (httpd_start(&server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server did not start");
        return;
    }
    for (size_t i = 0; i < sizeof files / sizeof files[0]; i++) {
        httpd_uri_t u = {
            .uri = files[i].path, .method = HTTP_GET, .handler = serve_file, .user_ctx = (void *)&files[i]};
        httpd_register_uri_handler(server, &u);
    }
    const httpd_uri_t api[] = {
        {.uri = "/api/status", .method = HTTP_GET, .handler = get_status},
        {.uri = "/api/settings", .method = HTTP_GET, .handler = get_settings},
        {.uri = "/api/settings", .method = HTTP_POST, .handler = post_settings},
        {.uri = "/api/mode", .method = HTTP_POST, .handler = post_mode},
        {.uri = "/api/wifi", .method = HTTP_GET, .handler = get_wifi},
        {.uri = "/api/wifi", .method = HTTP_POST, .handler = post_wifi},
        {.uri = "/api/wifi", .method = HTTP_DELETE, .handler = delete_wifi},
        {.uri = "/api/update", .method = HTTP_POST, .handler = post_update},
        {.uri = "/api/coredump", .method = HTTP_GET, .handler = get_coredump},
        {.uri = "/api/coredump", .method = HTTP_DELETE, .handler = delete_coredump},
        {.uri = "/api/update/check", .method = HTTP_POST, .handler = post_update_check},
        {.uri = "/api/update/install", .method = HTTP_POST, .handler = post_update_install},
        {.uri = "/api/ci", .method = HTTP_GET, .handler = get_ci},
        {.uri = "/api/ci/device", .method = HTTP_GET, .handler = get_ci_device},
        {.uri = "/api/ci/scan", .method = HTTP_POST, .handler = post_ci_scan},
        {.uri = "/api/ci/profile", .method = HTTP_POST, .handler = post_ci_profile},
        {.uri = "/api/ci/profile", .method = HTTP_DELETE, .handler = delete_ci_profile},
    };
    for (size_t i = 0; i < sizeof api / sizeof api[0]; i++) httpd_register_uri_handler(server, &api[i]);
    httpd_uri_t ws = {.uri = "/ws",
                      .method = HTTP_GET,
                      .handler = ws_handler,
                      .is_websocket = true,
                      .ws_post_handshake_cb = ws_on_connect};
    httpd_register_uri_handler(server, &ws);
    bus_register_output(PORT_WEB, &web_ops, NULL);
    net_add_service("_http", "_tcp", 80);
    ESP_LOGI(TAG, "web UI on port 80");
}

bool webui_running(void) { return server != NULL; }
