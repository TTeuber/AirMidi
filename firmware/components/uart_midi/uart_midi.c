#include "uart_midi.h"

#include "airmidi/midi1.h"
#include "board.h"
#include "bus.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_rom_gpio.h"
#include "esp_rom_sys.h"
#include "esp_sleep.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/gpio_sig_map.h"
#include "soc/soc_caps.h"
#include "soc/uart_periph.h"

static const char *TAG = "trs";

#define MIDI_UART UART_NUM_1    // UART0's pins (D6/D7) through the GPIO matrix; the console is on USB
#if SOC_UART_HP_NUM > 2
#define MIDI_UART_B UART_NUM_2  // rev D: the Type B receiver (U7), RX only
#else
#define MIDI_UART_B UART_NUM_1  // never used: no board with both TRS types has this chip (the C6)
#endif
#define RX_BUF 1024
#define TX_BUF 8192  // 2.6 s of MIDI at 31250 baud: a BLE burst arrives much faster than the wire
#define RUNNING_STATUS_IDLE_MS 1000

// Rev D's OUT (docs/midi_receiver_d/brief.md in the design workspace, "TRS Type A and B", firmware rules 1-5).
#define OUT_TICK_MS 100
#define OUT_UNPLUG_TICKS 3  // jack_det low this many ticks in a row (sampled with OUT idle): park the lines
#define OUT_PROBE_TICKS 5   // while parked, look for a receiver this often
#define OUT_PROBE_US 1000   // both lines high this long before reading jack_det

typedef struct {
    uart_port_t uart;
    midi1_in_t parser;
    char type;  // rev D: 'A' (U4) or 'B' (U7); 0 on boards with one input
} rx_t;

static rx_t rx_a = {.uart = MIDI_UART}, rx_b = {.uart = MIDI_UART_B};
static midi1_out_t encoder;
static uart_midi_stats_t stats;
static uint32_t last_tx_ms;

// Rev D's OUT. The monitor task owns the gates; the bus task only looks at `live`.
static volatile trs_type_t out_setting = TRS_TYPE_AUTO;
static volatile char in_type;  // last heard on IN
static volatile bool live;     // lines driven and the UART on the data gate; false = parked (both lines low)
static volatile bool resend_status;
static char out_type = 'A';

static void rx_task(void *arg) {
    rx_t *r = arg;
    esp_task_wdt_add(NULL);
    uint8_t buf[64];
    for (;;) {
        // uart_read_bytes() waits until it has the full length, so ask for one byte, then take whatever else is
        // already buffered (spike S0: reading 256 at a time added up to the whole timeout to every message).
        int n = uart_read_bytes(r->uart, buf, 1, pdMS_TO_TICKS(1000));
        esp_task_wdt_reset();
        if (n <= 0) continue;
        size_t more = 0;
        uart_get_buffered_data_len(r->uart, &more);
        if (more > sizeof buf - 1) more = sizeof buf - 1;
        if (more) n += uart_read_bytes(r->uart, buf + 1, more, 0);
        if (r->type && in_type != r->type) {
            in_type = r->type;
            ESP_LOGI(TAG, "IN: a Type %c cable", r->type);
        }
        stats.rx_bytes += n;
        for (int i = 0; i < n; i++) {
            ump_t u[2];
            int k = midi1_in_feed(&r->parser, buf[i], u);
            for (int j = 0; j < k; j++) bus_send(PORT_TRS, &u[j]);
        }
        stats.rx_dropped_bytes = rx_a.parser.dropped + rx_b.parser.dropped;
    }
}

