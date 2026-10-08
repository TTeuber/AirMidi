#include "uart_midi.h"

#include "airmidi/midi1.h"
#include "board.h"
#include "bus.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "trs";

#define MIDI_UART UART_NUM_1  // UART0's pins (D6/D7) through the GPIO matrix; the console is on USB
#define RX_BUF 1024
#define TX_BUF 8192  // 2.6 s of MIDI at 31250 baud: a BLE burst arrives much faster than the wire
#define RUNNING_STATUS_IDLE_MS 1000

static midi1_in_t parser;
static midi1_out_t encoder;
static uart_midi_stats_t stats;
static uint32_t last_tx_ms;

static void rx_task(void *arg) {
    esp_task_wdt_add(NULL);
    uint8_t buf[64];
    for (;;) {
        // uart_read_bytes() waits until it has the full length, so ask for one byte, then take whatever else is
        // already buffered (spike S0: reading 256 at a time added up to the whole timeout to every message).
        int n = uart_read_bytes(MIDI_UART, buf, 1, pdMS_TO_TICKS(1000));
        esp_task_wdt_reset();
        if (n <= 0) continue;
        size_t more = 0;
        uart_get_buffered_data_len(MIDI_UART, &more);
        if (more > sizeof buf - 1) more = sizeof buf - 1;
        if (more) n += uart_read_bytes(MIDI_UART, buf + 1, more, 0);
        stats.rx_bytes += n;
        for (int i = 0; i < n; i++) {
            ump_t u[2];
            int k = midi1_in_feed(&parser, buf[i], u);
            for (int j = 0; j < k; j++) bus_send(PORT_TRS, &u[j]);
        }
        stats.rx_dropped_bytes = parser.dropped;
    }
}

// Called from the bus task.
static void trs_write(void *ctx, const ump_t *u) {
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    // After a quiet second, send the status byte again: a device plugged in meanwhile has no running status.
    if (now - last_tx_ms > RUNNING_STATUS_IDLE_MS) midi1_out_reset_running(&encoder);
    uint8_t bytes[MIDI1_OUT_MAX];
    size_t n = midi1_out_encode(&encoder, u, bytes);
    if (!n) return;
    size_t room = 0;
    uart_get_tx_buffer_free_size(MIDI_UART, &room);
    if (room < n) {  // cannot happen while the bus respects trs_room(); never block the bus task anyway
        stats.tx_dropped++;
        midi1_out_reset_running(&encoder);  // the next message must not rely on a status that was dropped
        return;
    }
    uart_write_bytes(MIDI_UART, bytes, n);
    stats.tx_bytes += n;
    last_tx_ms = now;
}

static size_t trs_room(void *ctx) {
    size_t room = 0;
    uart_get_tx_buffer_free_size(MIDI_UART, &room);
    return room;
}

static const bus_output_ops_t ops = {.write = trs_write, .room = trs_room, .byte_stream = true};

void uart_midi_start(void) {
    const board_t *b = board();
    uart_config_t uc = {.baud_rate = 31250,
                        .data_bits = UART_DATA_8_BITS,
                        .parity = UART_PARITY_DISABLE,
                        .stop_bits = UART_STOP_BITS_1,
                        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
                        // The crystal: unaffected by frequency scaling, and with the BLE low-power clock on the
                        // main crystal it keeps running in light sleep.
                        .source_clk = UART_SCLK_XTAL};
    ESP_ERROR_CHECK(uart_driver_install(MIDI_UART, RX_BUF, TX_BUF, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(MIDI_UART, &uc));
    ESP_ERROR_CHECK(uart_set_pin(MIDI_UART, b->midi_tx, b->midi_rx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    // Hand received bytes to the driver after 1 byte-time of silence (default: 10), so a message is routed as
    // soon as it ends, not when the line has been quiet for 3 ms.
    uart_set_rx_timeout(MIDI_UART, 1);
    // In automatic light sleep, a few edges on RX wake the chip (3 is the minimum threshold).
    ESP_ERROR_CHECK(uart_set_wakeup_threshold(MIDI_UART, 3));
    ESP_ERROR_CHECK(esp_sleep_enable_uart_wakeup(MIDI_UART));
    midi1_in_init(&parser, 0);
    midi1_out_init(&encoder, true);
    bus_register_output(PORT_TRS, &ops, NULL);
    bus_set_up(PORT_TRS, true);  // a cable may or may not be in J2; there is no way to tell, so always up
    xTaskCreate(rx_task, "trs_rx", 3072, NULL, 12, NULL);
    ESP_LOGI(TAG, "UART1 at 31250 baud, TX GPIO%d (J2 OUT), RX GPIO%d (J3 IN)", b->midi_tx, b->midi_rx);
}

void uart_midi_get_stats(uart_midi_stats_t *out) { *out = stats; }
