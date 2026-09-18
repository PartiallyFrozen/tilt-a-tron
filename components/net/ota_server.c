#include "net/net.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include <stdarg.h>

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "ota";

static httpd_handle_t s_server;

// ---- recent log lines, readable over Wi-Fi at /log
#define LOG_RING_SIZE 16384
static char *s_log_ring;   // in PSRAM: internal RAM is for DMA buffers and Wi-Fi
static size_t s_log_head;      // next write position
static bool s_log_wrapped;
static portMUX_TYPE s_log_mux = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t s_prev_vprintf;
static char s_status_extra[448];
static net_screen_fn s_screen_fn;

// Someone is working with the watch over Wi-Fi: it mustn't doze off under them.
static volatile int64_t s_last_request_us = -1000000000;
static volatile bool s_transfer;
static void note_request(void) { s_last_request_us = esp_timer_get_time(); }
bool net_transfer_active(void) { return s_transfer; }
bool net_busy(void) { return s_transfer || esp_timer_get_time() - s_last_request_us < 120 * 1000000LL; }
static net_control_fn s_control_fn;

void net_set_control_hook(net_control_fn fn) { s_control_fn = fn; }

static esp_err_t input_get(httpd_req_t *req)
{
    note_request();
    char q[160] = "";
    httpd_req_get_url_query_str(req, q, sizeof(q));
    const bool ok = s_control_fn && s_control_fn(q);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, ok ? "ok" : "bad request");
}

void net_set_screen_hook(net_screen_fn fn) { s_screen_fn = fn; }

static esp_err_t screen_get(httpd_req_t *req)
{
    note_request();
    uint8_t *png = NULL;
    const size_t n = s_screen_fn ? s_screen_fn(&png) : 0;
    if (!n || !png) {
        free(png);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no screenshot");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, png[0] == 'B' ? "image/bmp" : "image/png");   // BMP when memory is short
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    const esp_err_t err = httpd_resp_send(req, (const char *)png, n);
    free(png);
    return err;
}

// Runs on whatever task is logging, some of which have tiny stacks, so the line
// buffer is shared (guarded by a mutex) rather than living on the caller's stack.
static char s_log_line[256];
static SemaphoreHandle_t s_log_lock;

static int log_capture_vprintf(const char *fmt, va_list args)
{
    if (s_log_lock && !xPortInIsrContext() && xSemaphoreTake(s_log_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
        va_list copy;
        va_copy(copy, args);
        int n = vsnprintf(s_log_line, sizeof(s_log_line), fmt, copy);
        va_end(copy);
        if (n > (int)sizeof(s_log_line) - 1) n = sizeof(s_log_line) - 1;
        if (n > 0) {
            portENTER_CRITICAL(&s_log_mux);
            for (int i = 0; i < n; i++) {
                s_log_ring[s_log_head++] = s_log_line[i];
                if (s_log_head == LOG_RING_SIZE) {
                    s_log_head = 0;
                    s_log_wrapped = true;
                }
            }
            portEXIT_CRITICAL(&s_log_mux);
        }
        xSemaphoreGive(s_log_lock);
    }
    return s_prev_vprintf ? s_prev_vprintf(fmt, args) : 0;
}

void net_log_capture_start(void)
{
    if (s_prev_vprintf) return;
    s_log_ring = heap_caps_malloc(LOG_RING_SIZE, MALLOC_CAP_SPIRAM);
    if (!s_log_ring) return;
    s_log_lock = xSemaphoreCreateMutex();
    s_prev_vprintf = esp_log_set_vprintf(log_capture_vprintf);
}

void net_status_set_extra(const char *json_fields)
{
    strlcpy(s_status_extra, json_fields ? json_fields : "", sizeof(s_status_extra));
}
static ota_status_t s_status;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static const char PAGE[] =
    "<!doctype html><meta name=viewport content='width=device-width'><title>Tilt-a-tron update</title>"
    "<body style='font-family:system-ui,sans-serif;background:#000;color:#eee;padding:24px;max-width:520px'>"
    "<h2>Tilt-a-tron firmware update</h2>"
    "<p><input type=file id=f accept='.bin'> <button onclick=go()>Upload</button></p><p id=s></p>"
    "<script>async function go(){const f=document.getElementById('f').files[0];if(!f)return;"
    "const s=document.getElementById('s');s.textContent='uploading '+(f.size/1024|0)+' KB...';"
    "try{const r=await fetch('/update',{method:'POST',body:f});s.textContent=await r.text();}"
    "catch(e){s.textContent='failed: '+e;}}</script>";

static void set_status(ota_state_t st, uint32_t rx, uint32_t total, const char *err)
{
    portENTER_CRITICAL(&s_mux);
    s_status.state = st;
    s_status.received = rx;
    s_status.total = total;
    if (err) strlcpy(s_status.error, err, sizeof(s_status.error));
    portEXIT_CRITICAL(&s_mux);
}

void ota_get_status(ota_status_t *out)
{
    portENTER_CRITICAL(&s_mux);
    *out = s_status;
    portEXIT_CRITICAL(&s_mux);
}

static esp_err_t page_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, PAGE, sizeof(PAGE) - 1);
}

