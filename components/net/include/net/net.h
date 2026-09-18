// Wi-Fi + over-the-air firmware updates.
//
// Wi-Fi is off unless the user turns it on in Settings. When enabled it connects
// in the background at boot and keeps reconnecting (radio power-save on, so it
// costs games as little as possible).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NET_HOSTNAME "tilt-a-tron"   // reachable as tilt-a-tron.local

// ---------------------------------------------------------------- Wi-Fi
typedef enum {
    NET_OFF,          // disabled by the user
    NET_NO_NETWORK,   // enabled but no saved network
    NET_CONNECTING,   // trying / reconnecting
    NET_CONNECTED,
    NET_FAILED,       // saved network rejected us (wrong password?) or can't be found
} net_state_t;

typedef struct {
    char ssid[33];
    int8_t rssi;
    bool secure;
} net_ap_t;

// Persisted on/off switch. Turning on starts a background connect.
bool net_enabled(void);
void net_set_enabled(bool on);
// Call once at boot: connects in the background if enabled and a network is saved.
void net_start(void);
// Safe mode: bring Wi-Fi up with the saved network even if the user switched it off.
void net_start_forced(void);
net_state_t net_state(void);
// Around console sleep: radio off, then back on and reconnecting if it was enabled.
void net_suspend(void);
void net_resume(void);
const char *net_ip(void);

// Saved network name, if any (also imports Wi-Fi a PeakPal firmware left in NVS).
bool net_saved_ssid(char *out, size_t len);
// Blocking join used by the setup screen. On success the network is saved,
// Wi-Fi is switched on, and it stays connected in the background.
bool net_join(const char *ssid, const char *pass, uint32_t timeout_ms);
// Blocking join of the saved network (update mode).
bool net_join_saved(uint32_t timeout_ms);
// Blocking scan, strongest first, duplicates and hidden networks removed.
int net_scan(net_ap_t *out, int max);

// ---------------------------------------------------------------- OTA server
typedef enum { OTA_IDLE, OTA_RECEIVING, OTA_DONE, OTA_FAILED } ota_state_t;
typedef struct {
    ota_state_t state;
    uint32_t received;
    uint32_t total;
    char error[48];
} ota_status_t;

// HTTP on port 80: GET / (upload page), GET /status (JSON), GET /log (recent log
// lines), GET /reboot, POST /update (raw .bin body)
esp_err_t ota_server_start(void);
// Keep the most recent log output in memory so it can be read over Wi-Fi at /log.
// Call first thing at boot.
void net_log_capture_start(void);
// Extra fields for /status, e.g. "\"safe\":true,\"crash\":\"...\"" (no braces).
void net_status_set_extra(const char *json_fields);
// GET /screen returns a PNG of the display. The hook must return a malloc'd PNG
// (the server frees it) and its size, or 0 on failure.
typedef size_t (*net_screen_fn)(uint8_t **png_out);
void net_set_screen_hook(net_screen_fn fn);
// GET /input?<query>: remote control for testing; the hook gets the raw query string.
typedef bool (*net_control_fn)(const char *query);
void net_set_control_hook(net_control_fn fn);
// While this returns true, reboots requested over HTTP (after an update, /reboot)
// are postponed: restarting while a computer has the drive open corrupts it.
typedef bool (*net_busy_fn)(void);
void net_set_reboot_guard(net_busy_fn busy);
void ota_get_status(ota_status_t *out);

// ---------------------------------------------------------------- update mode
// Set a flag and reboot; the next boot runs the update screen instead of the launcher.
void net_request_update_mode(void);
// Read and clear the flag.
bool net_take_update_mode(void);

#ifdef __cplusplus
}
#endif
