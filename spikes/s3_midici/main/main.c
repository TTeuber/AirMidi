// Spike S3: MIDI-CI initiator over TRS. Two seconds after boot, and on every button press, the receiver sends a
// MIDI-CI Discovery out of J2 (MIDI OUT), then asks the device that answers for its Property Exchange capabilities
// and three resources (ResourceList, DeviceInfo, AllCtrlList), and prints what came back with timings on the USB
// console. The device is a Mac script on the AudioFuse ports (mac_responder.py) answering from hard-coded JSON.
// Questions: does MIDI-CI work at 31250 baud through the opto, how long does a control list take, and what does
// the initiator need to handle (chunking, other traffic, timeouts)? Sequential and blocking on purpose: a spike.
#include <stdio.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define MIDI_UART UART_NUM_1
#define PIN_TX GPIO_NUM_16   // D6 -> J2 MIDI OUT
#define PIN_RX GPIO_NUM_17   // D7 <- J3 MIDI IN
#define PIN_LED GPIO_NUM_1   // D1
#define PIN_BUTTON GPIO_NUM_2  // D2, to GND

#define CI_VERSION 0x02
#define MAX_SYSEX 512  // what we tell the responder we can receive (the minimum for a PE initiator)
#define BROADCAST 0x0FFFFFFF

static uint32_t our_muid;

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

static void put_muid(uint8_t *p, uint32_t m) {
    for (int i = 0; i < 4; i++) p[i] = (m >> (7 * i)) & 0x7F;
}
static uint32_t get_muid(const uint8_t *p) { return p[0] | p[1] << 7 | p[2] << 14 | (uint32_t)p[3] << 21; }
static void put14(uint8_t *p, unsigned v) { p[0] = v & 0x7F, p[1] = (v >> 7) & 0x7F; }
static unsigned get14(const uint8_t *p) { return p[0] | p[1] << 7; }
static void put28(uint8_t *p, uint32_t v) { put_muid(p, v); }

// F0 7E 7F 0D <sub2> <ver> <src muid> <dst muid>: 14 bytes, data starts at [14].
static size_t ci_header(uint8_t *m, uint8_t sub2, uint32_t dst) {
    m[0] = 0xF0, m[1] = 0x7E, m[2] = 0x7F, m[3] = 0x0D, m[4] = sub2, m[5] = CI_VERSION;
    put_muid(m + 6, our_muid);
    put_muid(m + 10, dst);
    return 14;
}

static void send(const uint8_t *m, size_t n) {
    uart_write_bytes(MIDI_UART, m, n);
    gpio_set_level(PIN_LED, 1);
}

// Read until a complete SysEx message (F0 .. F7) arrives or the timeout passes. Real-time bytes (F8-FF) may sit
// inside SysEx and are skipped; any other status byte aborts the message in progress. Returns the length or 0.
static size_t read_sysex(uint8_t *buf, size_t cap, int timeout_ms) {
    int64_t end = now_ms() + timeout_ms;
    size_t n = 0;
    bool in = false;
    while (now_ms() < end) {
        uint8_t b;
        if (uart_read_bytes(MIDI_UART, &b, 1, pdMS_TO_TICKS(20)) != 1) continue;
        if (b >= 0xF8) continue;
        if (b == 0xF0) {
            in = true, n = 0;
        } else if (!in) {
            continue;
        } else if (b == 0xF7) {
            if (n + 1 < cap) buf[n++] = b;
            gpio_set_level(PIN_LED, 0);
            return n;
        } else if (b & 0x80) {
            in = false;
            continue;
        }
        if (n < cap) buf[n++] = b;
    }
    return 0;
}

// Wait for a MIDI-CI message of type sub2 addressed to us; everything else is printed and dropped.
static size_t wait_ci(uint8_t *buf, size_t cap, uint8_t sub2, int timeout_ms) {
    int64_t end = now_ms() + timeout_ms;
    for (;;) {
        int left = (int)(end - now_ms());
        if (left <= 0) return 0;
        size_t n = read_sysex(buf, cap, left);
        if (!n) return 0;
        if (n >= 15 && buf[1] == 0x7E && buf[3] == 0x0D && buf[4] == sub2 && get_muid(buf + 10) == our_muid) return n;
        printf("  (ignored %u-byte SysEx, sub-id %02X)\n", (unsigned)n, n > 4 ? buf[4] : 0);
    }
}

static uint8_t msg[1024];

static bool discovery(uint32_t *responder, unsigned *max_sysex) {
    size_t n = ci_header(msg, 0x70, BROADCAST);
    memcpy(msg + n, (uint8_t[]){0x7D, 0, 0}, 3), n += 3;  // manufacturer: 0x7D, educational/development
    put14(msg + n, 0), n += 2;                            // family
    put14(msg + n, 1), n += 2;                            // model
    memcpy(msg + n, (uint8_t[]){0, 0, 1, 0}, 4), n += 4;  // software revision
    msg[n++] = 1 << 3;                                     // categories: Property Exchange
    put28(msg + n, MAX_SYSEX), n += 4;
    msg[n++] = 0;  // output path ID
    msg[n++] = 0xF7;
    int64_t t0 = now_ms();
    send(msg, n);
    size_t r = wait_ci(msg, sizeof msg, 0x71, 3000);  // spec: wait at least 3 s for replies
    if (!r) {
        printf("Discovery: no reply in 3 s\n");
        return false;
    }
    const uint8_t *d = msg + 14;
    *responder = get_muid(msg + 6);
    *max_sysex = r >= 30 ? get_muid(d + 12) : 128;
    printf("Discovery reply after %lld ms (%u bytes): MUID %07lX, manufacturer %02X %02X %02X, family %u, model %u, "
           "revision %u.%u.%u.%u, categories 0x%02X, max SysEx %u\n",
           now_ms() - t0, (unsigned)r, (unsigned long)*responder, d[0], d[1], d[2], get14(d + 3), get14(d + 5), d[7],
           d[8], d[9], d[10], d[11], *max_sysex);
    return true;
}