static esp_err_t status_get(httpd_req_t *req)
{
    note_request();
    ota_status_t st;
    ota_get_status(&st);
    char *buf = heap_caps_malloc(1024, MALLOC_CAP_SPIRAM);
    if (!buf) return httpd_resp_send_500(req);
    const esp_app_desc_t *app = esp_app_get_description();
    // The ELF hash is unique to every build (the compile time isn't: it only
    // changes when that one file is recompiled), so it's how updates are verified.
    char sha[17];
    for (int i = 0; i < 8; i++) snprintf(sha + i * 2, 3, "%02x", app->app_elf_sha256[i]);
    snprintf(buf, 1024,
             "{\"app\":\"%s\",\"version\":\"%s\",\"sha\":\"%s\",\"built\":\"%s %s\",\"state\":%d,\"received\":%lu,\"total\":%lu,"
             "\"error\":\"%s\",\"uptime_s\":%lld,\"utc\":%lld,\"heap_internal\":%u,\"psram_free\":%u,\"psram_largest\":%u%s%s}",
             app->project_name, app->version, sha, app->date, app->time, st.state, (unsigned long)st.received,
             (unsigned long)st.total, st.error, esp_timer_get_time() / 1000000, (long long)time(NULL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM), s_status_extra[0] ? "," : "", s_status_extra);
    httpd_resp_set_type(req, "application/json");
    const esp_err_t err = httpd_resp_sendstr(req, buf);
    free(buf);
    return err;
}

static net_busy_fn s_reboot_guard;
void net_set_reboot_guard(net_busy_fn busy) { s_reboot_guard = busy; }

static esp_timer_handle_t s_reboot_timer;

static void reboot_cb(void *arg)
{
    if (s_reboot_guard && s_reboot_guard()) {
        // The drive is open on a computer: try again in a second, until it's ejected.
        ESP_LOGW(TAG, "reboot postponed: the drive is open on a computer");
        esp_timer_start_once(s_reboot_timer, 1000000);
        return;
    }
    esp_restart();
}

// Reboot in `us` microseconds, or once the drive is no longer open on a computer.
static void schedule_reboot(uint64_t us)
{
    if (!s_reboot_timer) {
        const esp_timer_create_args_t args = {.callback = reboot_cb, .name = "reboot"};
        esp_timer_create(&args, &s_reboot_timer);
    }
    if (s_reboot_timer) {
        esp_timer_stop(s_reboot_timer);
        esp_timer_start_once(s_reboot_timer, us);
    }
}

