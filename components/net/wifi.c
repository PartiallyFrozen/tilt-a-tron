#include "net/net.h"

#include <string.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "mdns.h"
#include "esp_sntp.h"
#include "esp_http_client.h"
#include <stdlib.h>
#include "nvs.h"

static const char *TAG = "net";

#define BIT_GOT_IP BIT0
#define BIT_DISCONNECTED BIT1

static EventGroupHandle_t s_events;
static bool s_inited, s_started, s_mdns, s_sntp;

// The clock app reads the system time; NTP keeps it right whenever we're online.
static void on_time_sync(struct timeval *tv)
{
    ESP_LOGI(TAG, "time synced from the network (UTC epoch %lld)", (long long)tv->tv_sec);
}

// The offset from UTC for wherever this network is (daylight saving included),
// asked once per boot from ip-api.com. The clock uses it unless a zone was set by hand.
static volatile int s_tz_min;
static volatile bool s_tz_known;

bool net_tz_offset_min(int *minutes)
{
    if (!s_tz_known) return false;
    *minutes = s_tz_min;
    return true;
}

static void tz_task(void *arg)
{
    char body[128] = "";
    for (int attempt = 0; attempt < 3 && !s_tz_known; attempt++) {
        const esp_http_client_config_t cfg = {.url = "http://ip-api.com/json/?fields=status,offset", .timeout_ms = 6000};
        esp_http_client_handle_t c = esp_http_client_init(&cfg);
        if (c && esp_http_client_open(c, 0) == ESP_OK) {
            esp_http_client_fetch_headers(c);
            const int n = esp_http_client_read_response(c, body, sizeof(body) - 1);
            if (n > 0) {
                body[n] = 0;
                const char *p = strstr(body, "\"offset\":");
                if (p && strstr(body, "success")) {
                    s_tz_min = atoi(p + 9) / 60;
                    s_tz_known = true;
                    ESP_LOGI(TAG, "time zone from the network: UTC%+d:%02d", s_tz_min / 60, abs(s_tz_min % 60));
                }
            }
        }
        if (c) {
            esp_http_client_close(c);
            esp_http_client_cleanup(c);
        }
        if (!s_tz_known) vTaskDelay(pdMS_TO_TICKS(5000));
    }
    if (!s_tz_known) ESP_LOGW(TAG, "couldn't get the time zone from the network");
    vTaskDelete(NULL);
}

static void start_sntp(void)
{
    if (s_sntp) return;
    s_sntp = true;
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_set_time_sync_notification_cb(on_time_sync);
    esp_sntp_init();
    xTaskCreatePinnedToCore(tz_task, "tz", 4096, NULL, 2, NULL, 0);
}
static char s_ip[16];
static volatile net_state_t s_state = NET_OFF;

// Background reconnect: after a drop, retry with backoff. Give up (FAILED) after
// repeated auth failures so a changed password doesn't burn the battery forever.
static volatile bool s_auto;          // background reconnect active
static volatile bool s_manual_join;   // a blocking net_join() owns the connection
static int s_fail_count;
static esp_timer_handle_t s_retry_timer;

static void retry_cb(void *arg)
{
    if (s_auto && !s_manual_join) esp_wifi_connect();
}

static void start_mdns(void)
{
    if (s_mdns || mdns_init() != ESP_OK) return;
    mdns_hostname_set(NET_HOSTNAME);
    mdns_instance_name_set("Tilt-a-tron");
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    s_mdns = true;
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *d = data;
        s_ip[0] = 0;
        xEventGroupSetBits(s_events, BIT_DISCONNECTED);
        if (s_auto && !s_manual_join) {
            const bool auth = d->reason == WIFI_REASON_AUTH_FAIL || d->reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
                              d->reason == WIFI_REASON_HANDSHAKE_TIMEOUT;
            s_fail_count++;
            if (auth && s_fail_count >= 5) {
                ESP_LOGW(TAG, "saved network keeps rejecting us (reason %d)", d->reason);
                s_state = NET_FAILED;
                return;
            }
            s_state = NET_CONNECTING;
            // 1 s, 2 s, 4 s ... capped at 30 s
            int64_t delay = 1000000LL << (s_fail_count > 5 ? 5 : s_fail_count - 1);
            if (delay > 30000000) delay = 30000000;
            esp_timer_stop(s_retry_timer);
            esp_timer_start_once(s_retry_timer, delay);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = data;
        esp_ip4addr_ntoa(&e->ip_info.ip, s_ip, sizeof(s_ip));
        s_fail_count = 0;
        s_state = NET_CONNECTED;
        xEventGroupSetBits(s_events, BIT_GOT_IP);
        start_mdns();
        start_sntp();
        // Accept firmware updates whenever we're on Wi-Fi: with the USB port used as
        // the theme drive, this is the way in.
        ota_server_start();
        ESP_LOGI(TAG, "connected, ip %s", s_ip);
    }
}

