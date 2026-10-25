#include "bus.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "airmidi/ci_guard.h"
#include "airmidi/merge.h"
#include "airmidi/midi1.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "bus";

#define QUEUE_LEN 128
#define SRC_BROADCAST PORT_COUNT  // merge source number for bus_broadcast()
#define TICK_MS 10                // wake-up interval for SysEx timeouts and the watchdog

// Port state changes travel through the same queue as the messages, so only the bus task ever touches the
// outputs' merge state, and a port coming up is ordered with the traffic around it.
typedef enum { ITEM_MESSAGE, ITEM_BROADCAST, ITEM_UP, ITEM_DOWN } item_kind_t;

typedef struct {
    uint8_t kind, port;
    ump_t u;
} bus_msg_t;

typedef struct {
    const bus_output_ops_t *ops;
    void *ctx;
    volatile bool up;
    merge_t *merge;  // byte-stream outputs only (2.6 KB each), allocated when the output registers
    ci_guard_t guard;
    bus_port_stats_t stats;
} output_t;

static QueueHandle_t queue;
static router_t router;
static output_t outputs[PORT_COUNT];
static uint16_t sysex_timeout;
static volatile uint32_t last_activity, queue_dropped;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

// Write what an output's merge allows now, as far as the transport has room.
static void drain_one(output_t *o, uint32_t now) {
    ump_t u;
    while ((!o->up || !o->ops->room || o->ops->room(o->ctx) >= MIDI1_OUT_MAX) && merge_pop(o->merge, &u, now)) {
        if (o->up) o->ops->write(o->ctx, &u), o->stats.out++;
    }
}

static void deliver(int port, int src, const ump_t *u, uint32_t now) {
    output_t *o = &outputs[port];
    if (!o->ops || !o->up) return;
    if (!o->ops->byte_stream) {
        o->ops->write(o->ctx, u), o->stats.out++;
        return;
    }
    // MIDI-CI Discovery loops (lib/ci_guard): the box's own initiator is never held back.
    if (!ci_guard_pass(&o->guard, src, src != PORT_CI, u, now)) {
        if (ump_sysex7_status(u) != UMP_SYSEX_CONTINUE && ump_sysex7_status(u) != UMP_SYSEX_END)
            ESP_LOGI(TAG, "port %d: Discovery from port %d dropped, it came back within a second", port, src);
        o->stats.dropped++;
        return;
    }
    uint32_t before = o->merge->dropped;
    merge_push(o->merge, src, u, now);
    o->stats.dropped += o->merge->dropped - before;
    o->stats.coalesced = o->merge->coalesced;
    // Drain right away rather than after the whole batch: a long SysEx is hundreds of UMPs, more than the merge
    // queue holds. Writes only fill the transport's buffer; the BLE packet still goes out once, at flush time.
    drain_one(o, now);
}

// After a batch, and every tick: SysEx timeouts, outputs that had no room before, then flush (send BLE packets).
static void drain(uint32_t now) {
    for (int p = 0; p < PORT_COUNT; p++) {
        output_t *o = &outputs[p];
        if (!o->ops) continue;
        if (o->ops->byte_stream) {
            uint32_t before = o->merge->dropped;
            drain_one(o, now);
            o->stats.dropped += o->merge->dropped - before;
        }
        if (o->up && o->ops->flush) o->ops->flush(o->ctx);
    }
}

static void bus_task(void *arg) {
    esp_task_wdt_add(NULL);
    for (;;) {
        bus_msg_t m;
        bool got = xQueueReceive(queue, &m, pdMS_TO_TICKS(TICK_MS));
        uint32_t now = now_ms();
        // Route everything that is waiting, then write it out in one batch (one BLE packet per batch).
        while (got) {
            switch (m.kind) {
                case ITEM_MESSAGE: {
                    last_activity = now;
                    outputs[m.port].stats.in++;
                    int dst[PORT_COUNT];
                    ump_t out[PORT_COUNT];
                    int n = router_route(&router, m.port, &m.u, dst, out);
                    for (int i = 0; i < n; i++) deliver(dst[i], m.port, &out[i], now);
                    break;
                }
                case ITEM_BROADCAST:
                    for (int p = 0; p < PORT_COUNT; p++) deliver(p, SRC_BROADCAST, &m.u, now);
                    break;
                case ITEM_UP:
                case ITEM_DOWN: {
                    // A port coming up starts with an empty merge; one going down drops what it still held.
                    output_t *o = &outputs[m.port];
                    if (o->merge) merge_init(o->merge, sysex_timeout);
                    ci_guard_init(&o->guard);
                    o->up = m.kind == ITEM_UP;
                    if (o->up && o->ops && o->ops->on_up) o->ops->on_up(o->ctx);
                    ESP_LOGI(TAG, "port %d %s", m.port, o->up ? "up" : "down");
                    break;
                }
            }
            got = xQueueReceive(queue, &m, 0);
        }
        drain(now);
        esp_task_wdt_reset();
    }
}

void bus_start(uint16_t timeout_ms) {
    sysex_timeout = timeout_ms;
    router_init(&router);
    for (int p = 0; p < PORT_COUNT; p++) ci_guard_init(&outputs[p].guard);
    queue = xQueueCreate(QUEUE_LEN, sizeof(bus_msg_t));
    // Above the transports' own tasks so a burst in is routed before more piles up; 6 KB for the merge's copy.
    xTaskCreate(bus_task, "bus", 6144, NULL, 15, NULL);
    ESP_LOGI(TAG, "started, SysEx timeout %u ms", timeout_ms);
}

void bus_register_output(int port, const bus_output_ops_t *ops, void *ctx) {
    output_t *o = &outputs[port];
    if (ops->byte_stream && !o->merge) {
        o->merge = malloc(sizeof *o->merge);
        assert(o->merge);
        merge_init(o->merge, sysex_timeout);
    }
    o->ops = ops;
    o->ctx = ctx;
}

static bool enqueue(item_kind_t kind, int port, const ump_t *u, TickType_t wait) {
    bus_msg_t m = {.kind = kind, .port = (uint8_t)port};
    if (u) m.u = *u;
    if (xQueueSend(queue, &m, wait) == pdTRUE) return true;
    queue_dropped++;
    return false;
}

// State changes may wait a little for room: losing one would leave a port up or down for good.
void bus_set_up(int port, bool up) { enqueue(up ? ITEM_UP : ITEM_DOWN, port, NULL, pdMS_TO_TICKS(100)); }

bool bus_is_up(int port) { return outputs[port].up; }

bool bus_send(int src, const ump_t *u) { return src >= 0 && src < PORT_COUNT && enqueue(ITEM_MESSAGE, src, u, 0); }

bool bus_broadcast(const ump_t *u) { return enqueue(ITEM_BROADCAST, 0, u, 0); }

uint32_t bus_last_activity_ms(void) { return last_activity; }

void bus_get_stats(int port, bus_port_stats_t *out) { *out = outputs[port].stats; }

uint32_t bus_queue_dropped(void) { return queue_dropped; }
