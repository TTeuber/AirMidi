// Spike S2: BLE MIDI <-> TRS bridge on the C6, to measure what Wi-Fi does to BLE MIDI on the one shared radio.
// BLE MIDI writes from the Mac go out of J2 (MIDI OUT); MIDI arriving on J3 (MIDI IN) is notified back over BLE.
// The Mac times both directions on its own clock through the AudioFuse (mac_latency.py). Console commands (USB):
//   o = Wi-Fi off, i = Wi-Fi on and joined but idle, b<ip> = Wi-Fi busy (UDP flood to <ip>:9999),
//   m<ip> = moderate Wi-Fi (100 x 1000-byte UDP packets/s), s = status, c = toggle the BLE-side CI responder.
// Wi-Fi credentials come from the gitignored .wifi at the repo root (see main/CMakeLists.txt).
#include <stdio.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "lwip/sockets.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs_flash.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#define DEV_NAME "airmidi-s2"
#define MIDI_UART UART_NUM_1
#define PIN_TX GPIO_NUM_16       // D6 -> J2 MIDI OUT
#define PIN_RX GPIO_NUM_17       // D7 <- J3 MIDI IN
#define PIN_LED GPIO_NUM_1       // D1
#define RF_SWITCH_EN GPIO_NUM_3  // low powers the RF switch
#define ANT_SELECT GPIO_NUM_14   // low = ceramic antenna

// BLE MIDI service 03B80E5A-EDE8-4B33-A751-6CE34EC4C700, characteristic 7772E5DB-3868-4112-A1A9-F2669D106BF3
// (NimBLE wants the 128-bit UUIDs little-endian).
static const ble_uuid128_t midi_svc_uuid =
    BLE_UUID128_INIT(0x00, 0xC7, 0xC4, 0x4E, 0xE3, 0x6C, 0x51, 0xA7, 0x33, 0x4B, 0xE8, 0xED, 0x5A, 0x0E, 0xB8, 0x03);
static const ble_uuid128_t midi_chr_uuid =
    BLE_UUID128_INIT(0xF3, 0x6B, 0x10, 0x9D, 0x66, 0xF2, 0xA9, 0xA1, 0x12, 0x41, 0x68, 0x38, 0xDB, 0xE5, 0x72, 0x77);

static uint8_t own_addr_type;
static uint16_t conn = BLE_HS_CONN_HANDLE_NONE, midi_handle, conn_itvl;
static bool subscribed;
static volatile uint32_t ble_in_msgs, ble_out_pkts, notify_fail;

// ---- BLE -> TRS ----------------------------------------------------------------------------------------------
// In a BLE MIDI packet every status byte is preceded by a timestamp byte (bit 7 set). So a byte with bit 7 set is
// a status byte if the byte before it was a timestamp, and a timestamp otherwise. Data bytes pass through as they
// are; the UART output keeps the same running status the BLE stream used. Byte 0 is the packet header.
// S3 part 2: a minimal MIDI-CI responder on the BLE side, so the question "does macOS run MIDI-CI over a BLE MIDI
// 1.0 endpoint?" can be answered without a device behind the TRS jacks. Answers Discovery only (no categories).
static bool ci_responder = true;
static uint32_t ci_muid;

static void ci_reply_discovery(const uint8_t *m, size_t n) {
    uint16_t ts = (esp_timer_get_time() / 1000) & 0x1FFF;
    uint8_t r[48] = {0x80 | (ts >> 7), 0x80 | (ts & 0x7F), 0xF0, 0x7E, 0x7F, 0x0D, 0x71, 0x02};
    size_t k = 8;
    for (int i = 0; i < 4; i++) r[k++] = (ci_muid >> (7 * i)) & 0x7F;  // source: us
    memcpy(r + k, m + 6, 4), k += 4;                                    // destination: the initiator
    const uint8_t ident[] = {0x7D, 0, 0, 0, 0, 3, 0, 0, 0, 1, 0};      // 7D 00 00, family 0, model 3, rev 0.0.1.0
    memcpy(r + k, ident, sizeof ident), k += sizeof ident;
    r[k++] = 0;                                     // categories: none (Discovery only)
    r[k++] = 0, r[k++] = 4, r[k++] = 0, r[k++] = 0;  // max SysEx 512
    r[k++] = n > 30 ? m[30] : 0;                    // the initiator's output path ID
    r[k++] = 0x7F;                                  // no function block
    r[k++] = 0x80 | (ts & 0x7F), r[k++] = 0xF7;
    struct os_mbuf *om = ble_hs_mbuf_from_flat(r, k);
    ble_gatts_notify_custom(conn, midi_handle, om);
}

