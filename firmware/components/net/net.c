#include "net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "board.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "mdns.h"
#include "nvs.h"

static const char *TAG = "net";

#define RETRIES_PER_NETWORK 3
#define RETRY_DELAY_US (2 * 1000 * 1000)

static volatile net_state_t state = NET_OFF;
static airmidi_mode_t mode = AIRMIDI_MODE_PLAY;
static char host[32];
static char ip_text[16];
static int network_index, attempts;
static bool wifi_running;
static esp_timer_handle_t retry_timer;
static bool radio_only;  // Play mode: Wi-Fi radio on (not joined) for ESP-NOW

// Setup from a phone: a scan or one join attempt, outside Network mode (see net.h).
#define PROBE_TIMEOUT_US (20 * 1000 * 1000)
static portMUX_TYPE probe_lock = portMUX_INITIALIZER_UNLOCKED;
static net_probe_t probe;
static net_found_t found[NET_SCAN_MAX];
static int n_found;
static bool probe_radio;  // the probe turned the radio on, and turns it off when done
static char probe_pass[65];
static esp_timer_handle_t probe_timer;

// ---- stored networks -----------------------------------------------------------------------------------------

typedef struct {
    char ssid[33];
    char pass[65];
} network_t;

static int load(network_t *nets) {
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READONLY, &h)) return 0;
    uint8_t n = 0;
    nvs_get_u8(h, "n", &n);
    if (n > NET_MAX_NETWORKS) n = NET_MAX_NETWORKS;
    for (int i = 0; i < n; i++) {
        char key[12];
        size_t len = sizeof nets[i].ssid;
        snprintf(key, sizeof key, "s%d", i);
        if (nvs_get_str(h, key, nets[i].ssid, &len)) nets[i].ssid[0] = 0;
        len = sizeof nets[i].pass;
        snprintf(key, sizeof key, "p%d", i);
        if (nvs_get_str(h, key, nets[i].pass, &len)) nets[i].pass[0] = 0;
    }
    nvs_close(h);
    return n;
}

static esp_err_t save(const network_t *nets, int n) {
    nvs_handle_t h;
    esp_err_t e = nvs_open("wifi", NVS_READWRITE, &h);
    if (e) return e;
    nvs_erase_all(h);
    for (int i = 0; i < n && !e; i++) {
        char key[12];
        snprintf(key, sizeof key, "s%d", i);
        e = nvs_set_str(h, key, nets[i].ssid);
        snprintf(key, sizeof key, "p%d", i);
        if (!e) e = nvs_set_str(h, key, nets[i].pass);
    }
    if (!e) e = nvs_set_u8(h, "n", (uint8_t)n);
    if (!e) e = nvs_commit(h);
    nvs_close(h);
    return e;
}

int net_networks(char ssids[][33], int max) {
    network_t nets[NET_MAX_NETWORKS];
    int n = load(nets);
    for (int i = 0; i < n && i < max; i++) memcpy(ssids[i], nets[i].ssid, 33);
    return n < max ? n : max;
}

esp_err_t net_add_network(const char *ssid, const char *pass) {
    if (!ssid || !*ssid || strlen(ssid) > 32 || strlen(pass) > 64) return ESP_ERR_INVALID_ARG;
    network_t nets[NET_MAX_NETWORKS + 1];
    int n = load(nets + 1), k = 1;
    // newest first; an SSID already stored is replaced, the oldest falls off the end
    strcpy(nets[0].ssid, ssid);
    strcpy(nets[0].pass, pass);
    for (int i = 1; i <= n; i++)
        if (strcmp(nets[i].ssid, ssid)) nets[k++] = nets[i];
    if (k > NET_MAX_NETWORKS) k = NET_MAX_NETWORKS;
    esp_err_t e = save(nets, k);
    ESP_LOGI(TAG, "stored network \"%s\" (%d stored)", ssid, k);
    network_index = 0, attempts = 0;
    return e;
}

esp_err_t net_remove_network(const char *ssid) {
    network_t nets[NET_MAX_NETWORKS];
    int n = load(nets), k = 0;
    for (int i = 0; i < n; i++)
        if (strcmp(nets[i].ssid, ssid)) nets[k++] = nets[i];
    if (k == n) return ESP_ERR_NOT_FOUND;
    return save(nets, k);
}

