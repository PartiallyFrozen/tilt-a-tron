// The Tilt-a-tron emulator's core: a game, and the console's own code around it, built as
// one library for a PC.
//
// It is not a lookalike. The canvas, the font, the banners, the pause menu, the save store
// and the whole game API (components/tat_api/tat_host.cpp) are the firmware's sources,
// compiled unchanged. What is replaced is the hardware under them, and there is not much:
// the presenter that would stream pixels to the panel collects them into a picture here,
// the engine's input comes from whoever is driving this, sound is a line in the log, and
// NVS is a table written to a file. So what a game draws here is what it draws on a watch,
// to the pixel, and a game that misbehaves here misbehaves there.
//
// What this cannot tell you is how a game feels in the hand, how fast the real chip runs
// it, or anything about the loader - a game is linked in here, not loaded from a .tat.
//
// tools/emu/emu.py builds this and drives it; see tools/emu/README.md.
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "audio/audio.h"
#include "engine/engine.h"
#include "engine/gfx.h"
#include "engine/presenter.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "nvs.h"
#include "tat/tat_api.h"
#include "tat/tat_host.h"

#ifdef _WIN32
#define EMU_API extern "C" __declspec(dllexport)
#else
#define EMU_API extern "C" __attribute__((visibility("default")))
#endif

// Built around a game - or, with EMU_SCREEN_*, around one of the console's own screens, which
// are not games and draw through the engine rather than the game API.
#if defined(EMU_SCREEN_BOOT) || defined(EMU_SCREEN_POWER) || defined(EMU_SCREEN_HOLD)
#define EMU_SCREEN 1
#include "console/boot_anim.h"
#include "console/power_menu.h"
#else
extern "C" const tat_game_t tat_game;   // the game this library was built around
#endif

// A game names its files through `tat_assets`, which on a watch the loader fills in from the
// package. Here it is filled in from the game's assets folder. The game sees it as const;
// it is defined in emu_assets.c, which does not, so that it can be written.
extern "C" void emu_set_asset(int slot, const char *name, const unsigned char *data, const unsigned char *end);

// ------------------------------------------------------------------ time, chance, the log

static int64_t s_now_us = 1000000;   // not zero: games seed things from it
static uint32_t s_rng = 0x2F6E2B1u;
static bool s_quiet = false;

int64_t esp_timer_get_time(void) { return s_now_us; }

uint32_t esp_random(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}

void emu_log_line(char level, const char *tag, const char *fmt, ...)
{
    if (s_quiet) return;
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    std::printf("%c (%lld) %s: %s\n", level, (long long)(s_now_us / 1000), tag, line);
    std::fflush(stdout);
}

// ------------------------------------------------------------------ sound

namespace wc::audio {
static int s_volume = 2;
static int s_tones = 0;
bool init() { return true; }
void play(const Tone &t)
{
    s_tones++;
    if (s_volume == 0) return;
    static const char *const W[] = {"square", "triangle", "noise"};
    emu_log_line('I', "audio", "%s %.0f->%.0f Hz, %u ms, vol %.2f, after %u ms", W[int(t.wave) % 3], t.f0, t.f1, t.ms,
                 t.volume, t.delay_ms);
}
void play(const Tone *tones, int count)
{
    for (int i = 0; i < count; i++) play(tones[i]);
}
void stopAll() {}
int volume() { return s_volume; }
void setVolume(int level) { s_volume = level; }
}  // namespace wc::audio

// ------------------------------------------------------------------ NVS, in a file

struct NvsEntry {
    std::string ns, key;
    int kind;   // 0 i32, 1 u8, 2 u16, 3 blob
    int32_t value;
    std::vector<unsigned char> blob;
};
static std::vector<NvsEntry> s_nvs;
static std::vector<std::string> s_open;   // handle - 1 indexes the namespace it was opened on
static std::string s_nvs_path;

