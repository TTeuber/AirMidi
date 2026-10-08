// Spike S1: a minimal Network MIDI 2.0 (UDP, M2-124) host on the receiver, bridged to the TRS jacks, to find out
// whether macOS 26 finds it, connects to it and passes MIDI through it. The receiver joins Wi-Fi (.wifi), advertises
// _midi2._udp over mDNS, accepts one client session (no authentication), answers Ping/Bye/Session Reset and UMP
// Endpoint/Function Block discovery, turns incoming UMP into MIDI 1.0 bytes on J2 (MIDI OUT) and MIDI from J3 (MIDI
// IN) into UMP for the client. MIDI 1.0 protocol (MT 0x2) on the wire; MIDI 2.0 channel voice coming in is scaled
// down. No FEC, no retransmit: a spike. Every session event is printed on the USB console.
#include <stdio.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "mdns.h"
#include "nvs_flash.h"

#define MIDI_UART UART_NUM_1
#define PIN_TX GPIO_NUM_16       // D6 -> J2 MIDI OUT
#define PIN_RX GPIO_NUM_17       // D7 <- J3 MIDI IN
#define PIN_LED GPIO_NUM_1       // D1
#define RF_SWITCH_EN GPIO_NUM_3  // low powers the RF switch
#define ANT_SELECT GPIO_NUM_14   // low = ceramic antenna
#define HOST_PORT 5673
#define EP_NAME "airmidi S1"

static char piid[16], hostname[24];
static int sock = -1;
static SemaphoreHandle_t tx_lock;

// ---- session (one client) ------------------------------------------------------------------------------------
static struct sockaddr_in client;
static bool established;
static uint16_t tx_seq, rx_seq_next;
static uint32_t rx_umps, tx_umps, rx_lost;

static void put32(uint8_t *p, uint32_t v) { p[0] = v >> 24, p[1] = v >> 16, p[2] = v >> 8, p[3] = v; }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

static void send_to(const struct sockaddr_in *to, const uint8_t *cmd, size_t n) {
    uint8_t pkt[1400] = {'M', 'I', 'D', 'I'};
    memcpy(pkt + 4, cmd, n);
    sendto(sock, pkt, n + 4, 0, (const struct sockaddr *)to, sizeof *to);
}

// Command with a string field in words: name (padded to 32 bits), then product instance id.
static size_t name_cmd(uint8_t *c, uint8_t code, uint8_t csd2) {
    size_t nl = strlen(EP_NAME), pl = strlen(piid);
    size_t nw = (nl + 3) / 4, pw = (pl + 3) / 4;
    memset(c, 0, 4 + 4 * (nw + pw));
    c[0] = code, c[1] = nw + pw, c[2] = nw, c[3] = csd2;
    memcpy(c + 4, EP_NAME, nl);
    memcpy(c + 4 + 4 * nw, piid, pl);
    return 4 + 4 * (nw + pw);
}

static void send_umps(const uint32_t *w, size_t nwords) {
    if (!established) return;
    uint8_t c[4 + 64 * 4];
    xSemaphoreTake(tx_lock, portMAX_DELAY);
    c[0] = 0xFF, c[1] = nwords, c[2] = tx_seq >> 8, c[3] = tx_seq & 0xFF;
    tx_seq++;
    for (size_t i = 0; i < nwords; i++) put32(c + 4 + 4 * i, w[i]);
    send_to(&client, c, 4 + 4 * nwords);
    tx_umps++;
    xSemaphoreGive(tx_lock);
}

// ---- UMP stream messages (MT 0xF): answer the client's endpoint and function block discovery ----------------
static void stream_text(uint16_t status, const char *s, int per_first, uint8_t fb) {
    // Endpoint Name / Product Instance Id: 14 bytes per UMP; Function Block Name: fb number + 13 bytes.
    size_t len = strlen(s), off = 0;
    do {
        uint32_t w[4] = {0};
        uint8_t b[16] = {0};
        int k = 2;  // bytes 0-1 hold type/format/status
        if (status == 0x012) b[k++] = fb;
        size_t room = 16 - k, take = len - off < room ? len - off : room;
        memcpy(b + k, s + off, take);
        bool first = off == 0, last = off + take >= len;
        int form = first && last ? 0 : first ? 1 : last ? 3 : 2;
        off += take;
        for (int i = 0; i < 4; i++) w[i] = get32(b + 4 * i);
        w[0] |= 0xF0000000u | form << 26 | status << 16;
        send_umps(w, 4);
    } while (off < len);
    (void)per_first;
}

