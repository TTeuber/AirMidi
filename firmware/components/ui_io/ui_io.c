#include "ui_io.h"

#include "board.h"
#include "bus.h"
#include "driver/gpio.h"
#include "esp_timer.h"

#define PERIOD_US 10000

static button_t button;
static void (*handler)(button_event_t);
static volatile led_state_t led = LED_ADVERTISING;

static void tick(void *arg) {
    const board_t *b = board();
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    button_event_t ev = button_update(&button, gpio_get_level(b->button) == 0, now);
    if (ev && handler) handler(ev);
    gpio_set_level(b->led, led_level(led, now, bus_last_activity_ms()));
}

void ui_start(void (*on_button)(button_event_t ev)) {
    handler = on_button;
    button_init(&button);
    const esp_timer_create_args_t args = {.callback = tick, .name = "ui"};
    esp_timer_handle_t t;
    ESP_ERROR_CHECK(esp_timer_create(&args, &t));
    ESP_ERROR_CHECK(esp_timer_start_periodic(t, PERIOD_US));
}

void ui_set_led(led_state_t s) { led = s; }