// In a BLE MIDI packet every status byte is preceded by a timestamp byte (bit 7 set). So a byte with bit 7 set is
// a status byte if the byte before it was a timestamp, and a timestamp otherwise. Data bytes pass through as they
// are; the UART output keeps the same running status the BLE stream used. Byte 0 is the packet header.
static void ble_to_trs(const uint8_t *p, size_t n) {
    uint8_t out[256];
    size_t k = 0;
    bool after_ts = false;
    for (size_t i = 1; i < n && k < sizeof out; i++) {
        if ((p[i] & 0x80) && !after_ts) {
            after_ts = true;  // timestamp: dropped, the UART sends at once
            continue;
        }
        if (p[i] & 0x80) ble_in_msgs++;
        after_ts = false;
        out[k++] = p[i];
    }
    if (k > 5 && out[0] == 0xF0) {
        printf("BLE SysEx in, %u bytes:", (unsigned)k);
        for (size_t i = 0; i < k && i < 16; i++) printf(" %02X", out[i]);
        printf(k > 16 ? " ...\n" : "\n");
        if (ci_responder && out[1] == 0x7E && out[3] == 0x0D && out[4] == 0x70 && out[k - 1] == 0xF7) {
            ci_reply_discovery(out, k);
            printf("CI: answered Discovery from %07lX as %07lX\n",
                   (unsigned long)(out[6] | out[7] << 7 | out[8] << 14 | (uint32_t)out[9] << 21),
                   (unsigned long)ci_muid);
            return;
        }
    }
    if (k) uart_write_bytes(MIDI_UART, out, k);
}

static int midi_access(uint16_t conn_handle, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg) {
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint8_t buf[256];
        uint16_t n = 0;
        ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof buf, &n);
        ble_to_trs(buf, n);
        gpio_set_level(PIN_LED, ble_in_msgs & 1);
    }
    return 0;  // reads return an empty value, as the BLE MIDI spec requires
}

static const struct ble_gatt_svc_def services[] = {
    {.type = BLE_GATT_SVC_TYPE_PRIMARY,
     .uuid = &midi_svc_uuid.u,
     .characteristics =
         (struct ble_gatt_chr_def[]){
             {.uuid = &midi_chr_uuid.u,
              .access_cb = midi_access,
              .val_handle = &midi_handle,
              .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_NOTIFY},
             {0}}},
    {0}};

// ---- TRS -> BLE ----------------------------------------------------------------------------------------------
static int data_len(uint8_t status) {
    switch (status & 0xF0) {
        case 0xC0: case 0xD0: return 1;
        case 0x80: case 0x90: case 0xA0: case 0xB0: case 0xE0: return 2;
    }
    switch (status) {
        case 0xF1: case 0xF3: return 1;
        case 0xF2: return 2;
    }
    return 0;
}

static uint8_t pkt[128];
static size_t pkt_len;

static void notify_packet(void) {
    if (pkt_len > 1 && conn != BLE_HS_CONN_HANDLE_NONE && subscribed) {
        struct os_mbuf *om = ble_hs_mbuf_from_flat(pkt, pkt_len);
        if (ble_gatts_notify_custom(conn, midi_handle, om) == 0) ble_out_pkts++;
        else notify_fail++;
    }
    pkt_len = 0;
}

// Assemble complete messages from UART bytes (running status expanded, so each BLE message carries its status)
// and append them to the packet with a timestamp in front. SysEx is passed on as it comes.
static void trs_to_ble(const uint8_t *b, size_t n) {
    static uint8_t running, need, msg[3], have;
    static bool sysex;
    uint16_t ts = (esp_timer_get_time() / 1000) & 0x1FFF;
    if (pkt_len == 0) pkt[pkt_len++] = 0x80 | (ts >> 7);
    for (size_t i = 0; i < n; i++) {
        uint8_t c = b[i];
        if (pkt_len > sizeof pkt - 8) notify_packet(), pkt[pkt_len++] = 0x80 | (ts >> 7);
        if (c >= 0xF8) {  // real-time: alone, anywhere
            pkt[pkt_len++] = 0x80 | (ts & 0x7F), pkt[pkt_len++] = c;
        } else if (c == 0xF0) {
            sysex = true, pkt[pkt_len++] = 0x80 | (ts & 0x7F), pkt[pkt_len++] = c;
        } else if (c == 0xF7) {
            sysex = false, pkt[pkt_len++] = 0x80 | (ts & 0x7F), pkt[pkt_len++] = c;
        } else if (sysex && !(c & 0x80)) {
            pkt[pkt_len++] = c;
        } else if (c & 0x80) {
            sysex = false;
            running = c < 0xF0 ? c : 0;
            msg[0] = c, have = 1, need = data_len(c);
            if (!need) pkt[pkt_len++] = 0x80 | (ts & 0x7F), pkt[pkt_len++] = c;
        } else if (have || running) {
            if (!have) msg[0] = running, have = 1, need = data_len(running);
            msg[have++] = c;
            if (have == need + 1) {
                pkt[pkt_len++] = 0x80 | (ts & 0x7F);
                memcpy(pkt + pkt_len, msg, have), pkt_len += have;
                have = 0;
            }
        }
    }
    if (!have) notify_packet();  // never split a message across packets
}