static void handle_stream(const uint32_t *w) {
    uint16_t status = (w[0] >> 16) & 0x3FF;
    if (status == 0x000) {  // Endpoint Discovery
        uint8_t filter = w[1] & 0xFF;
        printf("UMP: Endpoint Discovery, UMP %lu.%lu, filter 0x%02X\n", (w[0] >> 8) & 0xFF, w[0] & 0xFF, filter);
        if (filter & 0x01) {  // Endpoint Info: UMP 1.1, static, 1 function block, MIDI 1.0 protocol only
            uint32_t r[4] = {0xF0010101, 1u << 31 | 1u << 24 | 1u << 8, 0, 0};
            send_umps(r, 4);
        }
        if (filter & 0x02) {  // Device Identity: 7D 00 00 (development), family 0, model 1, version 0.0.1.0
            uint32_t r[4] = {0xF0020000, 0x007D0000, 0x00000100, 0x00000100};
            send_umps(r, 4);
        }
        if (filter & 0x04) stream_text(0x003, EP_NAME, 0, 0);
        if (filter & 0x08) stream_text(0x004, piid, 0, 0);
        if (filter & 0x10) {  // Stream Configuration Notification: MIDI 1.0 protocol, no JR timestamps
            uint32_t r[4] = {0xF0060100, 0, 0, 0};
            send_umps(r, 4);
        }
    } else if (status == 0x005) {  // Stream Configuration Request: we only do MIDI 1.0 protocol
        printf("UMP: Stream Configuration Request, protocol %lu -> answering 1\n", (w[0] >> 8) & 0xFF);
        uint32_t r[4] = {0xF0060100, 0, 0, 0};
        send_umps(r, 4);
    } else if (status == 0x010) {  // Function Block Discovery
        uint8_t fb = (w[0] >> 8) & 0xFF, filter = w[0] & 0xFF;
        printf("UMP: Function Block Discovery, block %u, filter 0x%02X\n", fb, filter);
        if (fb != 0 && fb != 0xFF) return;
        if (filter & 0x01) {  // active, block 0, bidirectional, MIDI 1.0 at 31.25k, UI hint sender+receiver
            uint32_t r[4] = {0xF0110000 | 1u << 15 | 0 << 8 | 0x3 << 4 | 0x2 << 2 | 0x3, 0x00010000, 0, 0};
            send_umps(r, 4);
        }
        if (filter & 0x02) stream_text(0x012, "TRS", 0, 0);
    } else {
        printf("UMP: stream message status 0x%03X ignored\n", status);
    }
}

// ---- UMP -> MIDI 1.0 bytes on J2 -----------------------------------------------------------------------------
static int data_len(uint8_t status) {
    switch (status & 0xF0) {
        case 0xC0: case 0xD0: return 1;
        case 0x80: case 0x90: case 0xA0: case 0xB0: case 0xE0: return 2;
    }
    return status == 0xF2 ? 2 : (status == 0xF1 || status == 0xF3) ? 1 : 0;
}

static int ump_words(uint8_t mt) {
    static const uint8_t n[16] = {1, 1, 1, 2, 2, 4, 1, 1, 2, 2, 2, 3, 3, 4, 4, 4};
    return n[mt];
}

static void ump_to_midi1(const uint32_t *w) {
    uint8_t mt = w[0] >> 28, b[16];
    size_t n = 0;
    if (mt == 0x1 || mt == 0x2) {
        uint8_t st = w[0] >> 16;
        b[n++] = st;
        int dl = data_len(st);
        if (dl > 0) b[n++] = (w[0] >> 8) & 0x7F;
        if (dl > 1) b[n++] = w[0] & 0x7F;
    } else if (mt == 0x3) {  // SysEx7 in 64-bit packets
        uint8_t form = (w[0] >> 20) & 0xF, cnt = (w[0] >> 16) & 0xF;
        uint8_t d[6] = {w[0] >> 8, w[0], w[1] >> 24, w[1] >> 16, w[1] >> 8, w[1]};
        if (form == 0 || form == 1) b[n++] = 0xF0;
        for (int i = 0; i < cnt && i < 6; i++) b[n++] = d[i] & 0x7F;
        if (form == 0 || form == 3) b[n++] = 0xF7;
    } else if (mt == 0x4) {  // MIDI 2.0 channel voice, scaled down (M2-104 translation, simplified)
        uint8_t op = (w[0] >> 20) & 0xF, ch = (w[0] >> 16) & 0xF, idx = (w[0] >> 8) & 0x7F;
        b[n++] = op << 4 | ch;
        switch (op) {
            case 0x8: case 0x9: {
                uint8_t v = w[1] >> 25;
                if (op == 0x9 && v == 0 && (w[1] >> 16)) v = 1;
                b[n++] = idx, b[n++] = v;
                break;
            }
            case 0xA: case 0xB: b[n++] = idx, b[n++] = w[1] >> 25; break;
            case 0xC: b[n++] = (w[1] >> 24) & 0x7F; break;
            case 0xD: b[n++] = w[1] >> 25; break;
            case 0xE: b[n++] = (w[1] >> 18) & 0x7F, b[n++] = (w[1] >> 25) & 0x7F; break;
            default: n = 0;  // per-note and (N)RPN messages: not in the spike
        }
    } else if (mt == 0xF) {
        handle_stream(w);
    }
    if (n) uart_write_bytes(MIDI_UART, b, n), gpio_set_level(PIN_LED, ++rx_umps & 1);
}