static NvsEntry *nvs_find(nvs_handle_t h, const char *key)
{
    if (h == 0 || h > s_open.size()) return nullptr;
    for (auto &e : s_nvs)
        if (e.ns == s_open[h - 1] && e.key == key) return &e;
    return nullptr;
}

static void nvs_write_file()
{
    if (s_nvs_path.empty()) return;
    FILE *f = std::fopen(s_nvs_path.c_str(), "w");
    if (!f) return;
    for (const auto &e : s_nvs) {
        if (e.kind == 3) {
            std::fprintf(f, "%s %s blob", e.ns.c_str(), e.key.c_str());
            for (unsigned char b : e.blob) std::fprintf(f, " %02x", b);
            std::fprintf(f, "\n");
        } else {
            std::fprintf(f, "%s %s %s %d\n", e.ns.c_str(), e.key.c_str(), e.kind == 0 ? "i32" : e.kind == 1 ? "u8" : "u16",
                         (int)e.value);
        }
    }
    std::fclose(f);
}

static void nvs_read_file()
{
    s_nvs.clear();
    FILE *f = s_nvs_path.empty() ? nullptr : std::fopen(s_nvs_path.c_str(), "r");
    if (!f) return;
    char line[4096];
    while (std::fgets(line, sizeof(line), f)) {
        char ns[64], key[64], kind[16];
        int used = 0;
        if (std::sscanf(line, "%63s %63s %15s%n", ns, key, kind, &used) != 3) continue;
        NvsEntry e{ns, key, 0, 0, {}};
        if (!std::strcmp(kind, "blob")) {
            e.kind = 3;
            const char *p = line + used;
            unsigned b;
            int n;
            while (std::sscanf(p, " %2x%n", &b, &n) == 1) {
                e.blob.push_back((unsigned char)b);
                p += n;
            }
        } else {
            e.kind = !std::strcmp(kind, "u8") ? 1 : !std::strcmp(kind, "u16") ? 2 : 0;
            e.value = std::atoi(line + used);
        }
        s_nvs.push_back(e);
    }
    std::fclose(f);
}

esp_err_t nvs_open(const char *ns, nvs_open_mode_t, nvs_handle_t *out)
{
    s_open.push_back(ns ? ns : "");
    *out = (nvs_handle_t)s_open.size();
    return ESP_OK;
}
void nvs_close(nvs_handle_t) {}
esp_err_t nvs_commit(nvs_handle_t)
{
    nvs_write_file();
    return ESP_OK;
}
esp_err_t nvs_erase_key(nvs_handle_t h, const char *key)
{
    for (size_t i = 0; i < s_nvs.size(); i++)
        if (h && h <= s_open.size() && s_nvs[i].ns == s_open[h - 1] && s_nvs[i].key == key) {
            s_nvs.erase(s_nvs.begin() + (long)i);
            return ESP_OK;
        }
    return ESP_ERR_NOT_FOUND;
}
static esp_err_t nvs_get(nvs_handle_t h, const char *key, int kind, int32_t *v)
{
    NvsEntry *e = nvs_find(h, key);
    if (!e) return ESP_ERR_NOT_FOUND;
    if (e->kind != kind) return ESP_ERR_NVS_TYPE_MISMATCH;
    *v = e->value;
    return ESP_OK;
}
esp_err_t nvs_get_i32(nvs_handle_t h, const char *key, int32_t *v) { return nvs_get(h, key, 0, v); }
esp_err_t nvs_get_u8(nvs_handle_t h, const char *key, uint8_t *v)
{
    int32_t x;
    const esp_err_t r = nvs_get(h, key, 1, &x);
    if (r == ESP_OK) *v = (uint8_t)x;
    return r;
}
esp_err_t nvs_get_u16(nvs_handle_t h, const char *key, uint16_t *v)
{
    int32_t x;
    const esp_err_t r = nvs_get(h, key, 2, &x);
    if (r == ESP_OK) *v = (uint16_t)x;
    return r;
}
esp_err_t nvs_set_i32(nvs_handle_t h, const char *key, int32_t v)
{
    NvsEntry *e = nvs_find(h, key);
    if (e && e->kind != 0) return ESP_ERR_NVS_TYPE_MISMATCH;   // the real one refuses a change of width
    if (!e) {
        if (h == 0 || h > s_open.size()) return ESP_FAIL;
        s_nvs.push_back({s_open[h - 1], key, 0, v, {}});
    } else {
        e->value = v;
    }
    return ESP_OK;
}
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *len)
{
    NvsEntry *e = nvs_find(h, key);
    if (!e || e->kind != 3) return ESP_ERR_NOT_FOUND;
    if (out) {
        if (*len < e->blob.size()) return ESP_FAIL;
        std::memcpy(out, e->blob.data(), e->blob.size());
    }
    *len = e->blob.size();
    return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *data, size_t len)
{
    NvsEntry *e = nvs_find(h, key);
    if (!e) {
        if (h == 0 || h > s_open.size()) return ESP_FAIL;
        s_nvs.push_back({s_open[h - 1], key, 3, 0, {}});
        e = &s_nvs.back();
    }
    e->kind = 3;
    e->blob.assign((const unsigned char *)data, (const unsigned char *)data + len);
    return ESP_OK;
}

