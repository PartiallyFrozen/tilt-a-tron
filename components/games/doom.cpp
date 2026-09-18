#include "games/doom.h"

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <strings.h>

#include "audio/audio.h"
#include "console/ui.h"
#include "doom/doom_port.h"
#include "engine/gestures.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "storage/storage.h"

namespace games {

using namespace wc;
namespace ui = console::ui;

namespace {

constexpr const char *TAG = "doom_app";
constexpr float PI = 3.14159265f;

// Doom's 320x200 picture was made for a 4:3 screen (pixels taller than wide). Shown
// that way and filling the round screen top to bottom, the middle ~240 columns fit.
constexpr float SCALE_Y = Gfx::H / float(DOOM_H);   // 2.33
constexpr float SCALE_X = SCALE_Y / 1.2f;           // 1.94

constexpr float FULL_LOCK = 50.0f * PI / 180.0f;   // wheel angle for the fastest turn
constexpr int MAX_TURN = 1150;                     // Doom angle units per tic (~220 deg/s)
constexpr float WALK_DEAD = 5.0f * PI / 180.0f;
constexpr float WALK_RANGE[3] = {28.0f * PI / 180.0f, 19.0f * PI / 180.0f, 12.0f * PI / 180.0f};
constexpr const char *WALK_NAMES[3] = {"LOW", "MED", "HIGH"};
constexpr const char *SKILL_NAMES[5] = {"BABY", "EASY", "NORMAL", "HARD", "NIGHTMARE"};

float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

bool isWad(const char *name)
{
    const size_t n = std::strlen(name);
    return n > 4 && strcasecmp(name + n - 4, ".wad") == 0;
}

// A WAD the user dropped on the drive: Doom/<anything>.wad, or one in the top folder.
bool findWadOnDrive(char *path, size_t len, uint32_t *size)
{
    static const char *const kDirs[] = {STORAGE_ROOT "/Doom", STORAGE_ROOT "/doom", STORAGE_ROOT};
    for (const char *dir : kDirs) {
        DIR *d = opendir(dir);
        if (!d) continue;
        bool found = false;
        while (const dirent *ent = readdir(d)) {
            if (!isWad(ent->d_name)) continue;
            snprintf(path, len, "%s/%s", dir, ent->d_name);
            struct stat st;
            if (stat(path, &st) == 0 && st.st_size > 12) {
                *size = uint32_t(st.st_size);
                found = true;
                break;
            }
        }
        closedir(d);
        if (found) return true;
    }
    return false;
}

Engine *s_progress_engine;

void installProgress(int percent)
{
    static int shown = -1;
    if (percent == shown || !s_progress_engine) return;
    shown = percent;
    Gfx &g = s_progress_engine->gfx();
    const int w = 300, x = Gfx::CX - w / 2, y = 250;
    g.fillRect(x, y, w, 26, ui::PANEL);
    g.rect(x, y, w, 26, ui::BOX);
    g.fillRect(x + 3, y + 3, (w - 6) * percent / 100, 20, ui::DANGER);
    s_progress_engine->presenter().present(g);
}

}  // namespace

struct Doom::State {
    Gestures ges;
    enum Screen { NO_WAD, LOADING, PLAYING, FAILED } screen = LOADING;
    bool menu = false, menu_dirty = false, static_dirty = true;

    // settings
    int skill = 2, walk_sens = 1;

    // tilt
    float grav_x = 0, grav_y = 1, grav_z = 0;
    float roll = 0, pitch = 0, pitch_neutral = 0;
    bool neutral_pending = true;
    float settle = 0;

    // touch -> fire
    int64_t touch_t0 = 0, fire_until = 0, use_until = 0;
    int touch_x0 = 0, touch_y0 = 0;
    bool swiping = false;

    doom_status_t status{};
    Color pal[256] = {};
    uint32_t pal_gen = ~0u;
    uint32_t shown_frames = 0;
    bool force = true;

    void load()
    {
        nvs_handle_t h;
        if (nvs_open("doom", NVS_READONLY, &h) != ESP_OK) return;
        uint8_t v;
        if (nvs_get_u8(h, "skill", &v) == ESP_OK && v < 5) skill = v;
        if (nvs_get_u8(h, "walk", &v) == ESP_OK && v < 3) walk_sens = v;
        nvs_close(h);
    }