// ---- UDP host ------------------------------------------------------------------------------------------------
static void handle_packet(const uint8_t *p, size_t n, const struct sockaddr_in *from) {
    if (n < 8 || memcmp(p, "MIDI", 4)) return;
    char who[24];
    snprintf(who, sizeof who, "%s:%u", inet_ntoa(from->sin_addr), ntohs(from->sin_port));
    bool from_client = established && from->sin_addr.s_addr == client.sin_addr.s_addr &&
                       from->sin_port == client.sin_port;
    for (size_t i = 4; i + 4 <= n;) {
        const uint8_t *c = p + i;
        uint8_t code = c[0], words = c[1];
        size_t len = 4 + 4 * words;
        if (i + len > n) break;
        uint8_t r[256];
        switch (code) {
            case 0x01: {  // Invitation
                char name[100] = "";
                size_t nl = c[2] * 4 < sizeof name - 1 ? c[2] * 4 : sizeof name - 1;
                memcpy(name, c + 4, nl), name[nl] = 0;
                printf("NM2: Invitation from %s (\"%s\", caps 0x%02X) -> accepted\n", who, name, c[3]);
                client = *from, established = true, tx_seq = 0, rx_seq_next = 0;
                send_to(from, r, name_cmd(r, 0x10, 0));
                break;
            }
            case 0x20:  // Ping -> Ping Reply with the same id
                memcpy(r, c, 8), r[0] = 0x21;
                send_to(from, r, 8);
                break;
            case 0x21: break;
            case 0x82:  // Session Reset
                printf("NM2: Session Reset from %s\n", who);
                tx_seq = 0, rx_seq_next = 0;
                r[0] = 0x83, r[1] = r[2] = r[3] = 0;
                send_to(from, r, 4);
                break;
            case 0xF0:  // Bye
                printf("NM2: Bye from %s (reason 0x%02X)\n", who, c[2]);
                if (from_client) established = false;
                r[0] = 0xF1, r[1] = r[2] = r[3] = 0;
                send_to(from, r, 4);
                break;
            case 0xF1: break;
            case 0xFF: {  // UMP Data
                if (!from_client) {
                    r[0] = 0xF0, r[1] = 0, r[2] = 0x05, r[3] = 0;  // Bye: session not established
                    send_to(from, r, 4);
                    break;
                }
                uint16_t seq = c[2] << 8 | c[3];
                if ((int16_t)(seq - rx_seq_next) < 0) break;  // duplicate or old
                rx_lost += (uint16_t)(seq - rx_seq_next);
                rx_seq_next = seq + 1;
                uint32_t w[64];
                for (int k = 0; k < words; k++) w[k] = get32(c + 4 + 4 * k);
                for (int k = 0; k < words;) {
                    int nw = ump_words(w[k] >> 28);
                    if (k + nw > words) break;
                    ump_to_midi1(w + k);
                    k += nw;
                }
                break;
            }
            default:  // NAK: command not supported
                printf("NM2: command 0x%02X from %s not supported\n", code, who);
                r[0] = 0x8F, r[1] = 1, r[2] = 0x01, r[3] = 0;
                memcpy(r + 4, c, 4);
                send_to(from, r, 8);
        }
        i += len;
    }
}

