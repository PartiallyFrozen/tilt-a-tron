#include "board/board.h"
#include "bench_game.h"
#include "console/console.h"
#include "console/icons.h"
#include "console/launcher.h"
#include "console/settings_app.h"
#include "audio/audio.h"
#include "console/theme.h"
#include "console/update_app.h"
#include "engine/engine.h"
#include "esp_log.h"
#include <cstring>

#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "games/breakout.h"
#include "games/maze.h"
#include "games/racer.h"
#include "games/jump.h"
#include "net/net.h"
#include "nvs_flash.h"
#include "storage/storage.h"

static const char *TAG = "main";

extern "C" void app_main(void)
{
    esp_err_t nvs = nvs_flash_init();
    if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    // Everything logged from here on can be read over Wi-Fi at /log.
    net_log_capture_start();

    // Three crashed boots in a row = safe mode: skip everything optional and just
    // bring up Wi-Fi so a fixed build can be sent over the air.
    const int failed_boots = console::noteBootStarted();
    const bool safe_mode = failed_boots >= 3;
    console::setSafeMode(safe_mode);
    // A crash during sound start-up last time? Leave sound off this time.
    const bool skip_audio = safe_mode || (console::crashedLastBoot() && strcmp(console::previousCrumb(), "audio") == 0);

    // Settings -> WI-FI UPDATE reboots into update mode. Wi-Fi needs internal RAM,
    // so the display pipeline takes fewer buffers in that mode.
    const bool update_mode = net_take_update_mode();

    static wc::Engine engine;
    wc::EngineConfig cfg;
    cfg.spi_hz = 80 * 1000 * 1000;   // 40 MHz is the safe fallback if the picture ever glitches
    cfg.present_buffers = 2;
    if (!engine.init(cfg)) {
        ESP_LOGE(TAG, "engine init failed");
        return;
    }
    console::applyBrightness();

    // The screen works, so this firmware can boot: keep it rather than silently
    // rolling back. Anything that breaks after this point reports itself on screen
    // and in last_crash.txt instead of disappearing.
    esp_ota_mark_app_valid_cancel_rollback();
    ESP_LOGI(TAG, "Tilt-a-tron %s", console::firmwareVersion());

    {
        char extra[192];
        snprintf(extra, sizeof(extra), "\"safe\":%s,\"failed_boots\":%d,\"crash\":\"%s\"",
                 safe_mode ? "true" : "false", failed_boots, console::lastCrashText());
        net_status_set_extra(extra);
    }
    if (console::crashedLastBoot()) ESP_LOGE(TAG, "previous boot: %s", console::lastCrashText());

    // Running 20 s without falling over counts as a good boot.
    static esp_timer_handle_t stable_timer;
    const esp_timer_create_args_t stable_args = {.callback = [](void *) { console::noteBootStable(); },
                                                 .arg = nullptr,
                                                 .dispatch_method = ESP_TIMER_TASK,
                                                 .name = "boot_stable",
                                                 .skip_unhandled_events = true};
    if (esp_timer_create(&stable_args, &stable_timer) == ESP_OK) esp_timer_start_once(stable_timer, 20000000);

    if (safe_mode) {
        ESP_LOGE(TAG, "SAFE MODE after %d failed boots (%s)", failed_boots, console::lastCrashText());
        net_start_forced();
        static console::UpdateApp rescue;
        engine.run(rescue);
        return;
    }

    if (skip_audio) {
        ESP_LOGW(TAG, "sound start-up crashed last time; leaving sound off for this boot");
    } else {
        console::crumb("audio");
        wc::audio::init();
        console::crumb("");
    }

    if (update_mode) {
        static console::UpdateApp updater;
        engine.run(updater);
        return;
    }

    // Hold BOOT as the screen comes on (not during reset: that enters download
    // mode) to get the bench/calibration app.
    bool bench = false;
    for (int i = 0; i < 20 && !bench; i++) {
        bench = buttons_read() & BTN_BOOT;
        vTaskDelay(pdMS_TO_TICKS(25));
    }
    if (bench) {
        static BenchGame bench_game;
        engine.run(bench_game);
        return;
    }

    // The Tilt-a-tron drive (themes). With DEBUG MODE off it's also a USB drive on a
    // computer; with it on, the USB port stays free for flashing and logs.
    const bool debug = storage_debug_mode();
    console::crumb("storage");
    if (storage_init(!debug) != ESP_OK) ESP_LOGE(TAG, "storage unavailable, using the built-in look");
    ESP_LOGI(TAG, "debug mode %s, usb power %s", debug ? "on (USB = flashing/logs)" : "off (USB = drive)",
             pmu_usb_power() ? "yes" : "no");

    // If the last run ended badly, say so on screen and leave a note on the drive.
    console::reportCrashIfAny(engine);

    console::crumb("theme");
    console::Theme::get().loadActive();
    console::crumb("");

    // Wi-Fi only runs if the user switched it on in Settings.
    net_start();

    // The app carousel.
    static games::Breakout breakout;
    static games::Maze maze;
    static games::Racer racer;
    static games::Jump jump;
    static console::SettingsApp settings;
    static console::WifiApp wifi_setup(&settings);
    static console::UpdateApp updater(&settings);
    settings.setScreens(&wifi_setup, &updater);
    static const console::App apps[] = {
        {"breakout", "BREAKOUT", wc::rgb(0xff, 0xd2, 0x3f), console::icons::breakout, &breakout},
        {"maze", "MARBLE MAZE", wc::rgb(222, 178, 112), console::icons::maze, &maze},
        {"racer", "GRAND PRIX", wc::rgb(255, 70, 70), console::icons::racer, &racer},
        {"jump", "SKY JUMP", wc::rgb(255, 190, 50), console::icons::jump, &jump},
        {"settings", "SETTINGS", wc::rgb(200, 205, 215), console::icons::settings, &settings},
    };
    static console::Launcher launcher(apps, sizeof(apps) / sizeof(apps[0]));
    engine.setHome(launcher);
    engine.setSleepHooks(net_suspend, net_resume);
    // Don't sleep while plugged in: a computer may be copying theme files, and on the
    // bench (debug mode) sleeping drops the serial/flashing connection.
    engine.setKeepAwakeHook([] { return storage_on_computer() || pmu_usb_power(); });
    engine.setBusyHook(storage_on_computer);
    engine.setAutoOffSeconds(console::autoOffSeconds());
    engine.run(launcher);
}