    void save()
    {
        nvs_handle_t h;
        if (nvs_open("doom", NVS_READWRITE, &h) != ESP_OK) return;
        nvs_set_u8(h, "skill", uint8_t(skill));
        nvs_set_u8(h, "walk", uint8_t(walk_sens));
        nvs_commit(h);
        nvs_close(h);
    }

    // Make sure there's a WAD in flash (copying one from the drive if needed), then
    // start the engine.
    void prepare(Engine &e)
    {
        if (doom_port_started()) {
            screen = PLAYING;
            return;
        }
        doom_wad_info_t have;
        doom_wad_info(&have);
        char path[300];
        uint32_t size = 0;
        const bool drive_ok = storage_ready() && !storage_on_computer();
        if (drive_ok && findWadOnDrive(path, sizeof(path), &size)) {
            const char *base = std::strrchr(path, '/') + 1;
            if (!have.present || have.length != size || strcasecmp(have.name, base) != 0) {
                ESP_LOGI(TAG, "installing %s (%u bytes)", path, unsigned(size));
                Gfx &g = e.gfx();
                ui::clearScreen(g);
                ui::title(g, "DOOM");
                ui::hint(g, 190, "COPYING THE GAME DATA", ui::TEXT);
                ui::hint(g, 216, "INTO THE WATCH...", ui::TEXT);
                e.presenter().present(g);
                s_progress_engine = &e;
                const bool ok = doom_wad_install_file(path, installProgress);
                s_progress_engine = nullptr;
                ESP_LOGI(TAG, "install %s", ok ? "done" : "FAILED");
                doom_wad_info(&have);
            }
        }
        if (!have.present) {
            screen = NO_WAD;
            return;
        }
        screen = LOADING;
        doom_port_set_sfx_volume(15);
        if (!doom_port_start(skill)) screen = FAILED;
    }

    void updateTilt(const InputState &in, float dt)
    {
        const float k = 1.0f - std::exp(-dt / 0.10f);
        grav_x += (in.tilt.ax - grav_x) * k;
        grav_y += (in.tilt.ay - grav_y) * k;
        grav_z += (in.tilt.az - grav_z) * k;
    }

    void updateControls(const InputState &in, float dt, int64_t now)
    {
        updateTilt(in, dt);
        const float upright = std::sqrt(grav_x * grav_x + grav_y * grav_y);
        int forward = 0, turn = 0;
        if (upright > 0.3f) {
            // The wheel: clockwise tips gravity toward +x in screen space.
            roll = std::atan2(grav_x, grav_y);
            float s = roll / FULL_LOCK;
            const float dead = 0.10f;
            s = std::fabs(s) < dead ? 0 : (s - std::copysign(dead, s)) / (1 - dead);
            s = clampf(s, -1, 1);
            s = 0.45f * s + 0.55f * s * std::fabs(s);   // fine aim near the middle
            turn = int(-s * MAX_TURN);

            // Forward/back lean, measured from however it's being held.
            pitch = std::atan2(grav_z, upright);
            if (neutral_pending) {
                settle += dt;
                if (settle > 0.5f) pitch_neutral = pitch, neutral_pending = false;
            } else {
                float p = pitch - pitch_neutral;
                p = std::fabs(p) < WALK_DEAD ? 0 : p - std::copysign(WALK_DEAD, p);
                forward = int(clampf(p / WALK_RANGE[walk_sens], -1, 1) * 50);
            }
        }

        // Tap or hold = fire. A swipe isn't: wait a moment to see which it is.
        const Touch &t = in.touch;
        if (t.pressed) touch_t0 = t.t_us, touch_x0 = t.x, touch_y0 = t.y, swiping = false;
        if (t.down && (std::abs(t.x - touch_x0) > 30 || std::abs(t.y - touch_y0) > 30)) swiping = true;
        const bool holding = t.down && !swiping && now - touch_t0 > 90000;
        if (ges.tap) {
            fire_until = now + 110000;
            if (status.dead) use_until = now + 110000;   // Doom restarts the level on "use"
        }
        const bool fire = holding || now < fire_until;
        const bool use = (in.held & BTN_B) || now < use_until;
        doom_port_controls(forward, turn, fire, use);
        if (ges.swipe_right) doom_port_next_weapon();
    }