// ---- station -------------------------------------------------------------------------------------------------

static void join_next(void) {
    network_t nets[NET_MAX_NETWORKS];
    int n = load(nets);
    if (!n) {
        ESP_LOGW(TAG, "no networks stored: add one in Setup mode");
        return;
    }
    const network_t *net = &nets[network_index % n];
    wifi_config_t wc = {0};
    memcpy(wc.sta.ssid, net->ssid, sizeof wc.sta.ssid);
    memcpy(wc.sta.password, net->pass, sizeof wc.sta.password);
    wc.sta.threshold.authmode = net->pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    wc.sta.pmf_cfg.capable = true;
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    ESP_LOGI(TAG, "joining \"%s\" (attempt %d)", net->ssid, attempts + 1);
    esp_wifi_connect();
}

static void retry(void *arg) {
    if (mode == AIRMIDI_MODE_NETWORK && wifi_running) join_next();
}

static bool sntp_started;

bool net_time_valid(void) { return time(NULL) > 1700000000; }  // 2023: anything earlier is the clock's reset value

static bool probe_event(esp_event_base_t base, int32_t id, void *data);
static void probe_cancel(void);

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (probe_event(base, id, data)) return;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (mode == AIRMIDI_MODE_NETWORK) join_next();  // in Play mode the radio may be on for ESP-NOW alone
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *d = data;
        if (mode != AIRMIDI_MODE_NETWORK || !wifi_running) return;  // a phone's try leaving again (Setup: the AP's
                                                                    // address stays in ip_text)
        ESP_LOGW(TAG, "not joined (reason %d)", d->reason);
        ip_text[0] = 0;
        state = NET_JOINING;
        if (++attempts >= RETRIES_PER_NETWORK) attempts = 0, network_index++;  // try the next stored network
        esp_timer_start_once(retry_timer, RETRY_DELAY_US);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        snprintf(ip_text, sizeof ip_text, IPSTR, IP2STR(&e->ip_info.ip));
        attempts = 0;
        state = NET_CONNECTED;
        // Modem sleep makes the router hold frames until the next beacon wake-up: network MIDI then arrives in
        // clumps ~100 ms apart (measured). Network mode is for playing over Wi-Fi, so the radio stays awake.
        esp_err_t ps = esp_wifi_set_ps(WIFI_PS_NONE);
        if (ps) ESP_LOGW(TAG, "could not turn Wi-Fi power save off: %s", esp_err_to_name(ps));
        ESP_LOGI(TAG, "joined, %s.local at %s", host, ip_text);
        // Wall-clock time, for RTP-MIDI's session clock (it must not start again from 0 after a restart).
        if (!sntp_started) {
            esp_sntp_config_t c = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
            esp_netif_sntp_init(&c);
            sntp_started = true;
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_START) {
        strcpy(ip_text, "192.168.4.1");
        state = NET_AP;
        ESP_LOGI(TAG, "access point \"%s\" up, http://192.168.4.1 or http://%s.local", host, host);
    }
}

// ---- modes ---------------------------------------------------------------------------------------------------

static void stop_wifi(void) {
    if (!wifi_running) return;
    wifi_running = false;
    esp_timer_stop(retry_timer);
    esp_wifi_stop();
    state = NET_OFF;
    ip_text[0] = 0;
}

static void limit_tx_power(void) {
    int8_t max = board()->wifi_max_tx;
    if (max) esp_wifi_set_max_tx_power(max);  // after esp_wifi_start(), or it does not hold
}

