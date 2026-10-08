// airmidi firmware entry point: bring the board up, start the bus and the transports (TRS, BLE MIDI), the button
// and LED, and the battery watch; then keep the LED state current and answer console commands.
#include <stdio.h>

#include "airmidi/settings.h"
#include "ble_midi.h"
#include "board.h"
#include "bus.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net.h"
#include "nvs_settings.h"
#include "ota.h"
#include "power.h"
#include "uart_midi.h"
#include "ui_io.h"
#include "webui.h"

static const char *TAG = "airmidi";

// All notes off on every output: sustain off, all sound off, all notes off, on all 16 channels.
static void panic(void) {
    static const uint8_t ccs[] = {64, 120, 123};
    for (int ch = 0; ch < 16; ch++) {
        for (int i = 0; i < 3; i++) {
            ump_t u = ump_m1_cv(0, 0xB0 | ch, ccs[i], 0);
            bus_broadcast(&u);
        }
    }
}

// Test hook for tools/hil/bridge_test.py (console 'm'): 60 notes on channel 6 from the web UI's port, 10 ms
// apart, so the TRS output has two sources at once and its merge can be checked on the wire.
static void inject_task(void *arg) {
    for (int i = 0; i < 60; i++) {
        ump_t u = ump_m1_cv(0, 0x95, 40 + i % 40, 1 + i);
        bus_send(PORT_WEB, &u);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    vTaskDelete(NULL);
}

static volatile button_event_t pending_event;
static bool settings_ok;

// The self-test for a new image (ota_confirm_after).
static bool healthy(void) { return settings_ok && ble_midi_ready() && webui_running() && bus_queue_dropped() == 0; }

// esp_timer task: hand the event to the main loop.
static void on_button(button_event_t ev) { pending_event = ev; }

// Switch mode now and remember it for the next boot.
static void set_mode(airmidi_mode_t mode) {
    settings_t s;
    nvs_settings_get(&s);
    if (s.mode != mode) {
        s.mode = mode;
        nvs_settings_set(&s);
    }
    ESP_LOGI(TAG, "mode: %s", airmidi_mode_name(mode));
    net_set_mode(mode);
}

static airmidi_mode_t current_mode(void) {
    settings_t s;
    nvs_settings_get(&s);
    return s.mode;
}

static void handle_button(button_event_t ev) {
    switch (ev) {
        case BUTTON_DOUBLE:
            ESP_LOGI(TAG, "button: double press, all notes off");
            panic();
            break;
        case BUTTON_SHORT:
            ESP_LOGI(TAG, "button: short press (MIDI-CI rescan arrives in phase 5)");
            break;
        case BUTTON_LONG:  // Play <-> Network; from Setup back to Play
            set_mode(current_mode() == AIRMIDI_MODE_PLAY ? AIRMIDI_MODE_NETWORK : AIRMIDI_MODE_PLAY);
            break;
        case BUTTON_VERY_LONG:
            set_mode(AIRMIDI_MODE_SETUP);
            break;
        default:
            break;
    }
}

static void print_status(void) {
    uart_midi_stats_t t;
    uart_midi_get_stats(&t);
    printf("trs: rx %lu bytes (%lu dropped), tx %lu bytes (%lu messages dropped)\n", (unsigned long)t.rx_bytes,
           (unsigned long)t.rx_dropped_bytes, (unsigned long)t.tx_bytes, (unsigned long)t.tx_dropped);
    for (int i = 0; i < 2; i++) {
        ble_midi_slot_stats_t s;
        ble_midi_get_stats(i, &s);
        printf("ble%d: %s, interval %.2f ms, mtu %u, rx %lu packets (%lu bad), tx %lu packets (%lu failed)\n", i,
               !s.connected   ? "idle"
               : s.subscribed ? "subscribed"
                              : "connected",
               s.interval_x125 * 1.25, s.mtu, (unsigned long)s.rx_packets, (unsigned long)s.rx_bad,
               (unsigned long)s.tx_packets, (unsigned long)s.tx_failed);
    }
    static const char *names[PORT_COUNT] = {"trs", "ble0", "ble1", "net0", "net1", "web", "usb", "espnow"};
    for (int p = 0; p < PORT_COUNT; p++) {
        bus_port_stats_t s;
        bus_get_stats(p, &s);
        if (s.in || s.out || s.dropped)
            printf("port %s: in %lu, out %lu, dropped %lu\n", names[p], (unsigned long)s.in, (unsigned long)s.out,
                   (unsigned long)s.dropped);
    }
    if (power_on_usb())
        printf("battery: switch off, on USB power; ");
    else
        printf("battery %lu mV (%d %%)%s; ", (unsigned long)power_cell_mv(), power_percent(),
               power_low() ? " LOW" : "");
    char ip[16];
    net_ip(ip);
    printf("mode %s; wifi %s %s rssi %d; ", airmidi_mode_name(current_mode()), net_state_name(net_state()), ip,
           net_rssi());
    printf("bus queue dropped %lu; free heap %lu\n", (unsigned long)bus_queue_dropped(),
           (unsigned long)esp_get_free_heap_size());
}

void app_main(void) {
    const esp_app_desc_t *app = esp_app_get_description();
    const board_t *b = board();
    ESP_LOGI(TAG, "airmidi %s, built %s %s with ESP-IDF %s", app->version, app->date, app->time, app->idf_ver);
    ESP_LOGI(TAG, "board: %s", b->name);
    ESP_LOGI(TAG, "reset reason: %s", ota_reset_reason());

    ota_boot_report();
    board_init();
    settings_ok = nvs_settings_init() == ESP_OK;
    settings_t s;
    nvs_settings_get(&s);

    bus_start(s.sysex_timeout_ms);
    uart_midi_start();
    ble_midi_start(s.name);
    net_start(s.mode, s.name);
    webui_start(&(webui_hooks_t){.set_mode = set_mode});
    power_start();
    ui_start(on_button);
    // A freshly updated image proves itself before it is kept: radio stack up, settings readable, still running
    // after 10 s. A crash or a failed check before then boots the previous image again.
    ota_confirm_after(10000, healthy);

    for (;;) {
        button_event_t ev = pending_event;
        if (ev) pending_event = BUTTON_NONE, handle_button(ev);

        if (power_empty()) {
            panic();
            ui_set_led(LED_OFF);
            vTaskDelay(pdMS_TO_TICKS(200));  // let the notes-off reach the outputs
            power_shutdown();
        }
        net_state_t ns = net_state();
        ui_set_led(power_low()            ? LED_LOW_BATTERY
                   : ns == NET_AP         ? LED_SETUP
                   : ns == NET_JOINING    ? LED_WIFI_JOINING
                   : ble_midi_connected() ? LED_CONNECTED
                                          : LED_ADVERTISING);

        int c = getchar();  // console (USB): 's' counters, 'i' image info, 'P'/'N'/'S' mode, 'm' merge test hook,
                            // '0'-'2' power mode
        if (c == 's') print_status();
        if (c == 'i') ota_boot_report();  // which slot runs, its OTA state, core dump
        if (c == 'P') set_mode(AIRMIDI_MODE_PLAY);
        if (c == 'N') set_mode(AIRMIDI_MODE_NETWORK);
        if (c == 'S') set_mode(AIRMIDI_MODE_SETUP);
        if (c == 'm') xTaskCreate(inject_task, "inject", 2048, NULL, 5, NULL);
        if (c >= '0' && c <= '2') power_set_mode(c - '0');  // power modes, for measuring ('2' needs RESET after)
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