static esp_err_t log_get(httpd_req_t *req)
{
    note_request();
    httpd_resp_set_type(req, "text/plain");
    // Oldest part first, then the newest, so it reads in order.
    portENTER_CRITICAL(&s_log_mux);
    const size_t head = s_log_head;
    const bool wrapped = s_log_wrapped;
    portEXIT_CRITICAL(&s_log_mux);
    if (!s_log_ring) return httpd_resp_sendstr(req, "(log capture is off)");
    if (wrapped) httpd_resp_send_chunk(req, s_log_ring + head, LOG_RING_SIZE - head);
    if (head) httpd_resp_send_chunk(req, s_log_ring, head);
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t reboot_get(httpd_req_t *req)
{
    httpd_resp_sendstr(req, "rebooting");
    schedule_reboot(500000);
    return ESP_OK;
}

static esp_err_t fail(httpd_req_t *req, esp_ota_handle_t h, const char *msg)
{
    if (h) esp_ota_abort(h);
    ESP_LOGE(TAG, "update failed: %s", msg);
    set_status(OTA_FAILED, s_status.received, s_status.total, msg);
    httpd_resp_set_status(req, "500 Internal Server Error");
    return httpd_resp_sendstr(req, msg);
}

#define RX_BUF 8192
static char s_rx_buf[RX_BUF];   // out of the server task's stack

static esp_err_t update_post_inner(httpd_req_t *req)
{
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) return fail(req, 0, "no OTA partition");
    if (req->content_len == 0 || req->content_len > part->size) return fail(req, 0, "bad image size");

    esp_ota_handle_t h = 0;
    if (esp_ota_begin(part, req->content_len, &h) != ESP_OK) return fail(req, 0, "ota begin failed");
    ESP_LOGI(TAG, "receiving %u bytes into %s", (unsigned)req->content_len, part->label);
    esp_wifi_set_ps(WIFI_PS_NONE);   // full radio speed for the download
    set_status(OTA_RECEIVING, 0, req->content_len, "");

    char *buf = s_rx_buf;
    size_t remaining = req->content_len, received = 0;
    while (remaining > 0) {
        int n = httpd_req_recv(req, buf, remaining < RX_BUF ? remaining : RX_BUF);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (n <= 0) return fail(req, h, "connection lost");
        if (esp_ota_write(h, buf, n) != ESP_OK) return fail(req, h, "flash write failed");
        remaining -= n;
        received += n;
        set_status(OTA_RECEIVING, received, req->content_len, NULL);
    }

    esp_err_t err = esp_ota_end(h);
    if (err != ESP_OK) return fail(req, 0, err == ESP_ERR_OTA_VALIDATE_FAILED ? "image is not valid" : "ota end failed");
    if (esp_ota_set_boot_partition(part) != ESP_OK) return fail(req, 0, "set boot partition failed");

    set_status(OTA_DONE, received, req->content_len, NULL);
    ESP_LOGI(TAG, "update written, rebooting");
    httpd_resp_sendstr(req, "OK, rebooting");

    // The update screen reboots after showing "done"; this is the safety net.
    schedule_reboot(4000000);
    return ESP_OK;
}

// Uploads hold the watch awake from the first byte to the last (see net_busy).
static esp_err_t update_post(httpd_req_t *req)
{
    s_transfer = true;
    const esp_err_t err = update_post_inner(req);
    s_transfer = false;
    note_request();
    return err;
}

esp_err_t ota_server_start(void)
{
    if (s_server) return ESP_OK;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 8192;
    cfg.recv_wait_timeout = 15;
    cfg.max_uri_handlers = 12;
    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) return err;
    const httpd_uri_t uris[] = {
        {.uri = "/", .method = HTTP_GET, .handler = page_get},
        {.uri = "/status", .method = HTTP_GET, .handler = status_get},
        {.uri = "/log", .method = HTTP_GET, .handler = log_get},
        {.uri = "/reboot", .method = HTTP_GET, .handler = reboot_get},
        {.uri = "/screen", .method = HTTP_GET, .handler = screen_get},
        {.uri = "/input", .method = HTTP_GET, .handler = input_get},
        {.uri = "/update", .method = HTTP_POST, .handler = update_post},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) httpd_register_uri_handler(s_server, &uris[i]);
    ESP_LOGI(TAG, "update server listening on port 80");
    return ESP_OK;
}