static esp_err_t init_once(void)
{
    if (s_inited) return ESP_OK;
    if (!s_events) s_events = xEventGroupCreate();
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif");
    // These two are fine to have already: a previous attempt may have got this far.
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) ESP_RETURN_ON_ERROR(err, TAG, "event loop");
    static esp_netif_t *sta;
    if (!sta) {
        sta = esp_netif_create_default_wifi_sta();
        esp_netif_set_hostname(sta, NET_HOSTNAME);
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi init failed (%s) - internal RAM free: %u bytes", esp_err_to_name(err),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        return err;
    }
    esp_wifi_set_storage(WIFI_STORAGE_RAM);   // credentials live in our own NVS namespace
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, on_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "mode");

    const esp_timer_create_args_t t = {.callback = retry_cb, .name = "wifi_retry"};
    esp_timer_create(&t, &s_retry_timer);
    s_inited = true;
    return ESP_OK;
}

static esp_err_t radio_on(void)
{
    ESP_RETURN_ON_ERROR(init_once(), TAG, "init");
    if (!s_started) {
        ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start");
        esp_wifi_set_ps(WIFI_PS_MIN_MODEM);   // light on the radio while games run
        s_started = true;
    }
    return ESP_OK;
}

// ---------------------------------------------------------------- credentials

static bool load_creds(const char *ns, char *ssid, size_t slen, char *pass, size_t plen)
{
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READONLY, &h) != ESP_OK) return false;
    size_t sl = slen, pl = plen;
    bool ok = nvs_get_str(h, "ssid", ssid, &sl) == ESP_OK && ssid[0];
    if (ok && nvs_get_str(h, "pass", pass, &pl) != ESP_OK) pass[0] = 0;
    nvs_close(h);
    return ok;
}

static void save_creds(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, "ssid", ssid);
    nvs_set_str(h, "pass", pass);
    nvs_commit(h);
    nvs_close(h);
}

static bool get_creds(char *ssid, size_t slen, char *pass, size_t plen)
{
    if (load_creds("wifi", ssid, slen, pass, plen)) return true;
    // This board may have run PeakPal before, which saved Wi-Fi in its own namespace.
    if (load_creds("peakpal", ssid, slen, pass, plen)) {
        ESP_LOGI(TAG, "imported saved Wi-Fi '%s' from PeakPal settings", ssid);
        save_creds(ssid, pass);
        return true;
    }
    return false;
}

bool net_saved_ssid(char *out, size_t len)
{
    char pass[65];
    return get_creds(out, len, pass, sizeof(pass));
}

// ---------------------------------------------------------------- on/off

bool net_enabled(void)
{
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open("wifi", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "enabled", &v);
        nvs_close(h);
    }
    return v;
}

static void save_enabled(bool on)
{
    nvs_handle_t h;
    // Said out loud when it fails: a switch that does not stay switched looks, from outside,
    // exactly like a switch nobody touched.
    esp_err_t err = nvs_open("wifi", NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_u8(h, "enabled", on);
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }
    if (err == ESP_OK) ESP_LOGI(TAG, "WI-FI switched %s", on ? "on" : "off");
    else ESP_LOGE(TAG, "could not save WI-FI %s: %s", on ? "on" : "off", esp_err_to_name(err));
}

static void apply_config(const char *ssid, const char *pass)
{
    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, pass, sizeof(wc.sta.password));
    wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wc.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    esp_wifi_set_config(WIFI_IF_STA, &wc);
}

static bool s_forced;

void net_start_forced(void)
{
    s_forced = true;
    net_start();
}

