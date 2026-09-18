#include "board/board.h"
#include "bench_game.h"
#include "console/console.h"
#include "console/icons.h"
#include "console/calibrate_app.h"
#include "console/launcher.h"
#include "console/settings_app.h"
#include "audio/audio.h"
#include "console/theme.h"
#include "console/update_app.h"
#include "engine/engine.h"
#include "esp_heap_caps.h"
#include "lodepng.h"
#include "esp_log.h"
#include <cmath>
#include <cstdio>
#include <cstring>

#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "games/breakout.h"
#include "games/maze.h"
#include "games/racer.h"
#include "games/jump.h"
#include "games/tiltatris.h"
#include "games/clock.h"
#include "games/star.h"
#include "net/net.h"
#include "link/link.h"
#include "nvs_flash.h"
#include "storage/storage.h"

static const char *TAG = "main";

static wc::Engine *s_engine;
static const console::App *s_apps;
static int s_app_count;

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
    s_engine = &engine;
    wc::EngineConfig cfg;
    cfg.spi_hz = 80 * 1000 * 1000;   // 40 MHz is the safe fallback if the picture ever glitches
    cfg.present_buffers = 2;
    if (!engine.init(cfg)) {
        ESP_LOGE(TAG, "engine init failed");
        return;
    }
    console::applyBrightness();
    console::loadTiltCalibration();

    // The screen works, so this firmware can boot: keep it rather than silently
    // rolling back. Anything that breaks after this point reports itself on screen
    // and in last_crash.txt instead of disappearing.
    esp_ota_mark_app_valid_cancel_rollback();
    ESP_LOGI(TAG, "Tilt-a-tron %s", console::firmwareVersion());

    {
        char extra[448];
        snprintf(extra, sizeof(extra), "\"safe\":%s,\"failed_boots\":%d,\"crash\":\"%s\"",
                 safe_mode ? "true" : "false", failed_boots, console::lastCrashText());
        net_status_set_extra(extra);
        // GET /screen: a PNG of the display, for checking the look over Wi-Fi.
        net_set_screen_hook([](uint8_t **png_out) -> size_t {
            wc::Engine &en = *s_engine;
            en.presenter().snapshotInto(&en.gfx());
            en.requestRedraw();
            vTaskDelay(pdMS_TO_TICKS(120));   // a frame or two, so the copy has happened
            const int n = wc::Gfx::W * wc::Gfx::H;
            // A full-size PNG needs about 2 MB of working memory. If PSRAM is too
            // fragmented for that, send the picture at half size instead of nothing.
            const int step = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) > 2200 * 1024 ? 1 : 2;
            const int ow = wc::Gfx::W / step, oh = wc::Gfx::H / step;
            uint8_t *rgb = static_cast<uint8_t *>(heap_caps_malloc(ow * oh * 3, MALLOC_CAP_SPIRAM));
            if (!rgb) return 0;
            const wc::Color *px = en.gfx().pixels();
            for (int y = 0; y < oh; y++) {
                for (int x = 0; x < ow; x++) {
                    const wc::Color p = px[y * step * wc::Gfx::W + x * step];
                    const uint16_t c = uint16_t((p >> 8) | (p << 8));
                    uint8_t *o = rgb + (y * ow + x) * 3;
                    o[0] = uint8_t((c >> 11) << 3);
                    o[1] = uint8_t(((c >> 5) & 63) << 2);
                    o[2] = uint8_t((c & 31) << 3);
                }
            }
            unsigned char *png = nullptr;
            size_t len = 0;
            LodePNGState st;
            lodepng_state_init(&st);
            st.info_raw.colortype = LCT_RGB;
            st.info_png.color.colortype = LCT_RGB;
            st.encoder.zlibsettings.btype = 2;
            st.encoder.zlibsettings.use_lz77 = 1;
            st.encoder.zlibsettings.windowsize = 2048;   // quick rather than small
            const unsigned err = lodepng_encode(&png, &len, rgb, ow, oh, &st);
            lodepng_state_cleanup(&st);
            heap_caps_free(rgb);
            if (err) {
                free(png);
                return 0;
            }
            *png_out = png;
            return len;
        });
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
    // It has to be held the whole time (1.2 s): one stray press while the watch wakes
    // used to be enough, and the bench app is a developer tool nobody should land in.
    bool bench = true;
    for (int i = 0; i < 48 && bench; i++) {
        bench = buttons_read() & BTN_BOOT;
        vTaskDelay(pdMS_TO_TICKS(25));
    }
    if (bench) {
        static BenchGame bench_game;
        engine.run(bench_game);
        return;
    }

    // The Tilt-a-tron drive (themes). With Settings > USB DRIVE on it's also a USB
    // drive on a computer; off (default), the USB port stays free for flashing and logs.
    const bool usb_drive = storage_usb_drive_enabled();
    console::crumb("storage");
    if (storage_init(usb_drive) != ESP_OK) ESP_LOGE(TAG, "storage unavailable, using the built-in look");
    ESP_LOGI(TAG, "usb drive %s, usb power %s", usb_drive ? "on (USB = drive)" : "off (USB = flashing/logs)",
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
    static games::Tiltatris tiltatris;
    static games::Clock clock_app;
    static games::Star star;
    static console::SettingsApp settings;
    static console::WifiApp wifi_setup(&settings);
    static console::UpdateApp updater(&settings);
    static console::CalibrateApp calibrate(&settings);
    settings.setScreens(&wifi_setup, &updater, &calibrate);
    static const console::App apps[] = {
        {"breakout", "BREAKOUT", wc::rgb(0xff, 0xd2, 0x3f), console::icons::breakout, &breakout},
        {"maze", "MARBLE MAZE", wc::rgb(222, 178, 112), console::icons::maze, &maze},
        {"racer", "GRAND PRIX", wc::rgb(255, 70, 70), console::icons::racer, &racer},
        {"jump", "SKY JUMP", wc::rgb(255, 190, 50), console::icons::jump, &jump},
        {"tiltatris", "TILT-A-TRIS", wc::rgb(80, 220, 240), console::icons::tiltatris, &tiltatris},
        {"star", "SLEEPY STAR", wc::rgb(255, 217, 61), console::icons::star, &star},
        {"clock", "CLOCK", wc::rgb(214, 170, 60), console::icons::clock, &clock_app},
        {"settings", "SETTINGS", wc::rgb(200, 205, 215), console::icons::settings, &settings},
    };
    static console::Launcher launcher(apps, sizeof(apps) / sizeof(apps[0]));
    static console::AppsApp games_screen(&settings, apps, sizeof(apps) / sizeof(apps[0]));
    settings.setGamesScreen(&games_screen);
    // GET /input: remote control for testing without touching the watch.
    //   app=N (0 = home)  tap=x,y  swipe=x0,y0,x1,y1  hold=x,y,ms  btn=a|b[,ms]  tilt=ax,ay,az|off
    s_apps = apps;
    s_app_count = sizeof(apps) / sizeof(apps[0]);
    net_set_control_hook([](const char *q) -> bool {
        int a, b, c, d, ms;
        float fx, fy, fz;
        const char *p;
        bool any = false;
        if ((p = std::strstr(q, "app=")) && std::sscanf(p + 4, "%d", &a) == 1) {
            if (a == 0) s_engine->goHome();
            else if (a > 0 && a <= s_app_count && s_apps[a - 1].game) s_engine->switchTo(*s_apps[a - 1].game);
            else return false;
            any = true;
        }
        if ((p = std::strstr(q, "tap=")) && std::sscanf(p + 4, "%d,%d", &a, &b) == 2) {
            s_engine->injectTouch(a, b, a, b, 80);
            any = true;
        }
        if ((p = std::strstr(q, "hold=")) && std::sscanf(p + 5, "%d,%d,%d", &a, &b, &ms) == 3) {
            s_engine->injectTouch(a, b, a, b, ms);
            any = true;
        }
        if ((p = std::strstr(q, "swipe=")) && std::sscanf(p + 6, "%d,%d,%d,%d", &a, &b, &c, &d) == 4) {
            s_engine->injectTouch(a, b, c, d, 160);
            any = true;
        }
        if ((p = std::strstr(q, "btn="))) {
            ms = 80;
            std::sscanf(p + 5, ",%d", &ms);
            s_engine->injectButton(p[4] == 'a' ? wc::BTN_A : wc::BTN_B, ms);
            any = true;
        }
        if ((p = std::strstr(q, "tilt="))) {
            if (std::strncmp(p + 5, "off", 3) == 0) s_engine->injectTilt(NAN, 0, 0);
            else if (std::sscanf(p + 5, "%f,%f,%f", &fx, &fy, &fz) == 3) s_engine->injectTilt(fx, fy, fz);
            else return false;
            any = true;
        }
        return any;
    });
    // The USB link: what the manager app on a computer talks to. Every app is built in
    // for now, so it can look but not install (docs/GAME_API.md).
    link_set_info_hook([](uint32_t *total, uint32_t *free_bytes, uint8_t *count) {
        *total = 0;   // no games region yet
        *free_bytes = 0;
        *count = uint8_t(s_app_count);
    });
    link_set_list_hook([](link_game_t *out, int max) -> int {
        int n = 0;
        for (int i = 0; i < s_app_count && n < max; i++) {
            if (std::strcmp(s_apps[i].id, "settings") == 0) continue;
            link_game_t &g = out[n++];
            memset(&g, 0, sizeof(g));
            snprintf(g.id, sizeof(g.id), "%s", s_apps[i].id);
            snprintf(g.name, sizeof(g.name), "%s", s_apps[i].name);
            g.accent = s_apps[i].accent;
            g.flags = LINK_GAME_BUILTIN | (console::appHidden(s_apps[i].id) ? LINK_GAME_HIDDEN : 0);
        }
        return n;
    });
    link_set_icon_hook([](const char *id, const uint8_t **png, size_t *len) -> bool {
        return storage_builtin_icon(id, png, len);
    });
    link_set_fs_root(STORAGE_ROOT);
    link_start();

    engine.setHome(launcher);
    engine.setSleepHooks(net_suspend, net_resume);
    // Plugged in means awake. AUTO OFF is for saving the battery, and there is no battery
    // to save on USB power - meanwhile sleeping drops the USB port, which cuts off the
    // manager app and anyone flashing it. Also stays awake for a Wi-Fi update or an open
    // app session. Double-clicking PWR still sleeps it deliberately.
    engine.setKeepAwakeHook(
        [] { return pmu_usb_power() || storage_on_computer() || net_busy() || link_session_active(); });
    engine.setBusyHook([] { return storage_on_computer() || net_transfer_active(); });
    net_set_reboot_guard(storage_on_computer);
    engine.setAutoOffSeconds(console::autoOffSeconds());
    engine.run(launcher);
}

