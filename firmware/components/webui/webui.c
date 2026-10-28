#include "webui.h"

#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "api.h"
#include "bus.h"
#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net.h"
#include "ota.h"

static const char *TAG = "web";

static httpd_handle_t server;

// ---- static files (gzipped at build time, see CMakeLists.txt) -----------------------------------------------

#define EMBEDDED(sym)                                                 \
    extern const uint8_t sym##_start[] asm("_binary_" #sym "_start"); \
    extern const uint8_t sym##_end[] asm("_binary_" #sym "_end");

EMBEDDED(index_html_gz)
EMBEDDED(app_js_gz)
EMBEDDED(style_css_gz)
EMBEDDED(geist_medium_woff2_gz)

typedef struct {
    const char *path, *type;
    const uint8_t *start, *end;
} static_file_t;

static const static_file_t files[] = {
    {"/", "text/html", index_html_gz_start, index_html_gz_end},
    {"/app.js", "text/javascript", app_js_gz_start, app_js_gz_end},
    {"/style.css", "text/css", style_css_gz_start, style_css_gz_end},
    {"/geist-medium.woff2", "font/woff2", geist_medium_woff2_gz_start, geist_medium_woff2_gz_end},
};

static esp_err_t serve_file(httpd_req_t *req) {
    const static_file_t *f = req->user_ctx;
    httpd_resp_set_type(req, f->type);
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    // The files are part of the firmware: they change only with an update, which restarts the box.
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, (const char *)f->start, f->end - f->start);
}

// ---- helpers -------------------------------------------------------------------------------------------------

static esp_err_t send_json(httpd_req_t *req, cJSON *root) {
    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    esp_err_t e = httpd_resp_sendstr(req, text);
    free(text);
    return e;
}

static esp_err_t send_error(httpd_req_t *req, const char *status, const char *message) {
    httpd_resp_set_status(req, status);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "error", message);
    return send_json(req, root);
}

static void restart_soon(void *arg) { esp_restart(); }

static void restart_after(uint32_t ms) {
    const esp_timer_create_args_t a = {.callback = restart_soon, .name = "restart"};
    esp_timer_handle_t t;
    if (esp_timer_create(&a, &t) == ESP_OK) esp_timer_start_once(t, (uint64_t)ms * 1000);
}

static volatile uint32_t ws_dropped;  // words for the pages that did not fit (one that cannot keep up)

// ---- /api/update, /api/coredump ------------------------------------------------------------------------------

// The image streams straight into the other OTA slot; nothing larger than 4 KB is held in RAM.
static esp_err_t post_update(httpd_req_t *req) {
    if (req->content_len <= 0) return send_error(req, "400 Bad Request", "empty upload");
    esp_err_t e = ota_begin();
    if (e) return send_error(req, "500 Internal Server Error", esp_err_to_name(e));
    static char buf[4096];
    int left = req->content_len;
    while (left > 0) {
        int n = httpd_req_recv(req, buf, left < (int)sizeof buf ? left : (int)sizeof buf);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (n <= 0 || (e = ota_write(buf, n)) != ESP_OK) {
            ota_abort();
            return send_error(req, "400 Bad Request", n <= 0 ? "upload interrupted" : esp_err_to_name(e));
        }
        left -= n;
    }
    e = ota_end();
    if (e == ESP_ERR_OTA_VALIDATE_FAILED)
        return send_error(req, "400 Bad Request", "not a valid image for this board, or not signed with our key");
    if (e) return send_error(req, "500 Internal Server Error", esp_err_to_name(e));
    httpd_resp_sendstr(req, "{\"ok\":true}");
    restart_after(500);
    return ESP_OK;
}

static bool chunk_sink(void *ctx, const void *data, size_t len) {
    return httpd_resp_send_chunk(ctx, data, len) == ESP_OK;
}

static esp_err_t get_coredump(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"airmidi-coredump.elf\"");
    if (!ota_coredump_read(chunk_sink, req)) return send_error(req, "404 Not Found", "no core dump");
    return httpd_resp_send_chunk(req, NULL, 0);
}

// ---- /api/...: everything JSON goes through api_call() (components/api), the same as the BLE config service ----