// ------------------------------------------------------------------ the panel

// What would have gone down the wire. A canvas game streams its picture in bands, just as
// it does to the real panel; a menu draws into the framebuffer and that is copied instead.
static wc::Color s_frame[wc::Gfx::W * wc::Gfx::H];
static bool s_streamed = false;

namespace wc {

bool Presenter::init(int, int) { return true; }

void Presenter::presentBands(const BandFill &fill)
{
    static Color band[Gfx::W * 34];
    const int rows_max = 34;
    for (int y = 0; y < Gfx::H; y += rows_max) {
        const int rows = std::min(rows_max, Gfx::H - y);
        int x0 = Gfx::W, x1 = 0;
        for (int b = y / Gfx::BAND_H; b <= (y + rows - 1) / Gfx::BAND_H; b++) {
            int vx0, vx1;
            Gfx::visibleSpan(b, vx0, vx1);
            x0 = std::min(x0, vx0);
            x1 = std::max(x1, vx1);
        }
        if (x1 <= x0) continue;
        fill(y, rows, x0, x1 - x0, band);
        for (int r = 0; r < rows; r++)
            std::memcpy(s_frame + (y + r) * Gfx::W + x0, band + r * (x1 - x0), size_t(x1 - x0) * sizeof(Color));
    }
    s_streamed = true;
    streamed_ = true;
}

// A game that has nothing new to show simply does not present, and the panel goes on showing
// what it had - so the framebuffer is only copied when it is the thing being looked at,
// which for a game is when its pause menu is open.
void Presenter::present(Gfx &gfx)
{
#ifdef EMU_SCREEN
    const bool looked_at = true;   // a console screen draws nowhere else
#else
    const bool looked_at = tat::api()->menu_is_open();
#endif
    if (!streamed_ && looked_at) std::memcpy(s_frame, gfx.pixels(), sizeof(s_frame));
    streamed_ = false;
}

void Presenter::flush() {}

// The engine's own input is a task reading I2C. Here it is whatever the front end says.
bool Input::init() { return true; }
void Input::snapshot(InputState &) {}
void Input::pause(bool) {}
void Input::setCalibration(const TiltCal &) {}
TiltCal Input::calibration() { return {}; }

}  // namespace wc

// The one door into the engine that engine.h leaves open for this.
namespace wc {
struct EngineEmulator {
    static InputState &input(Engine &e) { return e.input_state_; }
    static bool wants_home(Engine &e) { return e.pending_ != nullptr; }
    static void set_home(Engine &e, Game &home)
    {
        e.home_ = &home;
        e.pending_ = nullptr;
    }
};
}  // namespace wc
using wc::EngineEmulator;

