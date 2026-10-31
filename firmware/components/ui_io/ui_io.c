#include "ui_io.h"

#include <string.h>

#include "board.h"
#include "bus.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#define PERIOD_US 10000

static button_t buttons[PANEL_KEYS];
static bool
    swallow[PANEL_KEYS];  // a press that began before ui_start (the hold that woke the box): wait for its release
static int n_buttons;
static gpio_num_t pins[PANEL_KEYS];
static void (*handler)(int key, button_event_t ev);
static volatile led_state_t led = LED_ADVERTISING;
static volatile bool dark;

static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static panel_state_t panel;  // from main (ui_set_panel), plus what only this file knows: hold times, sweep, bar
static uint32_t bar_until, sweep_from;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static void panel_leds_init(const board_t *b) {
    // XTAL clock: steady while frequency scaling moves the APB clock (power mode 1).
    ledc_timer_config_t t = {.speed_mode = LEDC_LOW_SPEED_MODE,
                             .duty_resolution = LEDC_TIMER_8_BIT,
                             .timer_num = LEDC_TIMER_0,
                             .freq_hz = 2000,
                             .clk_cfg = LEDC_USE_XTAL_CLK};
    ESP_ERROR_CHECK(ledc_timer_config(&t));
    for (int k = 0; k < PANEL_KEYS; k++) {
        ledc_channel_config_t c = {.gpio_num = b->panel_led[k],
                                   .speed_mode = LEDC_LOW_SPEED_MODE,
                                   .channel = (ledc_channel_t)k,
                                   .timer_sel = LEDC_TIMER_0,
                                   .duty = 0};
        ESP_ERROR_CHECK(ledc_channel_config(&c));
    }
}

static void panel_show(uint32_t now) {
    uint8_t level[PANEL_KEYS] = {0};
    if (!dark) {
        panel_state_t s;
        taskENTER_CRITICAL(&lock);
        s = panel;
        taskEXIT_CRITICAL(&lock);
        for (int k = 0; k < PANEL_KEYS; k++) s.held_ms[k] = swallow[k] ? 0 : button_held_ms(&buttons[k], now);
        s.bar_until = bar_until;
        s.sweep_from = sweep_from;
        s.last_activity_ms = bus_last_activity_ms();
        panel_leds(&s, now, level);
    }
    for (int k = 0; k < PANEL_KEYS; k++) {
        ledc_set_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)k, level[k]);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)k);
    }
}

static void tick(void *arg) {
    const board_t *b = board();
    uint32_t now = now_ms();
    for (int k = 0; k < n_buttons; k++) {
        bool pressed = gpio_get_level(pins[k]) == 0;
        if (swallow[k]) {
            if (!pressed) swallow[k] = false;
            continue;
        }
        button_event_t ev = button_update(&buttons[k], pressed, now);
        if (ev && handler) handler(k, ev);
    }
    if (b->panel)
        panel_show(now);
    else if (b->led != GPIO_NUM_NC)
        gpio_set_level(b->led, !dark && led_level(led, now, bus_last_activity_ms()));
}

void ui_start(void (*on_button)(int key, button_event_t ev)) {
    const board_t *b = board();
    handler = on_button;
    if (b->panel) {
        n_buttons = PANEL_KEYS;
        memcpy(pins, b->panel_button, sizeof pins);
        uint64_t mask = 0;
        for (int k = 0; k < PANEL_KEYS; k++) mask |= 1ULL << pins[k];
        gpio_config_t in = {.pin_bit_mask = mask, .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE};
        ESP_ERROR_CHECK(gpio_config(&in));
        panel_leds_init(b);
        sweep_from = now_ms();
    } else if (b->button != GPIO_NUM_NC) {
        n_buttons = 1;
        pins[0] = b->button;  // board_init set it up
    }
    for (int k = 0; k < n_buttons; k++) {
        button_init(&buttons[k]);
        buttons[k].fast_double = b->panel;  // panic at the second press (one button: rev A's gestures as they were)
        swallow[k] = gpio_get_level(pins[k]) == 0;
    }
    const esp_timer_create_args_t args = {.callback = tick, .name = "ui"};
    esp_timer_handle_t t;
    ESP_ERROR_CHECK(esp_timer_create(&args, &t));
    ESP_ERROR_CHECK(esp_timer_start_periodic(t, PERIOD_US));
}

void ui_set_led(led_state_t s) { led = s; }

void ui_set_panel(const panel_state_t *s) {
    taskENTER_CRITICAL(&lock);
    panel = *s;
    taskEXIT_CRITICAL(&lock);
}

void ui_show_battery(void) { bar_until = now_ms() + PANEL_BAR_MS; }

void ui_dark(void) { dark = true; }

bool ui_button_down(int key) { return key < n_buttons && gpio_get_level(pins[key]) == 0; }