static esp_err_t api_handler(httpd_req_t *req) {
    static const char *methods[] = {[HTTP_GET] = "GET", [HTTP_POST] = "POST", [HTTP_DELETE] = "DELETE"};
    if (req->content_len > API_BODY_MAX) return send_error(req, "413 Payload Too Large", "16 KB at most");
    char *body = NULL;
    if (req->content_len > 0) {
        body = malloc(req->content_len);
        if (!body) return send_error(req, "500 Internal Server Error", "out of memory");
        int got = 0;
        while (got < req->content_len) {
            int n = httpd_req_recv(req, body + got, req->content_len - got);
            if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
            if (n <= 0) {
                free(body);
                return ESP_FAIL;
            }
            got += n;
        }
    }
    api_reply_t r = api_call(methods[req->method], req->uri, body, req->content_len > 0 ? req->content_len : 0);
    free(body);
    if (!r.json) return send_error(req, "500 Internal Server Error", "out of memory");
    httpd_resp_set_status(req, api_status_line(r.status));
    httpd_resp_set_type(req, "application/json");
    esp_err_t e = httpd_resp_sendstr(req, r.json);
    free(r.json);
    if (r.then) r.then(r.then_arg);  // answered first: a mode switch may take the network away
    return e;
}

uint32_t webui_ws_dropped(void) { return ws_dropped; }

// ---- /ws: the control surface's MIDI, as UMP ----------------------------------------------------------------
// Binary frames, each a run of UMP words, big-endian. From the browser they go on the bus as PORT_WEB; what the
// bus routes to PORT_WEB collects in ws_out and goes to every connected page from the server's task, one frame
// with everything collected since the last one. At most one send is queued at a time: the server's work queue is
// a UDP socket with a 6-message mailbox, and queuing a frame per bus batch overflowed it as soon as SysEx came in
// from TRS (a batch every 2 ms), losing frames.

#define WS_CLIENTS 4
static int ws_fds[WS_CLIENTS];
static int n_ws;
static portMUX_TYPE ws_lock = portMUX_INITIALIZER_UNLOCKED;  // clients, ws_out, ws_queued: bus and httpd tasks
static uint32_t ws_out[1024];                                // a 788-byte control list is ~260 words
static int n_ws_out;
static bool ws_queued;  // a send is waiting in the server's queue

static void ws_add(int fd) {
    bool first = false, known = false;
    taskENTER_CRITICAL(&ws_lock);
    for (int i = 0; i < n_ws; i++) known |= ws_fds[i] == fd;
    if (!known && n_ws < WS_CLIENTS) first = n_ws == 0, ws_fds[n_ws++] = fd;
    taskEXIT_CRITICAL(&ws_lock);
    if (first) bus_set_up(PORT_WEB, true);
}

// ESP-IDF 5.5 does not call the URI handler for the handshake itself; this callback runs right after it.
static esp_err_t ws_on_connect(httpd_req_t *req) {
    ws_add(httpd_req_to_sockfd(req));
    return ESP_OK;
}

static void ws_remove(int fd) {
    bool last = false;
    taskENTER_CRITICAL(&ws_lock);
    for (int i = 0; i < n_ws; i++) {
        if (ws_fds[i] == fd) {
            ws_fds[i] = ws_fds[--n_ws];
            last = n_ws == 0;
            break;
        }
    }
    taskEXIT_CRITICAL(&ws_lock);
    if (last) bus_set_up(PORT_WEB, false);
}

static void on_close(httpd_handle_t hd, int fd) {
    ws_remove(fd);
    close(fd);
}

static esp_err_t ws_handler(httpd_req_t *req) {
    ws_add(httpd_req_to_sockfd(req));  // normally already known from ws_on_connect
    httpd_ws_frame_t f = {0};
    if (httpd_ws_recv_frame(req, &f, 0) != ESP_OK) return ESP_FAIL;
    if (f.type != HTTPD_WS_TYPE_BINARY || f.len == 0 || f.len > 512 || f.len % 4) {
        uint8_t skip[64];  // anything else: read and drop it
        f.payload = skip;
        return f.len <= sizeof skip ? httpd_ws_recv_frame(req, &f, f.len) : ESP_OK;
    }
    uint8_t buf[512];
    f.payload = buf;
    if (httpd_ws_recv_frame(req, &f, f.len) != ESP_OK) return ESP_FAIL;
    int words = f.len / 4;
    for (int k = 0; k < words;) {
        ump_t u = {{0}};
        int nw = ump_words_for_mt(buf[4 * k] >> 4);
        if (k + nw > words) break;
        for (int j = 0; j < nw; j++)
            u.w[j] = (uint32_t)buf[4 * (k + j)] << 24 | (uint32_t)buf[4 * (k + j) + 1] << 16 |
                     (uint32_t)buf[4 * (k + j) + 2] << 8 | buf[4 * (k + j) + 3];
        bus_send(PORT_WEB, &u);
        k += nw;
    }
    return ESP_OK;
}