static bool pe_capabilities(uint32_t dst) {
    size_t n = ci_header(msg, 0x30, dst);
    msg[n++] = 1;           // simultaneous requests we support
    msg[n++] = 0, msg[n++] = 0;  // PE version 1.0/1.1
    msg[n++] = 0xF7;
    int64_t t0 = now_ms();
    send(msg, n);
    size_t r = wait_ci(msg, sizeof msg, 0x31, 1000);
    if (!r) {
        printf("PE capabilities: no reply\n");
        return false;
    }
    printf("PE capabilities after %lld ms: %u simultaneous requests\n", now_ms() - t0, msg[14]);
    return true;
}

static char pe_data[8192];

// Get one resource; collects every chunk of the reply into pe_data.
static bool pe_get(uint32_t dst, const char *resource) {
    static uint8_t request_id;
    request_id = (request_id + 1) & 0x7F;
    char header[64];
    int hl = snprintf(header, sizeof header, "{\"resource\":\"%s\"}", resource);
    size_t n = ci_header(msg, 0x34, dst);
    msg[n++] = request_id;
    put14(msg + n, hl), n += 2;
    memcpy(msg + n, header, hl), n += hl;
    put14(msg + n, 1), n += 2;  // number of chunks
    put14(msg + n, 1), n += 2;  // this chunk
    put14(msg + n, 0), n += 2;  // property data length
    msg[n++] = 0xF7;
    int64_t t0 = now_ms();
    send(msg, n);
    size_t total = 0, wire = 0;
    char reply_header[128] = "";
    for (;;) {
        size_t r = wait_ci(msg, sizeof msg, 0x35, 2000);
        if (!r) {
            printf("Get %s: timed out after %u bytes\n", resource, (unsigned)total);
            return false;
        }
        wire += r;
        const uint8_t *p = msg + 14;
        if (p[0] != request_id) continue;
        unsigned h = get14(p + 1);
        if (h && h < sizeof reply_header) memcpy(reply_header, p + 3, h), reply_header[h] = 0;
        p += 3 + h;
        unsigned chunks = get14(p), this_chunk = get14(p + 2), len = get14(p + 4);
        if (total + len < sizeof pe_data) memcpy(pe_data + total, p + 6, len), total += len;
        if (this_chunk == 0 || this_chunk == chunks) break;
    }
    pe_data[total] = 0;
    int64_t dt = now_ms() - t0;
    printf("Get %s after %lld ms: header %s, %u bytes of data, %u bytes on the wire (%.0f bytes/s)\n", resource, dt,
           reply_header, (unsigned)total, (unsigned)wire, dt ? wire * 1000.0 / dt : 0);
    printf("%s\n", pe_data);
    return true;
}

static void run(void) {
    our_muid = esp_random() & 0x0FFFFFFF;
    if (our_muid >= 0x0FFFFF00) our_muid &= 0x0FFFFEFF;  // reserved range and broadcast
    uart_flush_input(MIDI_UART);
    printf("\n== MIDI-CI over TRS, our MUID %07lX ==\n", (unsigned long)our_muid);
    int64_t t0 = now_ms();
    uint32_t dev;
    unsigned max_sysex;
    if (!discovery(&dev, &max_sysex) || !pe_capabilities(dev)) return;
    bool ok = pe_get(dev, "ResourceList") && pe_get(dev, "DeviceInfo") && pe_get(dev, "AllCtrlList");
    printf("== %s, %lld ms from Discovery to the last reply ==\n", ok ? "done" : "FAILED", now_ms() - t0);
}

void app_main(void) {
    uart_config_t uc = {.baud_rate = 31250, .data_bits = UART_DATA_8_BITS, .parity = UART_PARITY_DISABLE,
                        .stop_bits = UART_STOP_BITS_1, .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
                        .source_clk = UART_SCLK_DEFAULT};
    ESP_ERROR_CHECK(uart_driver_install(MIDI_UART, 4096, 2048, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(MIDI_UART, &uc));
    ESP_ERROR_CHECK(uart_set_pin(MIDI_UART, PIN_TX, PIN_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    gpio_config_t led = {.pin_bit_mask = 1ULL << PIN_LED, .mode = GPIO_MODE_OUTPUT};
    gpio_config(&led);
    gpio_config_t btn = {.pin_bit_mask = 1ULL << PIN_BUTTON, .mode = GPIO_MODE_INPUT, .pull_up_en = 1};
    gpio_config(&btn);

    vTaskDelay(pdMS_TO_TICKS(2000));
    run();
    for (;;) {
        if (!gpio_get_level(PIN_BUTTON)) {
            run();
            while (!gpio_get_level(PIN_BUTTON)) vTaskDelay(pdMS_TO_TICKS(20));
        }
        // a line on the console re-runs it too, so the Mac script can trigger a scan
        int c = getchar();
        if (c == 'd') run();
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