// ------------------------------------------------------------------ what the front end calls

struct HomeMarker : wc::Game {
    void update(wc::Engine &, float) override {}
    void draw(wc::Engine &, wc::Gfx &) override {}
};

#ifdef EMU_SCREEN
// The boot animation, as BootSplash plays it: two spins, a stretch of "setting up" as a new
// watch would show, and round again.
struct BootScreen : wc::Game {
    console::BootAnim anim;
    float t = 0;
    void begin(wc::Engine &) override { anim.init(); }
    void update(wc::Engine &, float dt) override { t += dt; }
    void draw(wc::Engine &e, wc::Gfx &) override
    {
        const float lap = console::BootAnim::SPIN_S * 2 + 2.0f, at = std::fmod(t, lap);
        if (at < console::BootAnim::SPIN_S * 2) anim.spin(e.presenter(), at);
        else anim.waiting(e.presenter(), "SETTING UP", "SKATER GIRLZ", int((at - console::BootAnim::SPIN_S * 2) * 5), 10);
    }
};
#endif

static wc::Engine s_engine;
static HomeMarker s_home;
#ifdef EMU_SCREEN
static wc::Game *s_game;
#else
static tat::HostedGame *s_game;
#endif
static std::vector<std::vector<unsigned char>> s_asset_bytes;
static std::vector<std::string> s_asset_names;

static struct {
    bool down;
    int x, y;
    uint32_t held;
    float ax, ay, az, gx, gy, gz;
} s_in = {false, 0, 0, 0, 0, 1, 0, 0, 0, 0}, s_prev = s_in;   // held upright, the way it is on a wrist
static int64_t s_btn_down_us[8];

#ifdef EMU_SCREEN
EMU_API const char *emu_game_id(void) { return "screen"; }
EMU_API const char *emu_game_name(void) { return "TILT-A-TRON"; }
#else
EMU_API const char *emu_game_id(void) { return tat_game.id; }
EMU_API const char *emu_game_name(void) { return tat_game.name; }
#endif

// Before emu_begin: each file in the game's assets folder, by the name the game asks for.
EMU_API void emu_add_asset(const char *name, const unsigned char *data, int len)
{
    s_asset_names.emplace_back(name);
    s_asset_bytes.emplace_back(data, data + len);
}

EMU_API void emu_options(const char *save_path, unsigned seed, int quiet)
{
    s_nvs_path = save_path ? save_path : "";
    if (seed) s_rng = seed;
    s_quiet = quiet != 0;
}

EMU_API int emu_begin(void)
{
    nvs_read_file();
#ifdef EMU_SCREEN
    if (!s_engine.gfx().init()) return 0;
    EngineEmulator::set_home(s_engine, s_home);
#ifdef EMU_SCREEN_BOOT
    s_game = new BootScreen;
#else
    s_game = new console::PowerMenu;
#endif
#ifdef EMU_SCREEN_HOLD
    EngineEmulator::input(s_engine).held = wc::BTN_B;   // as the engine shows it: PWR still down
#endif
    s_game->begin(s_engine);
    s_game->enter(s_engine);
    return 1;
#else
    for (size_t i = 0; i < s_asset_names.size() && i < 31; i++) {
        emu_set_asset((int)i, s_asset_names[i].c_str(), s_asset_bytes[i].data(),
                      s_asset_bytes[i].data() + s_asset_bytes[i].size());
    }
    if (tat_game.asset_count > (int)s_asset_names.size())
        emu_log_line('W', "emu", "%s declares %d assets and its folder has %d", tat_game.id, tat_game.asset_count,
                     (int)s_asset_names.size());
    if (!s_engine.gfx().init()) return 0;
    EngineEmulator::set_home(s_engine, s_home);
    s_game = new tat::HostedGame(tat_game);
    s_game->begin(s_engine);
    s_game->enter(s_engine);
    return 1;
#endif
}

