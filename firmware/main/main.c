// airmidi firmware entry point: bring the board up, start the bus, the transports (TRS, BLE MIDI, Network MIDI 2.0,
// RTP-MIDI), MIDI-CI,
// the web UI, the button and LED (rev A) or the panel (rev B), and the battery watch; then keep the LEDs current and
// answer console commands.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "airmidi/settings.h"
#include "api.h"
#include "ble_config.h"
#include "ble_midi.h"
#include "board.h"
#include "bus.h"
#include "ci.h"
#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "espnow_midi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "net.h"
#include "net_midi.h"
#include "nvs_settings.h"
#include "ota.h"
#include "power.h"
#include "rtp_midi.h"
#include "uart_midi.h"
#include "ui_io.h"
#include "usb_midi.h"
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

static QueueHandle_t button_events;  // (key << 8 | event) from the ui timer to the main loop
static bool settings_ok;
static bool ble_running;  // the BLE stack was started at boot

// The self-test for a new image (ota_confirm_after).
static bool healthy(void) {
#if CONFIG_AIRMIDI_TEST_SELFTEST_FAIL
    return false;  // test image (ota_test.py): must roll back
#endif
    return settings_ok && (!ble_running || ble_midi_ready()) && webui_running() && bus_queue_dropped() == 0;
}

#if CONFIG_AIRMIDI_TEST_CRASH
static void crash(void *arg) { abort(); }  // test image (ota_test.py): must roll back
#endif

// esp_timer task: hand the event to the main loop.
static void on_button(int key, button_event_t ev) {
    uint16_t e = key << 8 | ev;
    xQueueSend(button_events, &e, 0);
}

// Survives the restart a mode change can cause (not a power-on): the mode Setup returns to, and a panel hold that
// needed the other mode first (BLE hold from Network: pair once BLE is up; NET hold from Play: check for an update
// once Wi-Fi has joined).
typedef enum { AFTER_NOTHING, AFTER_PAIR, AFTER_UPDATE } after_restart_t;
#define KEPT_MAGIC 0xA1D1B0B0u
static RTC_NOINIT_ATTR struct {
    uint32_t magic;
    uint8_t before_setup;  // airmidi_mode_t
    uint8_t after;         // after_restart_t
} kept;

static void kept_check(void) {
    if (kept.magic != KEPT_MAGIC || kept.before_setup >= AIRMIDI_MODE_COUNT || kept.before_setup == AIRMIDI_MODE_SETUP)
        kept.before_setup = AIRMIDI_MODE_PLAY, kept.after = AFTER_NOTHING;
    kept.magic = KEPT_MAGIC;
}

// BLE shares the radio with Wi-Fi and costs network MIDI ~16 ms of latency (measured), so Network mode runs
// without it unless the setting asks for it.
static bool ble_wanted(const settings_t *s) { return s->mode != AIRMIDI_MODE_NETWORK || s->ble_in_network; }

// Network MIDI 2.0 and RTP-MIDI, the first time the box is in Network mode (their sessions take ~45 KB that Play
// mode never needs; the S3 runs short of RAM otherwise).
static void start_network_midi(void) {
    settings_t s;
    nvs_settings_get(&s);
    net_midi_start(s.name);
    rtp_midi_start(s.name);
}

// Switch mode now and remember it for the next boot. Starting or stopping BLE needs a restart (NimBLE does not
// come back cleanly after a deinit), so a switch that changes it saves and restarts.
static void set_mode(airmidi_mode_t mode) {
    settings_t s;
    nvs_settings_get(&s);
    if (s.mode != mode) {
        if (mode == AIRMIDI_MODE_SETUP) kept.before_setup = s.mode;  // the panel's STP tap goes back to it
        s.mode = mode;
        nvs_settings_set(&s);
    }
    ESP_LOGI(TAG, "mode: %s", airmidi_mode_name(mode));
    if (ble_wanted(&s) != ble_running) {
        ESP_LOGI(TAG, "BLE %s: restarting", ble_running ? "off" : "on");
        panic();
        vTaskDelay(pdMS_TO_TICKS(300));
        esp_restart();
    }
    net_set_mode(mode);
    if (mode == AIRMIDI_MODE_NETWORK) start_network_midi();
    if (ble_running) ble_midi_set_slow_advertising(mode != AIRMIDI_MODE_PLAY);
    // Setup mode is the pairing window for phones (ble_config.h): open while in it, shut on the way out.
    if (mode == AIRMIDI_MODE_SETUP)
        ble_config_set_window(BLE_CONFIG_WINDOW_FOREVER);
    else if (ble_config_window_left() == BLE_CONFIG_WINDOW_FOREVER)
        ble_config_set_window(0);
}

