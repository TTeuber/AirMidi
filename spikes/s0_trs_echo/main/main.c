// Spike S0: TRS MIDI echo. Every byte that arrives on J3 (MIDI IN) goes straight back out of J2 (MIDI OUT), so a
// Mac script can send through the AudioFuse's MIDI out and read the same bytes back on its MIDI in. Raw bytes, no
// parsing: this only proves both cables and both directions. The console (USB) prints a byte count once a second
// while traffic flows, and the LED toggles on every received chunk.
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/uart.h"

#define MIDI_UART UART_NUM_1
#define PIN_TX GPIO_NUM_16  // D6 -> J2 MIDI OUT
#define PIN_RX GPIO_NUM_17  // D7 <- J3 MIDI IN (6N138, 1k pull-up)
#define PIN_LED GPIO_NUM_1  // D1

void app_main(void) {
    uart_config_t uc = {.baud_rate = 31250, .data_bits = UART_DATA_8_BITS, .parity = UART_PARITY_DISABLE,
                        .stop_bits = UART_STOP_BITS_1, .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
                        .source_clk = UART_SCLK_DEFAULT};
    ESP_ERROR_CHECK(uart_driver_install(MIDI_UART, 2048, 2048, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(MIDI_UART, &uc));
    ESP_ERROR_CHECK(uart_set_pin(MIDI_UART, PIN_TX, PIN_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    // Fire the RX interrupt after 1 byte-time of silence instead of the default 10, so a single message is echoed
    // as soon as it ends rather than waiting for the line to go quiet.
    uart_set_rx_timeout(MIDI_UART, 1);
    gpio_config_t led = {.pin_bit_mask = 1ULL << PIN_LED, .mode = GPIO_MODE_OUTPUT};
    gpio_config(&led);

    printf("\n== S0 TRS echo: J3 IN -> J2 OUT, UART1 RX GPIO17 / TX GPIO16, 31250 baud ==\n");
    printf("RX idle level: %s\n", gpio_get_level(PIN_RX) ? "high (ok)" : "LOW (cable/opto?)");
    uint8_t buf[256];
    long total = 0, shown = 0;
    int led_on = 0;
    TickType_t last_print = xTaskGetTickCount();
    for (;;) {
        // uart_read_bytes() only returns early once it has the full length, so wait for one byte, then take
        // whatever else is already buffered without waiting.
        int n = uart_read_bytes(MIDI_UART, buf, 1, pdMS_TO_TICKS(100));
        if (n > 0) {
            size_t more = 0;
            uart_get_buffered_data_len(MIDI_UART, &more);
            if (more > sizeof buf - 1) more = sizeof buf - 1;
            if (more) n += uart_read_bytes(MIDI_UART, buf + 1, more, 0);
            uart_write_bytes(MIDI_UART, buf, n);
            total += n;
            gpio_set_level(PIN_LED, led_on ^= 1);
            if (total - n == shown) {  // first chunk after a quiet second: show its first bytes
                printf("rx:");
                for (int i = 0; i < n && i < 12; i++) printf(" %02X", buf[i]);
                printf(n > 12 ? " ...\n" : "\n");
            }
        }
        if (xTaskGetTickCount() - last_print >= pdMS_TO_TICKS(1000)) {
            last_print = xTaskGetTickCount();
            if (total != shown) printf("echoed %ld bytes total\n", total), shown = total;
        }
    }
}
