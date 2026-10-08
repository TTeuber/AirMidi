#include "net.h"

#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
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

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        join_next();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ip_text[0] = 0;
        if (mode != AIRMIDI_MODE_NETWORK || !wifi_running) return;
        state = NET_JOINING;
        if (++attempts >= RETRIES_PER_NETWORK) attempts = 0, network_index++;  // try the next stored network
        esp_timer_start_once(retry_timer, RETRY_DELAY_US);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        snprintf(ip_text, sizeof ip_text, IPSTR, IP2STR(&e->ip_info.ip));
        attempts = 0;
        state = NET_CONNECTED;
        ESP_LOGI(TAG, "joined, %s.local at %s", host, ip_text);
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

void net_set_mode(airmidi_mode_t m) {
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
    } else {
        ESP_LOGI(TAG, "Wi-Fi off (play mode)");
        return;
    }
    ESP_ERROR_CHECK(esp_wifi_start());
    wifi_running = true;
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
    mdns_service_add(host, type, proto, port, NULL, 0);
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