static void uart_task(void *arg) {
    uint8_t buf[64];
    for (;;) {
        int n = uart_read_bytes(MIDI_UART, buf, 1, portMAX_DELAY);
        size_t more = 0;
        uart_get_buffered_data_len(MIDI_UART, &more);
        if (more > sizeof buf - 1) more = sizeof buf - 1;
        if (more) n += uart_read_bytes(MIDI_UART, buf + 1, more, 0);
        trs_to_ble(buf, n);
    }
}

// ---- GAP -----------------------------------------------------------------------------------------------------
static void advertise(void);

static int gap_event(struct ble_gap_event *e, void *arg) {
    switch (e->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (e->connect.status) {
                advertise();
                break;
            }
            conn = e->connect.conn_handle;
            struct ble_gap_conn_desc d;
            ble_gap_conn_find(conn, &d);
            conn_itvl = d.conn_itvl;
            // Apple's accessory guidelines: min >= 11.25 ms for MIDI, max >= min + 15 ms, else iOS/macOS refuse.
            printf("connected, interval %.2f ms; asking for 11.25-26.25 ms\n", conn_itvl * 1.25);
            struct ble_gap_upd_params up = {.itvl_min = 9, .itvl_max = 21, .latency = 0,
                                            .supervision_timeout = 200};
            ble_gap_update_params(conn, &up);
            break;
        case BLE_GAP_EVENT_CONN_UPDATE: {
            struct ble_gap_conn_desc d;
            if (ble_gap_conn_find(e->conn_update.conn_handle, &d) == 0) conn_itvl = d.conn_itvl;
            printf("connection interval now %.2f ms\n", conn_itvl * 1.25);
            break;
        }
        case BLE_GAP_EVENT_DISCONNECT:
            printf("disconnected (reason 0x%X)\n", e->disconnect.reason);
            conn = BLE_HS_CONN_HANDLE_NONE, subscribed = false;
            advertise();
            break;
        case BLE_GAP_EVENT_SUBSCRIBE:
            if (e->subscribe.attr_handle == midi_handle) subscribed = e->subscribe.cur_notify;
            break;
        case BLE_GAP_EVENT_MTU:
            printf("MTU %d\n", e->mtu.value);
            break;
    }
    return 0;
}

static void advertise(void) {
    struct ble_hs_adv_fields f = {0};
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    f.uuids128 = (ble_uuid128_t *)&midi_svc_uuid;
    f.num_uuids128 = 1;
    f.uuids128_is_complete = 1;
    ble_gap_adv_set_fields(&f);
    struct ble_hs_adv_fields rsp = {0};
    rsp.name = (uint8_t *)DEV_NAME, rsp.name_len = strlen(DEV_NAME), rsp.name_is_complete = 1;
    ble_gap_adv_rsp_set_fields(&rsp);
    struct ble_gap_adv_params p = {.conn_mode = BLE_GAP_CONN_MODE_UND, .disc_mode = BLE_GAP_DISC_MODE_GEN,
                                   .itvl_min = 32, .itvl_max = 48};  // 20-30 ms
    ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, &p, gap_event, NULL);
}

static void on_sync(void) {
    ble_hs_id_infer_auto(0, &own_addr_type);
    advertise();
    printf("advertising as %s\n", DEV_NAME);
}

static void host_task(void *p) {
    nimble_port_run();
    nimble_port_freertos_deinit();
}

// ---- Wi-Fi ---------------------------------------------------------------------------------------------------
static bool wifi_inited, wifi_on;
static volatile bool got_ip;
static volatile int flood_mode;  // 0 none, 1 moderate, 2 flat out
static char flood_ip[32];
static volatile uint32_t udp_sent;

#ifdef WIFI_SSID
static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) esp_wifi_connect();
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        got_ip = false;
        if (wifi_on) esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        printf("wifi: got ip " IPSTR "\n", IP2STR(&e->ip_info.ip));
        got_ip = true;
    }
}
#endif

static void wifi_start(void) {
#ifdef WIFI_SSID
    if (!wifi_inited) {
        esp_netif_create_default_wifi_sta();
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&cfg));
        esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL);
        esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL);
        wifi_config_t wc = {0};
        strncpy((char *)wc.sta.ssid, WIFI_SSID, sizeof wc.sta.ssid);
        strncpy((char *)wc.sta.password, WIFI_PASS, sizeof wc.sta.password);
        esp_wifi_set_mode(WIFI_MODE_STA);
        esp_wifi_set_config(WIFI_IF_STA, &wc);
        wifi_inited = true;
    }
    wifi_on = true;
    esp_wifi_start();