void net_set_mode(airmidi_mode_t m) {
    probe_cancel();
    mode = m;
    stop_wifi();
    if (m == AIRMIDI_MODE_NETWORK) {
        esp_wifi_set_mode(WIFI_MODE_STA);
        state = NET_JOINING;
        network_index = 0, attempts = 0;
    } else if (m == AIRMIDI_MODE_SETUP) {
        wifi_config_t wc = {.ap = {.channel = 1, .max_connection = 2, .authmode = WIFI_AUTH_OPEN}};
        // Open on purpose: it exists only while the box is in Setup mode (8 s button press), so a phone can join
        // without a password to give it one. Firmware it accepts must be signed anyway.
        size_t len = strlen(host);  // host is at most 31 characters, the SSID field holds 32
        memcpy(wc.ap.ssid, host, len);
        wc.ap.ssid_len = len;
        esp_wifi_set_mode(WIFI_MODE_AP);
        esp_wifi_set_config(WIFI_IF_AP, &wc);
    } else if (radio_only) {
        esp_wifi_set_mode(WIFI_MODE_STA);  // not joining anything: the radio is on for ESP-NOW
        ESP_ERROR_CHECK(esp_wifi_start());
        limit_tx_power();
        esp_wifi_set_ps(WIFI_PS_NONE);  // modem sleep would hold frames until the next wake-up
        wifi_running = true;
        ESP_LOGI(TAG, "Wi-Fi radio on for ESP-NOW only (play mode)");
        return;
    } else {
        ESP_LOGI(TAG, "Wi-Fi off (play mode)");
        return;
    }
    ESP_ERROR_CHECK(esp_wifi_start());
    wifi_running = true;
    limit_tx_power();
}

void net_keep_radio_on(bool on) { radio_only = on; }

// ---- setup from a phone: scan, one join attempt (see net.h) --------------------------------------------------

static const char *probe_names[] = {"idle", "scanning", "scanned", "joining", "joined", "failed"};
const char *net_probe_state_name(net_probe_state_t s) { return probe_names[s]; }

static void set_probe(net_probe_state_t st, const char *reason) {
    taskENTER_CRITICAL(&probe_lock);
    probe.state = st;
    if (reason) snprintf(probe.reason, sizeof probe.reason, "%s", reason);
    taskEXIT_CRITICAL(&probe_lock);
}

// Radio for a probe. Setup: add the station to the access point. Play: on, as a station (unless ESP-NOW has it on
// already). Returns whether the station still has to start (STA_START follows).
static esp_err_t probe_radio_on(bool *starting) {
    *starting = false;
    if (mode == AIRMIDI_MODE_NETWORK) return ESP_ERR_INVALID_STATE;
    if (mode == AIRMIDI_MODE_SETUP) {
        wifi_mode_t m;
        esp_wifi_get_mode(&m);
        if (m == WIFI_MODE_APSTA) return ESP_OK;
        *starting = true;  // stays APSTA afterwards: switching back would restart the access point under the page
        return esp_wifi_set_mode(WIFI_MODE_APSTA);
    }
    if (wifi_running) return ESP_OK;
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_err_t e = esp_wifi_start();
    if (e) return e;
    limit_tx_power();
    probe_radio = wifi_running = true;
    *starting = true;
    return ESP_OK;
}

static void probe_radio_off(void) {
    if (!probe_radio) return;
    probe_radio = false;
    wifi_running = false;
    esp_wifi_stop();
}

// The attempt is over (joined, failed or timed out): leave the network again and put the radio back.
static void probe_finish(void) {
    esp_timer_stop(probe_timer);
    memset(probe_pass, 0, sizeof probe_pass);
    esp_wifi_disconnect();
    probe_radio_off();
}

static const char *reason_text(int r) {
    switch (r) {
        case WIFI_REASON_NO_AP_FOUND:
            return "network not found (out of range, or 5 GHz only)";
        case WIFI_REASON_AUTH_FAIL:
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_HANDSHAKE_TIMEOUT:
            return "wrong password";
        case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
        case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
            return "the network's security is one the box does not support";
        default:
            return NULL;
    }
}

static void collect_scan(void) {
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n > 32) n = 32;
    wifi_ap_record_t *recs = n ? calloc(n, sizeof *recs) : NULL;
    if (n && (!recs || esp_wifi_scan_get_ap_records(&n, recs) != ESP_OK)) n = 0;
    net_found_t list[NET_SCAN_MAX];
    int k = 0;
    for (int i = 0; i < n; i++) {
        const char *name = (const char *)recs[i].ssid;
        if (!name[0]) continue;  // hidden
        int j = 0;
        while (j < k && strcmp(list[j].ssid, name)) j++;
        if (j < k) {  // the same name from another access point: keep the strongest
            if (recs[i].rssi > list[j].rssi) list[j].rssi = recs[i].rssi;
            continue;
        }
        if (k == NET_SCAN_MAX) continue;
        snprintf(list[k].ssid, sizeof list[k].ssid, "%s", name);
        list[k].rssi = recs[i].rssi;
        list[k].secure = recs[i].authmode != WIFI_AUTH_OPEN;
        k++;
    }
    free(recs);
    for (int i = 1; i < k; i++)  // strongest first (insertion sort, at most 20)
        for (int j = i; j > 0 && list[j].rssi > list[j - 1].rssi; j--) {
            net_found_t t = list[j];
            list[j] = list[j - 1], list[j - 1] = t;
        }
    taskENTER_CRITICAL(&probe_lock);
    memcpy(found, list, k * sizeof list[0]);
    n_found = k;
    taskEXIT_CRITICAL(&probe_lock);
}

