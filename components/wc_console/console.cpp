#include "console/console.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "board/board.h"
#include "console/ui.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_core_dump.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "storage/storage.h"
#include "esp_app_desc.h"
#include "nvs.h"

namespace console {

static constexpr uint8_t kLevels[BRIGHTNESS_LEVELS] = {50, 110, 180, 255};
static int s_level = -1;

int brightnessLevel()
{
    if (s_level < 0) {
        s_level = 2;
        nvs_handle_t h;
        if (nvs_open("console", NVS_READONLY, &h) == ESP_OK) {
            uint8_t v;
            if (nvs_get_u8(h, "bright", &v) == ESP_OK && v < BRIGHTNESS_LEVELS) s_level = v;
            nvs_close(h);
        }
    }
    return s_level;
}

void applyBrightness() { display_set_brightness(kLevels[brightnessLevel()]); }

void setBrightnessLevel(int level)
{
    s_level = level % BRIGHTNESS_LEVELS;
    applyBrightness();
    nvs_handle_t h;
    if (nvs_open("console", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "bright", uint8_t(s_level));
        nvs_commit(h);
        nvs_close(h);
    }
}

static constexpr int kAutoOffSeconds[AUTO_OFF_OPTIONS] = {60, 120, 300, 600, 0};
static int s_auto_off = -1;

int autoOffIndex()
{
    if (s_auto_off < 0) {
        s_auto_off = 1;   // 2 minutes
        nvs_handle_t h;
        if (nvs_open("console", NVS_READONLY, &h) == ESP_OK) {
            uint8_t v;
            if (nvs_get_u8(h, "auto_off", &v) == ESP_OK && v < AUTO_OFF_OPTIONS) s_auto_off = v;
            nvs_close(h);
        }
    }
    return s_auto_off;
}

int autoOffSeconds() { return kAutoOffSeconds[autoOffIndex()]; }

void setAutoOffIndex(int index)
{
    s_auto_off = index % AUTO_OFF_OPTIONS;
    nvs_handle_t h;
    if (nvs_open("console", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "auto_off", uint8_t(s_auto_off));
        nvs_commit(h);
        nvs_close(h);
    }
}

bool appHidden(const char *id)
{
    if (std::strcmp(id, "settings") == 0) return false;
    uint8_t v = 0;
    nvs_handle_t h;
    if (nvs_open("apps", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, id, &v);
        nvs_close(h);
    }
    return v != 0;
}

void setAppHidden(const char *id, bool hidden)
{
    nvs_handle_t h;
    if (nvs_open("apps", NVS_READWRITE, &h) != ESP_OK) return;
    if (hidden) nvs_set_u8(h, id, 1);
    else nvs_erase_key(h, id);
    nvs_commit(h);
    nvs_close(h);
}

static bool s_tilt_calibrated = false;

void loadTiltCalibration()
{
    nvs_handle_t h;
    if (nvs_open("console", NVS_READONLY, &h) != ESP_OK) return;
    wc::TiltCal cal;
    size_t len = sizeof(cal);
    if (nvs_get_blob(h, "tiltcal", &cal, &len) == ESP_OK && len == sizeof(cal)) {
        wc::Input::setCalibration(cal);
        s_tilt_calibrated = true;
    }
    nvs_close(h);
}

void saveTiltCalibration(const wc::TiltCal &cal)
{
    wc::Input::setCalibration(cal);
    s_tilt_calibrated = true;
    nvs_handle_t h;
    if (nvs_open("console", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, "tiltcal", &cal, sizeof(cal));
        nvs_commit(h);
        nvs_close(h);
    }
}

void clearTiltCalibration()
{
    wc::Input::setCalibration(wc::TiltCal{});
    s_tilt_calibrated = false;
    nvs_handle_t h;
    if (nvs_open("console", NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, "tiltcal");
        nvs_commit(h);
        nvs_close(h);
    }
}

bool tiltCalibrated() { return s_tilt_calibrated; }

const char *firmwareVersion() { return esp_app_get_description()->version; }

// ------------------------------------------------------------------ crash breadcrumbs

// RTC memory keeps its contents across a crash and restart (but not power loss).
#define CRUMB_MAGIC 0x54494C54
static RTC_NOINIT_ATTR uint32_t s_crumb_magic;
static RTC_NOINIT_ATTR char s_crumb[48];
static char s_previous[48];
static char s_crash_text[320];
static bool s_crashed;

void crumb(const char *what)
{
    s_crumb_magic = CRUMB_MAGIC;
    std::snprintf(s_crumb, sizeof(s_crumb), "%s", what ? what : "");
}

static const char *resetReasonName(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_PANIC: return "CRASH";
    case ESP_RST_TASK_WDT: return "TASK STUCK";
    case ESP_RST_INT_WDT: return "SYSTEM STUCK";
    case ESP_RST_WDT: return "WATCHDOG";
    case ESP_RST_BROWNOUT: return "POWER DIP";
    default: return "RESTART";
    }
}