// Called from the bus task.
static void trs_write(void *ctx, const ump_t *u) {
    if (board()->trs_ab && !live) {  // no receiver in OUT: nothing is driven, so nothing to send
        stats.tx_unplugged++;
        resend_status = true;
        return;
    }
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    // After a quiet second, send the status byte again: a device plugged in meanwhile has no running status.
    if (now - last_tx_ms > RUNNING_STATUS_IDLE_MS || resend_status) {
        midi1_out_reset_running(&encoder);
        resend_status = false;
    }
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

// ---- rev D: OUT on either line -------------------------------------------------------------------------------
// Gate low = line high, gate high = line low (a P/N FET pair per line, 33R to the jack). The gates stay outputs all
// along (board_init made them so before the source came on): only their signal changes, from the plain GPIO level
// to the UART's TX and back, never through an input or a reset pin.

static gpio_num_t data_gate(char type) { return type == 'A' ? board()->midi_gate_tip : board()->midi_gate_ring; }

// A gate as a plain GPIO output at `level` (the level first, so the pin goes straight there).
static void gate_level(gpio_num_t pin, int level) {
    gpio_set_level(pin, level);
    esp_rom_gpio_connect_out_signal(pin, SIG_GPIO_OUT_IDX, false, false);
}

// Both lines low: no current, whatever is in the jack (a mono plug, headphones, nothing). The UART's TX goes nowhere.
static void park(void) {
    gate_level(board()->midi_gate_tip, 1);
    gate_level(board()->midi_gate_ring, 1);
}

// Both lines high: the idle state, and the one jack_det is read in.
static void lines_high(void) {
    gate_level(board()->midi_gate_tip, 0);
    gate_level(board()->midi_gate_ring, 0);
}

// From lines_high(): the UART's TX onto the data line's gate, inverted, so its idle high keeps the gate low (the line
// high) and a 0 bit drives the line low: current round the loop. The other line stays high as the source.
static void drive(char type) {
    out_type = type;
    esp_rom_gpio_connect_out_signal(data_gate(type), UART_PERIPH_SIGNAL(MIDI_UART, SOC_UART_TX_PIN_IDX), true, false);
    live = true;
}

static char wanted_type(void) {
    switch (out_setting) {
        case TRS_TYPE_A:
            return 'A';
        case TRS_TYPE_B:
            return 'B';
        default:
            return in_type ? in_type : 'A';  // auto: what IN heard last, the standard Type A until then
    }
}

// jack_det is high only with a receiver in OUT and the ring line high. While sending, the ring is low on every 0 bit
// (Type B) or sags near the threshold (Type A, the source), so it is only read with the UART idle.
static void out_task(void *arg) {
    const board_t *b = board();
    int lows = 0;
    for (int tick = 0;; tick++) {
        vTaskDelay(pdMS_TO_TICKS(OUT_TICK_MS));
        if (live) {
            if (wanted_type() != out_type) {  // a type change: through parked, then the probe drives the new type
                live = false;
                park();
                ESP_LOGI(TAG, "OUT: changing to Type %c", wanted_type());
                tick = OUT_PROBE_TICKS - 1;
                continue;
            }
            if (uart_wait_tx_done(MIDI_UART, 0) != ESP_OK) continue;  // sending: try next tick
            lows = gpio_get_level(b->jack_det) ? 0 : lows + 1;
            if (lows >= OUT_UNPLUG_TICKS) {
                live = false;
                park();
                lows = 0;
                ESP_LOGI(TAG, "OUT: no receiver (nothing, or a mono plug): lines parked low");
            }
        } else if (tick % OUT_PROBE_TICKS == 0) {
            // What was queued before the lines were parked drains to no pin; wait for it rather than cut a byte.
            if (uart_wait_tx_done(MIDI_UART, 0) != ESP_OK) continue;
            lines_high();
            esp_rom_delay_us(OUT_PROBE_US);
            if (gpio_get_level(b->jack_det)) {
                drive(wanted_type());
                ESP_LOGI(TAG, "OUT: receiver plugged in, sending Type %c (%s)", out_type,
                         out_setting == TRS_TYPE_AUTO ? "auto" : "set");
            } else {
                park();
            }
        }
    }
}

// ---- start ---------------------------------------------------------------------------------------------------

static void rx_uart_start(uart_port_t uart, int tx_buf) {
    uart_config_t uc = {.baud_rate = 31250,
                        .data_bits = UART_DATA_8_BITS,
                        .parity = UART_PARITY_DISABLE,
                        .stop_bits = UART_STOP_BITS_1,
                        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
                        // The crystal: unaffected by frequency scaling, and with the BLE low-power clock on the
                        // main crystal it keeps running in light sleep.
                        .source_clk = UART_SCLK_XTAL};
    ESP_ERROR_CHECK(uart_driver_install(uart, RX_BUF, tx_buf, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(uart, &uc));
}

void uart_midi_start(trs_type_t out_type_setting) {
    const board_t *b = board();
    out_setting = out_type_setting;
    rx_uart_start(MIDI_UART, TX_BUF);
    // Rev D: RX only here; drive() puts TX on a gate by hand.
    ESP_ERROR_CHECK(uart_set_pin(MIDI_UART, b->trs_ab ? UART_PIN_NO_CHANGE : b->midi_tx, b->midi_rx, UART_PIN_NO_CHANGE,
                                 UART_PIN_NO_CHANGE));
    // Hand received bytes to the driver after 1 byte-time of silence (default: 10), so a message is routed as
    // soon as it ends, not when the line has been quiet for 3 ms.
    uart_set_rx_timeout(MIDI_UART, 1);
    // In automatic light sleep, a few edges on RX wake the chip (3 is the minimum threshold).
    ESP_ERROR_CHECK(uart_set_wakeup_threshold(MIDI_UART, 3));
    ESP_ERROR_CHECK(esp_sleep_enable_uart_wakeup(MIDI_UART));
    midi1_in_init(&rx_a.parser, 0);
    midi1_out_init(&encoder, true);
    bus_register_output(PORT_TRS, &ops, NULL);
    bus_set_up(PORT_TRS, true);  // a cable may or may not be in J2; there is no way to tell, so always up
    xTaskCreate(rx_task, "trs_rx", 3072, &rx_a, 12, NULL);
    if (!b->trs_ab) {
        ESP_LOGI(TAG, "UART1 at 31250 baud, TX GPIO%d (J2 OUT), RX GPIO%d (J3 IN)", b->midi_tx, b->midi_rx);
        return;
    }
    stats.both_types = true;
    rx_a.type = 'A';
    rx_b.type = 'B';
    rx_uart_start(MIDI_UART_B, 0);
    ESP_ERROR_CHECK(
        uart_set_pin(MIDI_UART_B, UART_PIN_NO_CHANGE, b->midi_rx_b, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    uart_set_rx_timeout(MIDI_UART_B, 1);
    midi1_in_init(&rx_b.parser, 0);
    xTaskCreate(rx_task, "trs_rx_b", 3072, &rx_b, 12, NULL);
    // board_init parked the lines; the first probe looks for a receiver.
    xTaskCreate(out_task, "trs_out", 2560, NULL, 5, NULL);
    ESP_LOGI(TAG, "IN: Type A on GPIO%d, Type B on GPIO%d; OUT: tip gate GPIO%d, ring gate GPIO%d, %s", b->midi_rx,
             b->midi_rx_b, b->midi_gate_tip, b->midi_gate_ring, trs_type_name(out_setting));
}

void uart_midi_set_out_type(trs_type_t t) {
    out_setting = t;  // out_task changes over within a tick if a receiver is in
    ESP_LOGI(TAG, "OUT type: %s", trs_type_name(t));
}

void uart_midi_get_stats(uart_midi_stats_t *out) {
    *out = stats;
    out->in_type = in_type;
    out->out_type = board()->trs_ab ? (live ? out_type : wanted_type()) : 'A';
    out->out_plugged = live;
}