    void openMenu()
    {
        menu = true;
        menu_dirty = true;
        doom_port_set_active(false);
        doom_port_controls(0, 0, false, false);
    }

    void closeMenu()
    {
        menu = false;
        force = true;
        neutral_pending = true, settle = 0;   // the grip may have changed
        doom_port_set_active(true);
    }

    void menuTap(Engine &e, int x, int y)
    {
        if (ui::rowRect(0).hit(x, y)) {
            skill = (skill + 1) % 5;
            save();
        } else if (ui::rowRect(1).hit(x, y)) {
            wc::audio::setVolume(wc::audio::volume() == 0 ? 2 : 0);
        } else if (ui::rowRect(2).hit(x, y)) {
            walk_sens = (walk_sens + 1) % 3;
            save();
        } else if (ui::rowRect(3).hit(x, y)) {
            doom_port_new_game(skill);
            closeMenu();
            return;
        } else if (ui::buttonRect(0, 2).hit(x, y)) {
            closeMenu();
            return;
        } else if (ui::buttonRect(1, 2).hit(x, y)) {
            menu = false;
            e.goHome();
            return;
        }
        menu_dirty = true;
    }

    void update(Engine &e, float dt)
    {
        const InputState &in = e.input();
        ges.update(in.touch);
        const int64_t now = esp_timer_get_time();
        if (screen == NO_WAD || screen == FAILED) {
            if (ges.tap || ges.swipe_right || (in.clicked & BTN_B)) e.goHome();
            return;
        }
        doom_port_status(&status);
        if (status.state == DOOM_FAILED) {
            if (screen != FAILED) screen = FAILED, static_dirty = true;
            return;
        }
        if (screen == LOADING && status.state != DOOM_LOADING && status.frames > 0) screen = PLAYING, force = true;
        if (menu) {
            if (ges.tap) menuTap(e, ges.x, ges.y);
            else if (ges.swipe_right || (in.clicked & BTN_B)) closeMenu();
            return;
        }
        if (screen != PLAYING) return;
        if (ges.swipe_left) return openMenu();
        if (status.state == DOOM_TITLE && ges.tap) doom_port_new_game(skill);
        updateControls(in, dt, now);
    }

    void drawMenu(Gfx &g)
    {
        ui::clearScreen(g);
        ui::title(g, "PAUSED");
        ui::row(g, 0, "SKILL", SKILL_NAMES[skill]);
        ui::row(g, 1, "SOUND", wc::audio::volume() ? "ON" : "OFF", wc::audio::volume() ? ui::GO : ui::DIM);
        ui::row(g, 2, "WALK TILT", WALK_NAMES[walk_sens]);
        ui::row(g, 3, "NEW GAME", "GO", ui::ACCENT);
        ui::button(g, ui::buttonRect(0, 2), "RESUME");
        ui::outlineButton(g, ui::buttonRect(1, 2), "HOME");
    }

    void drawStatic(Gfx &g)
    {
        ui::clearScreen(g);
        ui::title(g, "DOOM", ui::TITLE_Y, ui::DANGER);
        if (screen == NO_WAD) {
            ui::hint(g, 130, "DOOM NEEDS ITS GAME DATA", ui::TEXT);
            ui::hint(g, 176, "1  SETTINGS > USB DRIVE: ON", ui::LABEL);
            ui::hint(g, 204, "2  PLUG INTO A COMPUTER", ui::LABEL);
            ui::hint(g, 232, "3  COPY DOOM1.WAD INTO A", ui::LABEL);
            ui::hint(g, 256, "FOLDER CALLED DOOM", ui::LABEL);
            ui::hint(g, 284, "4  EJECT, THEN OPEN DOOM", ui::LABEL);
            ui::hint(g, 330, "SHAREWARE DOOM1.WAD IS FREE", ui::DIM);
            ui::outlineButton(g, ui::buttonRect(0, 1), "HOME");
        } else if (screen == FAILED) {
            ui::hint(g, 170, "DOOM STOPPED:", ui::TEXT);
            char line[29];
            const char *msg = doom_port_error() ? doom_port_error() : "COULDN'T START";
            for (int i = 0; i < 4 && *msg; i++) {
                snprintf(line, sizeof(line), "%s", msg);
                for (char *c = line; *c; c++) *c = (*c >= 'a' && *c <= 'z') ? *c - 32 : (*c < 32 ? ' ' : *c);
                ui::hint(g, 204 + i * 24, line, ui::ACCENT);
                msg += std::strlen(line);
            }
            ui::hint(g, 320, "RESTART THE WATCH TO RETRY", ui::DIM);
            ui::outlineButton(g, ui::buttonRect(0, 1), "HOME");
        } else {
            ui::hint(g, 220, "LOADING...", ui::TEXT);
            ui::hint(g, 262, "HOLD IT LIKE A STEERING WHEEL", ui::DIM);
        }
    }