// The BLE config service answers with the same API as the web page.
static ble_config_reply_t ble_api(const char *method, const char *path, const char *body, size_t len) {
    api_reply_t r = api_call(method, path, body, len);
    return (ble_config_reply_t){.status = r.status, .json = r.json, .then = r.then, .then_arg = r.then_arg};
}

// Console 'E': the ESP-NOW cable on or off (for boxes without the web UI at hand). Restarts: the radio's state at
// boot depends on it.
static void toggle_espnow(void) {
    settings_t s;
    nvs_settings_get(&s);
    s.espnow = !s.espnow;
    nvs_settings_set(&s);
    ESP_LOGI(TAG, "ESP-NOW %s: restarting", s.espnow ? "on" : "off");
    panic();
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
}

static airmidi_mode_t current_mode(void) {
    settings_t s;
    nvs_settings_get(&s);
    return s.mode;
}

// ---- panel (rev B) --------------------------------------------------------------------------------------------

#define UPDATE_JOIN_WAIT_MS 30000  // an update check waits this long for Wi-Fi to join

static struct {
    bool update_waiting;  // for Wi-Fi to join; since update_asked
    uint32_t update_asked;
    volatile bool update_checking;
    volatile panel_result_t update_result;
    volatile uint32_t update_at;
    bool was_scanning;
    panel_result_t scan_result;
    uint32_t scan_at;
} pnl;

static uint32_t ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

// Ask GitHub for the latest release (blocking HTTPS, so a task of its own); install it when it is newer. The
// install restarts the box when the image verifies; the NET LED blinks fast meanwhile.
static void update_task(void *arg) {
    ota_manifest_t m;
    char err[64] = "";
    esp_err_t e = ota_check(NULL, &m, err, sizeof err);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "update check failed: %s", err);
    } else if (!m.newer) {
        ESP_LOGI(TAG, "up to date (%s)", m.version);
    } else {
        ESP_LOGI(TAG, "update %s found: installing", m.version);
        e = ota_install_start(NULL);
        if (e != ESP_OK) ESP_LOGW(TAG, "update install: %s", esp_err_to_name(e));
    }
    pnl.update_result = e == ESP_OK ? PANEL_RESULT_OK : PANEL_RESULT_FAILED;
    pnl.update_at = ms();
    pnl.update_checking = false;
    vTaskDelete(NULL);
}

static void update_check(void) {
    if (pnl.update_checking) return;
    if (net_state() != NET_CONNECTED) {  // the main loop starts it once joined
        pnl.update_waiting = true;
        pnl.update_asked = ms();
        return;
    }
    pnl.update_waiting = false;
    pnl.update_checking = true;
    if (xTaskCreate(update_task, "update", 8192, NULL, 4, NULL) != pdPASS) {
        pnl.update_checking = false;
        pnl.update_result = PANEL_RESULT_FAILED;
        pnl.update_at = ms();
    }
}

static void update_poll(void) {
    if (!pnl.update_waiting) return;
    if (net_state() == NET_CONNECTED) {
        update_check();
    } else if (ms() - pnl.update_asked > UPDATE_JOIN_WAIT_MS) {
        ESP_LOGW(TAG, "update check: Wi-Fi did not join");
        pnl.update_waiting = false;
        pnl.update_result = PANEL_RESULT_FAILED;
        pnl.update_at = ms();
    }
}

// The pairing window for a new phone (ble_config.h), as the web page's "add a phone" opens it.
static void pair(void) {
    if (ble_config_window_left() == BLE_CONFIG_WINDOW_FOREVER) return;  // Setup mode: open already
    ESP_LOGI(TAG, "pairing window open for 2 min");
    ble_config_set_window(120);
}

static void power_off_now(void) {
    panic();
    ui_dark();
    vTaskDelay(pdMS_TO_TICKS(200));  // let the notes-off reach the outputs
    power_off(board()->panel_button[PANEL_PWR]);
}