static int probe_attempts;

// Events while a probe runs; true when the probe took the event (the normal handling must not see it).
static bool probe_event(esp_event_base_t base, int32_t id, void *data) {
    net_probe_state_t st = probe.state;
    if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        if (st != NET_PROBE_SCANNING) return false;
        collect_scan();
        esp_timer_stop(probe_timer);
        set_probe(NET_PROBE_SCANNED, NULL);
        probe_radio_off();
        ESP_LOGI(TAG, "scan: %d networks", n_found);
        return true;
    }
    if (st != NET_PROBE_JOINING) return false;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        return true;
    }
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *d = data;
        if (++probe_attempts < 2 && d->reason != WIFI_REASON_NO_AP_FOUND) {
            esp_wifi_connect();  // one more go: a first association sometimes fails for no reason
            return true;
        }
        char why[64];
        const char *t = reason_text(d->reason);
        if (t)
            snprintf(why, sizeof why, "%s", t);
        else
            snprintf(why, sizeof why, "not joined (reason %d)", d->reason);
        ESP_LOGW(TAG, "trying \"%s\": %s", probe.ssid, why);
        set_probe(NET_PROBE_FAILED, why);
        probe_finish();
        return true;
    }
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        taskENTER_CRITICAL(&probe_lock);
        snprintf(probe.ip, sizeof probe.ip, IPSTR, IP2STR(&e->ip_info.ip));
        taskEXIT_CRITICAL(&probe_lock);
        ESP_LOGI(TAG, "trying \"%s\": joined (%s), network stored", probe.ssid, probe.ip);
        net_add_network(probe.ssid, probe_pass);
        set_probe(NET_PROBE_JOINED, NULL);
        probe_finish();
        return true;
    }
    return false;
}

static void probe_timeout(void *arg) {
    if (probe.state == NET_PROBE_JOINING) {
        set_probe(NET_PROBE_FAILED, "no answer from the network in 20 s");
        probe_finish();
    } else if (probe.state == NET_PROBE_SCANNING) {
        set_probe(NET_PROBE_FAILED, "the scan did not finish");
        probe_radio_off();
    }
}

static bool probe_busy(void) { return probe.state == NET_PROBE_SCANNING || probe.state == NET_PROBE_JOINING; }

// A mode change in the middle of a probe: the radio is about to be set up afresh.
static void probe_cancel(void) {
    if (!probe_timer || !probe_busy()) return;
    esp_timer_stop(probe_timer);
    memset(probe_pass, 0, sizeof probe_pass);
    probe_radio = false;  // net_set_mode stops the radio itself
    set_probe(NET_PROBE_FAILED, "the box changed mode");
}

esp_err_t net_scan_start(void) {
    if (probe_busy()) return ESP_ERR_INVALID_STATE;
    bool starting;
    esp_err_t e = probe_radio_on(&starting);
    if (e) return e;
    set_probe(NET_PROBE_SCANNING, "");
    e = esp_wifi_scan_start(&(wifi_scan_config_t){.show_hidden = false}, false);
    if (e) {
        set_probe(NET_PROBE_FAILED, esp_err_to_name(e));
        probe_radio_off();
        return e;
    }
    esp_timer_start_once(probe_timer, PROBE_TIMEOUT_US);
    return ESP_OK;
}

int net_scan_results(net_found_t *out, int max) {
    taskENTER_CRITICAL(&probe_lock);
    int n = n_found < max ? n_found : max;
    memcpy(out, found, n * sizeof found[0]);
    taskEXIT_CRITICAL(&probe_lock);
    return n;
}