static void udp_task(void *arg) {
    sock = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(HOST_PORT), .sin_addr.s_addr = INADDR_ANY};
    bind(sock, (struct sockaddr *)&a, sizeof a);
    uint8_t buf[1500];
    for (;;) {
        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        int n = recvfrom(sock, buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
        if (n > 0) handle_packet(buf, n, &from);
    }
}

// ---- TRS -> UMP ----------------------------------------------------------------------------------------------
static void uart_task(void *arg) {
    uint8_t running = 0, msg[3], have = 0, need = 0, sx[6], sxn = 0;
    bool sysex = false, sx_started = false;
    for (;;) {
        uint8_t c;
        if (uart_read_bytes(MIDI_UART, &c, 1, portMAX_DELAY) != 1) continue;
        uint32_t w[2];
        if (c >= 0xF8) {  // real-time
            w[0] = 0x10000000u | c << 16;
            send_umps(w, 1);
        } else if (c == 0xF0) {
            sysex = true, sx_started = false, sxn = 0;
        } else if (sysex && (c == 0xF7 || !(c & 0x80))) {
            if (c != 0xF7) sx[sxn++] = c;
            if (sxn == 6 || c == 0xF7) {
                uint8_t form = c == 0xF7 ? (sx_started ? 3 : 0) : (sx_started ? 2 : 1);
                w[0] = 0x30000000u | form << 20 | sxn << 16 | (sxn > 0 ? sx[0] << 8 : 0) | (sxn > 1 ? sx[1] : 0);
                w[1] = (uint32_t)(sxn > 2 ? sx[2] : 0) << 24 | (sxn > 3 ? sx[3] : 0) << 16 |
                       (sxn > 4 ? sx[4] : 0) << 8 | (sxn > 5 ? sx[5] : 0);
                send_umps(w, 2);
                sx_started = true, sxn = 0;
                if (c == 0xF7) sysex = false;
            }
        } else if (c & 0x80) {
            sysex = false;
            running = c < 0xF0 ? c : 0;
            msg[0] = c, have = 1, need = data_len(c);
            if (!need) w[0] = 0x10000000u | c << 16, send_umps(w, 1), have = 0;
        } else if (have || running) {
            if (!have) msg[0] = running, have = 1, need = data_len(running);
            msg[have++] = c;
            if (have == need + 1) {
                uint32_t mt = msg[0] >= 0xF0 ? 0x1 : 0x2;
                w[0] = mt << 28 | msg[0] << 16 | msg[1] << 8 | (need > 1 ? msg[2] : 0);
                send_umps(w, 1);
                have = 0;
            }
        }
    }
}

// ---- Wi-Fi + mDNS --------------------------------------------------------------------------------------------
static SemaphoreHandle_t got_ip;

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) esp_wifi_connect();
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) esp_wifi_connect();
    else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        printf("wifi: got ip " IPSTR "\n", IP2STR(&e->ip_info.ip));
        xSemaphoreGive(got_ip);
    }
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

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(piid, sizeof piid, "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    snprintf(hostname, sizeof hostname, "airmidi-%02x%02x", mac[4], mac[5]);
    tx_lock = xSemaphoreCreateMutex();
    got_ip = xSemaphoreCreateBinary();

    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) nvs_flash_erase(), nvs_flash_init();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
#ifndef WIFI_SSID
    printf("no .wifi at build time\n");
    return;
#else
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
    esp_wifi_start();
    xSemaphoreTake(got_ip, portMAX_DELAY);
    esp_wifi_set_ps(WIFI_PS_NONE);  // no power save in the spike: measure the protocol, not modem sleep

    ESP_ERROR_CHECK(mdns_init());
    mdns_hostname_set(hostname);
    mdns_txt_item_t txt[] = {{"UMPEndpointName", EP_NAME}, {"ProductInstanceId", piid}};
    ESP_ERROR_CHECK(mdns_service_add(hostname, "_midi2", "_udp", HOST_PORT, txt, 2));
    printf("mDNS: %s._midi2._udp.local port %d, endpoint \"%s\", product instance %s\n", hostname, HOST_PORT,
           EP_NAME, piid);

    xTaskCreate(udp_task, "udp", 6144, NULL, 10, NULL);
    xTaskCreate(uart_task, "uart", 4096, NULL, 10, NULL);
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        printf("status: %s, rx %lu UMP msgs to TRS, tx %lu UMP packets, rx gaps %lu\n",
               established ? inet_ntoa(client.sin_addr) : "no session", (unsigned long)rx_umps,
               (unsigned long)tx_umps, (unsigned long)rx_lost);
    }
#endif
}