static void handle_panel(int key, button_event_t ev) {
    static const char *names[PANEL_KEYS] = {"BLE", "NET", "STP", "PWR"};
    airmidi_mode_t mode = current_mode();
    if (ev == BUTTON_DOUBLE) {
        ESP_LOGI(TAG, "%s: double tap, all notes off", names[key]);
        panic();
        return;
    }
    if (ev != BUTTON_SHORT && ev != BUTTON_LONG) return;
    bool hold = ev == BUTTON_LONG;
    ESP_LOGI(TAG, "%s: %s", names[key], hold ? "hold" : "tap");
    switch (key) {
        case PANEL_BLE:  // tap: BLE mode (Play); hold: pairing window, in BLE mode
            if (!hold) {
                set_mode(AIRMIDI_MODE_PLAY);
            } else if (ble_running) {
                if (mode == AIRMIDI_MODE_NETWORK) set_mode(AIRMIDI_MODE_PLAY);
                pair();
            } else {
                kept.after = AFTER_PAIR;
                set_mode(AIRMIDI_MODE_PLAY);  // restarts: BLE comes on
            }
            break;
        case PANEL_NET:  // tap: Network mode; hold: check for an update, in Network mode
            if (hold) {
                if (mode != AIRMIDI_MODE_NETWORK) kept.after = AFTER_UPDATE;  // in case the switch restarts
                set_mode(AIRMIDI_MODE_NETWORK);
                kept.after = AFTER_NOTHING;  // no restart: do it here
                update_check();
            } else {
                set_mode(AIRMIDI_MODE_NETWORK);
            }
            break;
        case PANEL_STP:  // tap: Setup mode, again: back to the mode before; hold: MIDI-CI scan
            if (hold) {
                ci_scan(false);
            } else {
                set_mode(mode == AIRMIDI_MODE_SETUP ? (airmidi_mode_t)kept.before_setup : AIRMIDI_MODE_SETUP);
            }
            break;
        case PANEL_PWR:  // tap: battery bar; hold: off
            if (hold)
                power_off_now();
            else
                ui_show_battery();
            break;
    }
}

// After a restart a panel hold asked for.
static void kept_after_boot(void) {
    after_restart_t a = kept.after;
    kept.after = AFTER_NOTHING;
    if (a == AFTER_PAIR && ble_running) pair();
    if (a == AFTER_UPDATE) update_check();
}

static void panel_update(void) {
    ci_status_t ci;
    ci_get_status(&ci);
    if (pnl.was_scanning && !ci.scanning) {
        bool found = !strcmp(ci.result, "ok") || !strcmp(ci.result, "cached");
        pnl.scan_result = found ? PANEL_RESULT_OK : PANEL_RESULT_FAILED;
        pnl.scan_at = ms();
    }
    pnl.was_scanning = ci.scanning;
    update_poll();

    net_state_t ns = net_state();
    uint32_t window = ble_running ? ble_config_window_left() : 0;
    ota_fetch_status_t fs;
    ota_fetch_status(&fs);
    panel_state_t p = {
        .ble_on = ble_running,
        .ble_connected = ble_running && ble_midi_connected(),
        .ble_pairing = window && window != BLE_CONFIG_WINDOW_FOREVER,
        .net = ns == NET_JOINING     ? PANEL_NET_JOINING
               : ns == NET_CONNECTED ? PANEL_NET_JOINED
               : ns == NET_AP        ? PANEL_NET_AP
                                     : PANEL_NET_OFF,
        .net_session = bus_is_up(PORT_NET_0) || bus_is_up(PORT_NET_1) || bus_is_up(PORT_RTP_0) || bus_is_up(PORT_RTP_1),
        .update_checking = pnl.update_checking || pnl.update_waiting,
        .update_installing = !strcmp(fs.state, "downloading"),
        .update_result = pnl.update_result,
        .update_at = pnl.update_at,
        .scanning = ci.scanning,
        .scan_result = pnl.scan_result,
        .scan_at = pnl.scan_at,
        .battery_absent = power_on_usb(),
        .battery_low = power_low(),
        .battery_percent = power_percent(),
    };
    ui_set_panel(&p);
}

// ---- rev A: one button --------------------------------------------------------------------------------------