void net_start(void)
{
    if (!net_enabled() && !s_forced) {
        ESP_LOGI(TAG, "not started: WI-FI is switched off in Settings");
        s_state = NET_OFF;
        return;
    }
    char ssid[33], pass[65];
    if (!get_creds(ssid, sizeof(ssid), pass, sizeof(pass))) {
        ESP_LOGW(TAG, "not started: switched on, but no network has been chosen");
        s_state = NET_NO_NETWORK;
        return;
    }
    if (radio_on() != ESP_OK) {
        ESP_LOGE(TAG, "not started: the radio would not come up (%u bytes of internal RAM free)",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        s_state = NET_FAILED;
        return;
    }
    if (s_state == NET_CONNECTED) return;
    apply_config(ssid, pass);
    s_fail_count = 0;
    s_auto = true;
    s_state = NET_CONNECTING;
    esp_wifi_connect();
    ESP_LOGI(TAG, "connecting to '%s' in the background", ssid);
}

void net_set_enabled(bool on)
{
    save_enabled(on);
    if (on) {
        net_start();
        return;
    }
    s_auto = false;
    if (s_started) {
        esp_timer_stop(s_retry_timer);
        esp_wifi_disconnect();
        esp_wifi_stop();
        s_started = false;
    }
    s_ip[0] = 0;
    s_state = NET_OFF;
    ESP_LOGI(TAG, "wifi off");
}

static bool s_suspended;

void net_suspend(void)
{
    if (!s_started) return;
    s_suspended = true;
    s_auto = false;
    esp_timer_stop(s_retry_timer);
    esp_wifi_disconnect();
    esp_wifi_stop();
    s_started = false;
    s_ip[0] = 0;
    s_state = NET_CONNECTING;
}

void net_resume(void)
{
    if (!s_suspended) return;
    s_suspended = false;
    net_start();
}

net_state_t net_state(void) { return s_state; }
const char *net_ip(void) { return s_ip; }

// ---------------------------------------------------------------- blocking join / scan

static bool join_blocking(const char *ssid, const char *pass, uint32_t timeout_ms)
{
    if (radio_on() != ESP_OK) return false;
    s_manual_join = true;
    esp_timer_stop(s_retry_timer);
    esp_wifi_disconnect();
    apply_config(ssid, pass);
    xEventGroupClearBits(s_events, BIT_GOT_IP | BIT_DISCONNECTED);
    s_state = NET_CONNECTING;
    esp_wifi_connect();

    bool ok = false;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    for (;;) {
        TickType_t now = xTaskGetTickCount();
        if (now >= deadline) break;
        EventBits_t b = xEventGroupWaitBits(s_events, BIT_GOT_IP | BIT_DISCONNECTED, pdTRUE, pdFALSE, deadline - now);
        if (b & BIT_GOT_IP) {
            ok = true;
            break;
        }
        if (b & BIT_DISCONNECTED) {   // association often fails once or twice; keep trying
            vTaskDelay(pdMS_TO_TICKS(300));
            esp_wifi_connect();
        }
    }
    s_manual_join = false;
    if (!ok) {
        ESP_LOGW(TAG, "could not join '%s'", ssid);
        esp_wifi_disconnect();
        s_state = NET_FAILED;
    }
    return ok;
}

bool net_join(const char *ssid, const char *pass, uint32_t timeout_ms)
{
    if (!join_blocking(ssid, pass, timeout_ms)) {
        // Fall back to the previously saved network, if Wi-Fi is on.
        if (net_enabled()) net_start();
        return false;
    }
    save_creds(ssid, pass);
    save_enabled(true);
    s_fail_count = 0;
    s_auto = true;
    return true;
}

bool net_join_saved(uint32_t timeout_ms)
{
    char ssid[33], pass[65];
    if (!get_creds(ssid, sizeof(ssid), pass, sizeof(pass))) return false;
    if (!join_blocking(ssid, pass, timeout_ms)) return false;
    s_auto = true;
    return true;
}

int net_scan(net_ap_t *out, int max)
{
    if (radio_on() != ESP_OK) return 0;
    if (esp_wifi_scan_start(NULL, true) != ESP_OK) return 0;
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n > 40) n = 40;
    wifi_ap_record_t recs[40];
    esp_wifi_scan_get_ap_records(&n, recs);   // already sorted strongest first

    int count = 0;
    for (int i = 0; i < n && count < max; i++) {
        const char *ssid = (const char *)recs[i].ssid;
        if (!ssid[0]) continue;
        bool dup = false;
        for (int j = 0; j < count && !dup; j++) dup = strcmp(out[j].ssid, ssid) == 0;
        if (dup) continue;
        strlcpy(out[count].ssid, ssid, sizeof(out[count].ssid));
        out[count].rssi = recs[i].rssi;
        out[count].secure = recs[i].authmode != WIFI_AUTH_OPEN;
        count++;
    }
    return count;
}

// ---------------------------------------------------------------- update mode flag

void net_request_update_mode(void)
{
    nvs_handle_t h;
    if (nvs_open("console", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "update", 1);
        nvs_commit(h);
        nvs_close(h);
    }
    esp_restart();
}

bool net_take_update_mode(void)
{
    nvs_handle_t h;
    if (nvs_open("console", NVS_READWRITE, &h) != ESP_OK) return false;
    uint8_t v = 0;
    bool set = nvs_get_u8(h, "update", &v) == ESP_OK && v;
    if (set) {
        nvs_erase_key(h, "update");
        nvs_commit(h);
    }
    nvs_close(h);
    return set;
}