// Bus task.
static void web_write(void *ctx, const ump_t *u) {
    int nw = ump_words(u);
    taskENTER_CRITICAL(&ws_lock);
    if (n_ws_out + nw <= (int)(sizeof ws_out / sizeof ws_out[0])) {
        for (int j = 0; j < nw; j++) ws_out[n_ws_out++] = u->w[j];
    } else {
        ws_dropped += nw;
    }
    taskEXIT_CRITICAL(&ws_lock);
}

// Server task: send everything collected so far to every page.
static void ws_send_work(void *arg) {
    static uint8_t frame[4 * sizeof ws_out / sizeof ws_out[0]];
    int fds[WS_CLIENTS], n, words;
    taskENTER_CRITICAL(&ws_lock);
    words = n_ws_out;
    for (int k = 0; k < words; k++)
        frame[4 * k] = ws_out[k] >> 24, frame[4 * k + 1] = ws_out[k] >> 16, frame[4 * k + 2] = ws_out[k] >> 8,
                  frame[4 * k + 3] = ws_out[k];
    n_ws_out = 0;
    ws_queued = false;
    n = n_ws;
    memcpy(fds, ws_fds, sizeof fds);
    taskEXIT_CRITICAL(&ws_lock);
    httpd_ws_frame_t f = {.type = HTTPD_WS_TYPE_BINARY, .payload = frame, .len = 4 * words, .final = true};
    for (int i = 0; i < n && words; i++) httpd_ws_send_frame_async(server, fds[i], &f);
}

// Bus task, after a batch: queue one send unless one is already waiting (it will take these words too).
static void web_flush(void *ctx) {
    bool queue = false;
    taskENTER_CRITICAL(&ws_lock);
    if (n_ws_out && !ws_queued) queue = ws_queued = true;
    taskEXIT_CRITICAL(&ws_lock);
    if (queue && httpd_queue_work(server, ws_send_work, NULL) != ESP_OK) {
        taskENTER_CRITICAL(&ws_lock);
        ws_queued = false;  // try again after the next batch or bus tick
        taskEXIT_CRITICAL(&ws_lock);
    }
}

static const bus_output_ops_t web_ops = {.write = web_write, .flush = web_flush};

// ---- server --------------------------------------------------------------------------------------------------

void webui_start(void) {
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.uri_match_fn = httpd_uri_match_wildcard;  // "/api/*"
    cfg.max_uri_handlers = 28;
    cfg.stack_size = 6144;
    cfg.max_open_sockets = 7;     // + listen + control = 9 of CONFIG_LWIP_MAX_SOCKETS (16, see sdkconfig.defaults)
    cfg.lru_purge_enable = true;  // a phone that left without closing must not lock others out
    cfg.recv_wait_timeout = 10;
    cfg.close_fn = on_close;
    if (httpd_start(&server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server did not start");
        return;
    }
    for (size_t i = 0; i < sizeof files / sizeof files[0]; i++) {
        httpd_uri_t u = {
            .uri = files[i].path, .method = HTTP_GET, .handler = serve_file, .user_ctx = (void *)&files[i]};
        httpd_register_uri_handler(server, &u);
    }
    // The binary ones first (the server takes the first match), then every other /api/ path to api_handler.
    const httpd_uri_t binary[] = {
        {.uri = "/api/update", .method = HTTP_POST, .handler = post_update},
        {.uri = "/api/coredump", .method = HTTP_GET, .handler = get_coredump},
    };
    for (size_t i = 0; i < sizeof binary / sizeof binary[0]; i++) httpd_register_uri_handler(server, &binary[i]);
    for (httpd_method_t m = HTTP_DELETE; m <= HTTP_POST; m++) {  // DELETE 0, GET 1, HEAD 2, POST 3
        if (m == HTTP_HEAD) continue;
        httpd_uri_t u = {.uri = "/api/*", .method = m, .handler = api_handler};
        httpd_register_uri_handler(server, &u);
    }
    httpd_uri_t ws = {.uri = "/ws",
                      .method = HTTP_GET,
                      .handler = ws_handler,
                      .is_websocket = true,
                      .ws_post_handshake_cb = ws_on_connect};
    httpd_register_uri_handler(server, &ws);
    bus_register_output(PORT_WEB, &web_ops, NULL);
    net_add_service("_http", "_tcp", 80);
    ESP_LOGI(TAG, "web UI on port 80");
}

bool webui_running(void) { return server != NULL; }