static void handle_button(button_event_t ev) {
    switch (ev) {
        case BUTTON_DOUBLE:
            ESP_LOGI(TAG, "button: double press, all notes off");
            panic();
            break;
        case BUTTON_SHORT:
            ESP_LOGI(TAG, "button: short press, MIDI-CI scan");
            ci_scan(false);
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
    static const char *names[PORT_COUNT] = {"trs",  "ble0", "ble1", "blec", "net0",   "net1",
                                            "rtp0", "rtp1", "web",  "usb",  "espnow", "ci"};
    for (int p = 0; p < PORT_COUNT; p++) {
        bus_port_stats_t s;
        bus_get_stats(p, &s);
        if (s.in || s.out || s.dropped)
            printf("port %s: in %lu, out %lu, dropped %lu, coalesced %lu\n", names[p], (unsigned long)s.in,
                   (unsigned long)s.out, (unsigned long)s.dropped, (unsigned long)s.coalesced);
    }
    espnow_midi_info_t en;
    espnow_midi_get_info(&en);
    if (en.running) {
        printf(
            "espnow: channel %u%s, %d peer(s)%s%s, rx %lu frames (%lu lost, %lu dups) %lu hellos, tx %lu (%lu "
            "refused, last %s)\n",
            en.channel, en.on_ap_channel ? " (access point)" : "", en.n_peers, en.n_peers ? ", first " : "",
            en.n_peers ? en.peers[0] : "", (unsigned long)en.stats.rx_frames, (unsigned long)en.stats.rx_lost,
            (unsigned long)en.stats.rx_dups, (unsigned long)en.stats.rx_hellos, (unsigned long)en.stats.tx_frames,
            (unsigned long)en.tx_failed, esp_err_to_name(en.last_tx_error));
        printf("espnow: unicast acknowledged %lu, not %lu\n", (unsigned long)en.tx_acked, (unsigned long)en.tx_unacked);
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
    kept_check();
    // Woken from deep sleep by PWR: only a hold starts the box, a knock sends it back to sleep (before the radios).
    if (b->panel) power_confirm_wake(b->panel_button[PANEL_PWR]);
    settings_ok = nvs_settings_init() == ESP_OK;
    settings_t s;
    nvs_settings_get(&s);

    bus_start(s.sysex_timeout_ms);
    usb_midi_start(s.name);  // S3: the console moves to USB from here on
    uart_midi_start();
    ci_start();
    ble_running = ble_wanted(&s);
    if (ble_running) ble_midi_start(s.name, s.ble_device);
    net_keep_radio_on(s.espnow);  // ESP-NOW needs the radio in Play mode too
    net_start(s.mode, s.name);
    if (s.espnow) espnow_midi_start(s.name, s.espnow_group);
    if (ble_running) ble_midi_set_slow_advertising(s.mode != AIRMIDI_MODE_PLAY);
    if (s.mode == AIRMIDI_MODE_NETWORK) start_network_midi();
    api_init(&(api_hooks_t){.set_mode = set_mode, .web_dropped = webui_ws_dropped});
    webui_start();
    ble_config_start(ble_api);
    // Pairing window for a new phone: Setup mode, or 2 min after the box was switched on or reset by hand (not after
    // its own restarts, which a mode change or a settings change causes).
    esp_reset_reason_t why = esp_reset_reason();
    if (s.mode == AIRMIDI_MODE_SETUP)
        ble_config_set_window(BLE_CONFIG_WINDOW_FOREVER);
    else if (why == ESP_RST_POWERON || why == ESP_RST_EXT)
        ble_config_set_window(120);
    power_start();
    button_events = xQueueCreate(8, sizeof(uint16_t));
    ui_start(on_button);
    if (b->panel) kept_after_boot();
    // A freshly updated image proves itself before it is kept: radio stack up, settings readable, still running
    // after 10 s. A crash or a failed check before then boots the previous image again.
    ota_confirm_after(10000, healthy);
#if CONFIG_AIRMIDI_TEST_CRASH
    esp_timer_handle_t t;
    esp_timer_create(&(esp_timer_create_args_t){.callback = crash, .name = "crash"}, &t);
    esp_timer_start_once(t, 3000000);
#endif

    for (;;) {
        uint16_t e;
        while (xQueueReceive(button_events, &e, 0)) {
            if (b->panel)
                handle_panel(e >> 8, (button_event_t)(e & 0xff));
            else
                handle_button((button_event_t)(e & 0xff));
        }

        if (power_empty()) {
            panic();
            ui_dark();
            vTaskDelay(pdMS_TO_TICKS(200));  // let the notes-off reach the outputs
            power_shutdown();
        }
        if (b->panel) {
            panel_update();
        } else {
            net_state_t ns = net_state();
            ui_set_led(power_low()            ? LED_LOW_BATTERY
                       : ns == NET_AP         ? LED_SETUP
                       : ns == NET_JOINING    ? LED_WIFI_JOINING
                       : ble_midi_connected() ? LED_CONNECTED
                                              : LED_ADVERTISING);
        }

        int c = getchar();              // console: 's' counters, 'i' image info, 'P'/'N'/'S' mode, 'm' merge test hook,
                                        // '0'-'2' power mode, 'B' bootloader (S3)
        if (c == EOF) clearerr(stdin);  // no input yet; on a UART console EOF would otherwise stick
        // TinyUSB's console drops output nobody reads (a short write), which leaves stdout in its error state for
        // good: the box ran on but never printed again (S3). Clear it, so output resumes when someone reads.
        if (ferror(stdout)) clearerr(stdout);
        if (c == 's') print_status();
        if (c == 'i') ota_boot_report();  // which slot runs, its OTA state, core dump
        if (c == 'P') set_mode(AIRMIDI_MODE_PLAY);
        if (c == 'N') set_mode(AIRMIDI_MODE_NETWORK);
        if (c == 'S') set_mode(AIRMIDI_MODE_SETUP);
        if (c == 'd') ci_scan(false);  // MIDI-CI scan ('D': read everything again)
        if (c == 'D') ci_scan(true);
        if (c == 'm') xTaskCreate(inject_task, "inject", 2048, NULL, 5, NULL);
        if (c >= '0' && c <= '2') power_set_mode(c - '0');  // power modes, for measuring ('2' needs RESET after)
        if (c == 'B') usb_midi_bootloader();                // S3: restart into the ROM bootloader (flashing over USB)
        if (c == 'E') toggle_espnow();
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