#else
    printf("wifi: no .wifi file at build time\n");
#endif
}

static void wifi_stop(void) {
    flood_mode = 0;
    if (wifi_inited && wifi_on) wifi_on = false, esp_wifi_stop();
    got_ip = false;
}

static void flood_task(void *arg) {
    static uint8_t payload[1000];
    int s = -1;
    for (;;) {
        if (!flood_mode || !got_ip) {
            if (s >= 0) close(s), s = -1;
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        if (s < 0) s = socket(AF_INET, SOCK_DGRAM, 0);
        struct sockaddr_in to = {.sin_family = AF_INET, .sin_port = htons(9999)};
        inet_aton(flood_ip, &to.sin_addr);
        if (sendto(s, payload, sizeof payload, 0, (struct sockaddr *)&to, sizeof to) > 0) udp_sent++;
        // moderate: one packet per 10 ms tick; busy: as fast as lwIP takes them, yielding a tick every 16 packets
        // so the idle task (and its watchdog) still runs
        if (flood_mode == 1 || (udp_sent & 15) == 0) vTaskDelay(1);
    }
}

// ---- console -------------------------------------------------------------------------------------------------
static void status(void) {
    wifi_ap_record_t ap;
    int rssi = got_ip && esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0;
    printf("status: ble %s itvl %.2f ms, in %lu msgs, out %lu pkts (%lu notify fails); wifi %s rssi %d, flood %d, "
           "udp sent %lu\n",
           conn == BLE_HS_CONN_HANDLE_NONE ? "idle" : subscribed ? "subscribed" : "connected", conn_itvl * 1.25,
           (unsigned long)ble_in_msgs, (unsigned long)ble_out_pkts, (unsigned long)notify_fail,
#ifdef WIFI_SSID
           !wifi_on ? "off" :
#else
           !wifi_on ? "none" :
#endif
           got_ip ? "joined" : "joining", rssi, flood_mode, (unsigned long)udp_sent);
}

void app_main(void) {
    gpio_config_t io = {.pin_bit_mask = (1ULL << RF_SWITCH_EN) | (1ULL << ANT_SELECT) | (1ULL << PIN_LED),
                        .mode = GPIO_MODE_OUTPUT};
    gpio_config(&io);
    gpio_set_level(RF_SWITCH_EN, 0);
    gpio_set_level(ANT_SELECT, 0);

    uart_config_t uc = {.baud_rate = 31250, .data_bits = UART_DATA_8_BITS, .parity = UART_PARITY_DISABLE,
                        .stop_bits = UART_STOP_BITS_1, .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
                        .source_clk = UART_SCLK_DEFAULT};
    ESP_ERROR_CHECK(uart_driver_install(MIDI_UART, 2048, 2048, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(MIDI_UART, &uc));
    ESP_ERROR_CHECK(uart_set_pin(MIDI_UART, PIN_TX, PIN_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    uart_set_rx_timeout(MIDI_UART, 1);

    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) nvs_flash_erase(), nvs_flash_init();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ci_muid = esp_random() & 0x0FFFFEFF;
    ESP_ERROR_CHECK(nimble_port_init());
    ble_hs_cfg.sync_cb = on_sync;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set(DEV_NAME);
    ble_gatts_count_cfg(services);
    ble_gatts_add_svcs(services);
    nimble_port_freertos_init(host_task);

    xTaskCreate(uart_task, "uart", 4096, NULL, 10, NULL);
    xTaskCreate(flood_task, "flood", 4096, NULL, 2, NULL);

    char line[40];
    size_t len = 0;
    for (;;) {
        int c = getchar();
        if (c < 0) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (c != '\n' && c != '\r' && len < sizeof line - 1) {
            line[len++] = c;
            continue;
        }
        line[len] = 0;
        len = 0;
        switch (line[0]) {
            case 'o': wifi_stop(); printf("wifi: off\n"); break;
            case 'i': flood_mode = 0; if (!wifi_on) wifi_start(); printf("wifi: idle\n"); break;
            case 'b':
            case 'm':
                strncpy(flood_ip, line + 1, sizeof flood_ip - 1);
                if (!wifi_on) wifi_start();
                flood_mode = line[0] == 'b' ? 2 : 1;
                printf("wifi: %s flood to %s\n", flood_mode == 2 ? "full" : "moderate", flood_ip);
                break;
            case 's': status(); break;
            case 'c': ci_responder = !ci_responder; printf("CI responder %s\n", ci_responder ? "on" : "off"); break;
        }
    }
}