static RTC_NOINIT_ATTR uint32_t s_boot_magic;
static RTC_NOINIT_ATTR uint32_t s_boot_fails;

static bool s_safe_mode;
bool safeMode() { return s_safe_mode; }
void setSafeMode(bool on) { s_safe_mode = on; }

int noteBootStarted()
{
    // A power cycle (or deliberate restart) starts the count again; only crashes
    // and watchdog resets keep it running.
    const esp_reset_reason_t r = esp_reset_reason();
    const bool bad = r == ESP_RST_PANIC || r == ESP_RST_TASK_WDT || r == ESP_RST_INT_WDT || r == ESP_RST_WDT ||
                     r == ESP_RST_BROWNOUT;

    // Read what the previous boot was doing *before* this boot leaves its own crumbs.
    if (s_crumb_magic == CRUMB_MAGIC) std::snprintf(s_previous, sizeof(s_previous), "%s", s_crumb);
    else s_previous[0] = 0;
    crumb("boot");
    if (bad) {
        s_crashed = true;
        int n = std::snprintf(s_crash_text, sizeof(s_crash_text), "%s - %s", resetReasonName(r),
                              s_previous[0] ? s_previous : "unknown");
        // The core dump (if the coredump partition exists) says where it died:
        // task, PC and a backtrace. update.ps1 -Crash turns the addresses into lines.
        auto *sum = static_cast<esp_core_dump_summary_t *>(std::calloc(1, sizeof(esp_core_dump_summary_t)));
        if (sum && esp_core_dump_image_check() == ESP_OK && esp_core_dump_get_summary(sum) == ESP_OK) {
            n += std::snprintf(s_crash_text + n, sizeof(s_crash_text) - n, " | %s cause %u pc 0x%08x bt",
                               sum->exc_task, unsigned(sum->ex_info.exc_cause), unsigned(sum->exc_pc));
            for (unsigned i = 0; i < sum->exc_bt_info.depth && i < 10 && n < int(sizeof(s_crash_text)) - 12; i++)
                n += std::snprintf(s_crash_text + n, sizeof(s_crash_text) - n, " %08x", unsigned(sum->exc_bt_info.bt[i]));
            esp_core_dump_image_erase();
        }
        std::free(sum);
    } else {
        s_previous[0] = 0;
    }

    if (s_boot_magic != CRUMB_MAGIC || !bad) {
        s_boot_magic = CRUMB_MAGIC;
        s_boot_fails = 0;
    }
    const int before = int(s_boot_fails);
    s_boot_fails++;
    return before;
}

void noteBootStable() { s_boot_fails = 0; }

const char *previousCrumb() { return s_previous; }

bool crashedLastBoot() { return s_crashed; }
const char *lastCrashText() { return s_crash_text; }

void reportCrashIfAny(wc::Engine &e)
{
    if (!s_crashed) return;   // noteBootStarted() worked this out at the top of boot
    const esp_reset_reason_t reason = esp_reset_reason();
    ESP_LOGE("crash", "last boot ended with %s", s_crash_text);

    // Leave a note on the drive: with DEBUG MODE off this is the only way to see it.
    if (storage_ready()) {
        if (FILE *f = std::fopen(STORAGE_ROOT "/last_crash.txt", "a")) {
            std::fprintf(f, "%s (firmware %s)\n", s_crash_text, firmwareVersion());
            std::fclose(f);
        }
    }

    // Red screen for a few seconds so it's obvious something went wrong.
    wc::Gfx &g = e.gfx();
    g.clear(wc::rgb(60, 0, 0));
    g.textCentered(wc::Gfx::CX, 150, "SOMETHING BROKE", wc::colors::white, 3, true);
    g.textCentered(wc::Gfx::CX, 210, resetReasonName(reason), ui::ACCENT, 3, true);
    g.textCentered(wc::Gfx::CX, 260, s_previous[0] ? s_previous : "unknown", wc::colors::white, 2, true);
    g.textCentered(wc::Gfx::CX, 310, "SAVED TO last_crash.txt", ui::DIM, 2, true);
    e.presenter().present(g);
    e.presenter().flush();
    vTaskDelay(pdMS_TO_TICKS(4000));
}

}  // namespace console