    // The finished Doom frame, turned to stay upright and stretched to the round screen.
    void present(Engine &e)
    {
        uint32_t gen;
        const uint8_t *rgb8 = doom_port_palette(&gen);
        if (gen != pal_gen) {
            pal_gen = gen;
            for (int i = 0; i < 256; i++) pal[i] = wc::rgb(rgb8[i * 3], rgb8[i * 3 + 1], rgb8[i * 3 + 2]);
        }
        const uint8_t *src = doom_port_frame();
        const float angle = roll;
        e.presenter().presentBands([this, src, angle](int y0, int rows, int x0, int w, Color *dst) {
            Color lut[256];
            std::memcpy(lut, pal, sizeof(lut));
            const float c = std::cos(angle), s = std::sin(angle);
            const float sc = Gfx::W / 2.0f - 0.5f;
            // Screen -> upright picture is the inverse rotation; then each axis has its own scale.
            const float ux = c / SCALE_X, uy = -s / SCALE_X;   // d(src x) per screen x, y
            const float vx = s / SCALE_Y, vy = c / SCALE_Y;    // d(src y) per screen x, y
            const int32_t du = int32_t(ux * 2 * 65536), dv = int32_t(vx * 2 * 65536);
            for (int r = 0; r < rows; r++) {
                const float dx = x0 + 0.5f - sc, dy = (y0 + r) - sc;
                uint32_t u = uint32_t(int32_t((DOOM_W / 2.0f + ux * dx + uy * dy) * 65536));
                uint32_t v = uint32_t(int32_t((DOOM_H / 2.0f + vx * dx + vy * dy) * 65536));
                Color *out = dst + r * w;
                for (int x = 0; x + 1 < w; x += 2) {
                    const uint32_t ui_ = u >> 16, vi = v >> 16;
                    const Color px = (ui_ < uint32_t(DOOM_W) && vi < uint32_t(DOOM_H)) ? lut[src[vi * DOOM_W + ui_]] : Color(0);
                    out[x] = px;
                    out[x + 1] = px;
                    u += du;
                    v += dv;
                }
                if (w & 1) out[w - 1] = out[w - 2];
            }
        });
    }

    float shown_roll = 99;

    void draw(Engine &e, Gfx &g)
    {
        if (menu) {
            if (menu_dirty) drawMenu(g), menu_dirty = false;
            return;
        }
        if (screen != PLAYING) {
            if (static_dirty) drawStatic(g), static_dirty = false;
            return;
        }
        // Only send a frame when there's something new: a Doom frame, or the watch turned.
        if (!force && status.frames == shown_frames && std::fabs(roll - shown_roll) < 0.004f) return;
        force = false;
        shown_frames = status.frames;
        shown_roll = roll;
        present(e);
    }
};

Doom::Doom() : s_(new State) {}
Doom::~Doom() { delete s_; }

void Doom::begin(Engine &e) { s_->load(); }

void Doom::enter(Engine &e)
{
    s_->menu = false;
    s_->static_dirty = true;
    s_->force = true;
    s_->neutral_pending = true, s_->settle = 0;
    s_->prepare(e);
    doom_port_set_active(true);
}

void Doom::leave(Engine &e)
{
    doom_port_controls(0, 0, false, false);
    doom_port_set_active(false);
}

void Doom::update(Engine &e, float dt) { s_->update(e, std::min(dt, 0.1f)); }
void Doom::draw(Engine &e, Gfx &g) { s_->draw(e, g); }
void Doom::redraw() { s_->force = s_->menu_dirty = s_->static_dirty = true; }
bool Doom::keepAwake() const { return false; }

}  // namespace games