esp_err_t net_try_network(const char *ssid, const char *pass) {
    if (!ssid || !*ssid || strlen(ssid) > 32 || (pass && strlen(pass) > 64)) return ESP_ERR_INVALID_ARG;
    if (probe_busy()) return ESP_ERR_INVALID_STATE;
    if (mode == AIRMIDI_MODE_NETWORK) return ESP_ERR_INVALID_STATE;
    taskENTER_CRITICAL(&probe_lock);
    snprintf(probe.ssid, sizeof probe.ssid, "%s", ssid);
    probe.ip[0] = probe.reason[0] = 0;
    probe.state = NET_PROBE_JOINING;
    taskEXIT_CRITICAL(&probe_lock);
    snprintf(probe_pass, sizeof probe_pass, "%s", pass ? pass : "");
    probe_attempts = 0;
    wifi_config_t wc = {0};
    memcpy(wc.sta.ssid, ssid, strlen(ssid));
    memcpy(wc.sta.password, probe_pass, strlen(probe_pass));
    wc.sta.threshold.authmode = probe_pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    wc.sta.pmf_cfg.capable = true;
    bool starting;
    esp_err_t e = probe_radio_on(&starting);
    if (!e) e = esp_wifi_set_config(WIFI_IF_STA, &wc);
    if (!e && !starting) e = esp_wifi_connect();  // otherwise on STA_START
    if (e) {
        set_probe(NET_PROBE_FAILED, esp_err_to_name(e));
        probe_finish();
        return e;
    }
    ESP_LOGI(TAG, "trying \"%s\"", ssid);
    esp_timer_start_once(probe_timer, PROBE_TIMEOUT_US);
    return ESP_OK;
}

void net_probe(net_probe_t *out) {
    taskENTER_CRITICAL(&probe_lock);
    *out = probe;
    taskEXIT_CRITICAL(&probe_lock);
}

uint8_t net_ap_channel(void) {
    wifi_ap_record_t ap;
    return state == NET_CONNECTED && esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.primary : 0;
}

void net_start(airmidi_mode_t m, const char *name) {
    snprintf(host, sizeof host, "%s", name);
    ESP_ERROR_CHECK(esp_netif_init());
    esp_event_loop_create_default();  // may already exist
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();
    esp_log_level_set("wifi", ESP_LOG_WARN);  // the driver logs every association step at INFO
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    esp_wifi_set_storage(WIFI_STORAGE_RAM);  // networks live in our own NVS namespace, not the driver's
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);
    const esp_timer_create_args_t ta = {.callback = retry, .name = "wifi_retry"};
    ESP_ERROR_CHECK(esp_timer_create(&ta, &retry_timer));
    const esp_timer_create_args_t pa = {.callback = probe_timeout, .name = "wifi_probe"};
    ESP_ERROR_CHECK(esp_timer_create(&pa, &probe_timer));

    ESP_ERROR_CHECK(mdns_init());
    mdns_hostname_set(host);
    mdns_instance_name_set(host);

#ifdef DEV_WIFI_SSID
    network_t nets[NET_MAX_NETWORKS];
    if (!load(nets)) net_add_network(DEV_WIFI_SSID, DEV_WIFI_PASS);  // dev build: the network from .wifi
#endif
    net_set_mode(m);
}

void net_add_service(const char *type, const char *proto, uint16_t port) {
    mdns_txt_item_t txt[] = {{"id", board_id()}};  // the same box as the one with this id over BLE
    mdns_service_add(host, type, proto, port, txt, 1);
}

net_state_t net_state(void) { return state; }

const char *net_state_name(net_state_t s) {
    static const char *names[] = {"off", "joining", "connected", "access point"};
    return names[s];
}

void net_ip(char out[16]) { memcpy(out, ip_text, 16); }

void net_ssid(char out[33]) {
    wifi_ap_record_t ap;
    out[0] = 0;
    if (state == NET_CONNECTED && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) memcpy(out, ap.ssid, 33);
    out[32] = 0;
}

int net_rssi(void) {
    wifi_ap_record_t ap;
    return state == NET_CONNECTED && esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0;
}