// Held buttons are a mask of TAT_BTN_*; touch is in screen pixels; tilt is gravity in g,
// screen-aligned as the games see it (+x right, +y down, +z out of the glass toward you).
EMU_API void emu_input(int touch_down, int x, int y, unsigned held, float ax, float ay, float az)
{
    s_in.down = touch_down != 0;
    if (touch_down) s_in.x = x, s_in.y = y;
    s_in.held = held;
    s_in.ax = ax, s_in.ay = ay, s_in.az = az;
}

// One frame: what the engine's loop does, without the engine's loop. Returns 1 if the game
// asked to go home.
EMU_API int emu_step(float dt)
{
    s_now_us += (int64_t)(dt * 1e6f);
    wc::InputState &in = EngineEmulator::input(s_engine);
    in = wc::InputState{};
    in.touch.down = s_in.down;
    in.touch.pressed = s_in.down && !s_prev.down;
    in.touch.released = !s_in.down && s_prev.down;
    in.touch.x = s_in.x;
    in.touch.y = s_in.y;
    in.touch.t_us = s_now_us;
    in.held = s_in.held;
    in.pressed = s_in.held & ~s_prev.held;
    in.released = ~s_in.held & s_prev.held;
    for (int b = 0; b < 8; b++) {
        const uint32_t bit = 1u << b;
        if (in.pressed & bit) s_btn_down_us[b] = s_now_us;
        if ((in.released & bit) && s_now_us - s_btn_down_us[b] < 600000) in.clicked |= bit;
        if ((in.held & bit) && s_now_us - s_btn_down_us[b] >= 600000 && s_now_us - s_btn_down_us[b] - (int64_t)(dt * 1e6f) < 600000)
            in.long_press |= bit;
    }
    in.tilt.ax = s_in.ax;
    in.tilt.ay = s_in.ay;
    in.tilt.az = s_in.az;
    // The gyro, from how the gravity vector turned since the last frame: enough for the games
    // that use it to steady themselves, and it is zero when nothing moves.
    in.tilt.gx = in.tilt.gy = in.tilt.gz = 0;
    s_prev = s_in;

    s_streamed = false;
    s_game->update(s_engine, dt);
    s_game->draw(s_engine, s_engine.gfx());
    s_engine.presenter().present(s_engine.gfx());
    return EngineEmulator::wants_home(s_engine) ? 1 : 0;
}

// The screen as 466 x 466 RGB, three bytes a pixel, with the corners the round panel does
// not have painted `corner` so a picture of it looks like the watch.
EMU_API void emu_frame(unsigned char *rgb, int corner_r, int corner_g, int corner_b)
{
    const int W = wc::Gfx::W, H = wc::Gfx::H;
    const float c = (W - 1) * 0.5f, r2 = (W * 0.5f) * (W * 0.5f);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            unsigned char *o = rgb + (y * W + x) * 3;
            const float dx = x - c, dy = y - c;
            if (dx * dx + dy * dy > r2) {
                o[0] = (unsigned char)corner_r, o[1] = (unsigned char)corner_g, o[2] = (unsigned char)corner_b;
                continue;
            }
            wc::Color v = s_frame[y * W + x];
            v = wc::Color((v >> 8) | (v << 8));   // the framebuffer is kept in the panel's byte order
            o[0] = (unsigned char)(((v >> 11) & 31) * 255 / 31);
            o[1] = (unsigned char)(((v >> 5) & 63) * 255 / 63);
            o[2] = (unsigned char)((v & 31) * 255 / 31);
        }
}

EMU_API int emu_tones(void) { return wc::audio::s_tones; }
EMU_API int emu_keep_awake(void) { return s_game && s_game->keepAwake() ? 1 : 0; }

EMU_API void emu_end(void)
{
    delete s_game;   // tells the game it is going, and reports whatever it left behind
    s_game = nullptr;
    nvs_write_file();
}
