#include "games/jump.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "audio/audio.h"
#include "console/pause_menu.h"
#include "console/ui.h"
#include "engine/canvas.h"
#include "engine/gestures.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "nvs.h"

using namespace wc;

// Sprite sheets, embedded from components/games/assets/jump/ (see tools/make_sprites.py).
extern "C" {
extern const uint8_t _binary_hopper_png_start[], _binary_hopper_png_end[];
extern const uint8_t _binary_monster_png_start[], _binary_monster_png_end[];
extern const uint8_t _binary_ledges_png_start[], _binary_ledges_png_end[];
extern const uint8_t _binary_spring_png_start[], _binary_spring_png_end[];
extern const uint8_t _binary_shot_png_start[], _binary_shot_png_end[];
extern const uint8_t _binary_clouds_png_start[], _binary_clouds_png_end[];
}

namespace games {

namespace {

const char *TAG = "jump";

// The game is drawn on a 155x155 pixel canvas that the presenter scales 3x, so
// every sprite pixel is a chunky 3x3 block on the 466 px screen. All positions
// below are canvas pixels.
constexpr int SCALE = 3;
constexpr int W = (Gfx::W + SCALE - 1) / SCALE, H = W;
constexpr float PI = 3.14159265f;

// World y points UP (0 = the ground).
constexpr float GRAVITY = 930.0f;     // px/s^2
constexpr float JUMP_V = 390.0f;      // a normal bounce reaches 82 px (about half the screen)
constexpr float SPRING_V = 700.0f;    // a spring reaches 263 px
constexpr float BULLET_V = 370.0f;
constexpr float PLAYER_HALF = 8.0f;   // half of Hopper's body width, for landing / collisions
constexpr float PLAYER_H = 22.0f;
constexpr float PLAT_W = 30.0f, PLAT_H = 7.0f;
constexpr float MONSTER_W = 18.0f, MONSTER_H = 16.0f;
constexpr int PLAYER_LINE = 79;       // Hopper never rises above this canvas y; the world scrolls instead
constexpr float TILT_GAIN[3] = {320.0f, 470.0f, 650.0f};   // px/s at full tilt: LOW / MED / HIGH
constexpr float PX_PER_M = 4.0f;      // score is height in "metres"
constexpr int SKY_STEPS = 32;         // palette entries for the sky gradient

uint32_t rnd(uint32_t n) { return esp_random() % n; }
float frand() { return (esp_random() & 0xFFFF) / 65535.0f; }
float clampf(float v, float a, float b) { return std::max(a, std::min(b, v)); }
float lerp(float a, float b, float t) { return a + (b - a) * t; }
Color mix(uint8_t r0, uint8_t g0, uint8_t b0, uint8_t r1, uint8_t g1, uint8_t b1, float t)
{
    return rgb(uint8_t(lerp(r0, r1, t)), uint8_t(lerp(g0, g1, t)), uint8_t(lerp(b0, b1, t)));
}

enum PType : uint8_t { NORMAL, MOVING, CRUMBLE, SPRING, GROUND };

struct Plat {
    float x, y;        // centre x, top y (world)
    float w;           // landing width
    PType type;
    float base_x, phase, speed;   // MOVING: swings around base_x
    bool broken;       // CRUMBLE after someone stepped on it
    float fall_t;
};
struct Monster {
    float x, y;        // centre x, bottom y (world)
    float base_x, phase;
    bool alive;
};
struct Bullet {
    float x, y;
};
struct Puff {
    float x, y, vx, vy, t;
    uint8_t c;
};
struct Cloud {
    float x, y;        // y in the slow parallax layer
    uint8_t kind;
};
struct Star {
    uint8_t x, y, size;
};

enum Phase { READY, PLAYING, DEAD, GAME_OVER };

namespace sfx {
using wc::audio::Tone;
using wc::audio::Wave;

void boing(float pitch01)
{
    const float f = 260 + 140 * pitch01;
    wc::audio::play({.f0 = f, .f1 = f * 2.1f, .ms = 70, .wave = Wave::Triangle, .volume = 0.6f});
}
void spring()
{
    const Tone t[] = {{.f0 = 300, .f1 = 1500, .ms = 140, .wave = Wave::Square, .volume = 0.55f},
                      {.f0 = 900, .f1 = 1800, .ms = 90, .wave = Wave::Triangle, .volume = 0.5f, .delay_ms = 60}};
    wc::audio::play(t, 2);
}
void crumble()
{
    const Tone t[] = {{.f0 = 200, .f1 = 90, .ms = 90, .wave = Wave::Noise, .volume = 0.6f},
                      {.f0 = 180, .f1 = 60, .ms = 120, .wave = Wave::Triangle, .volume = 0.4f}};
    wc::audio::play(t, 2);
}
void shoot() { wc::audio::play({.f0 = 1400, .f1 = 700, .ms = 45, .wave = Wave::Square, .volume = 0.35f}); }
void stomp()
{
    const Tone t[] = {{.f0 = 160, .f1 = 60, .ms = 110, .wave = Wave::Square, .volume = 0.7f},
                      {.f0 = 500, .f1 = 1000, .ms = 90, .wave = Wave::Triangle, .volume = 0.5f, .delay_ms = 90}};
    wc::audio::play(t, 2);
}
void pop()
{
    const Tone t[] = {{.f0 = 700, .f1 = 1400, .ms = 60, .wave = Wave::Square, .volume = 0.5f},
                      {.f0 = 300, .f1 = 100, .ms = 80, .wave = Wave::Noise, .volume = 0.45f, .delay_ms = 40}};
    wc::audio::play(t, 2);
}
void hurt()
{
    const Tone t[] = {{.f0 = 400, .f1 = 150, .ms = 200, .wave = Wave::Square, .volume = 0.7f},
                      {.f0 = 120, .f1 = 60, .ms = 200, .wave = Wave::Noise, .volume = 0.4f}};
    wc::audio::play(t, 2);
}
void fall() { wc::audio::play({.f0 = 900, .f1 = 80, .ms = 600, .wave = Wave::Triangle, .volume = 0.6f}); }
void start() { wc::audio::play({.f0 = 500, .f1 = 900, .ms = 100, .wave = Wave::Triangle, .volume = 0.6f}); }
void newBest()
{
    const Tone t[] = {{.f0 = 660, .ms = 90, .wave = Wave::Square, .volume = 0.6f, .delay_ms = 0},
                      {.f0 = 880, .ms = 90, .wave = Wave::Square, .volume = 0.6f, .delay_ms = 100},
                      {.f0 = 1100, .ms = 90, .wave = Wave::Square, .volume = 0.6f, .delay_ms = 200},
                      {.f0 = 1320, .ms = 260, .wave = Wave::Square, .volume = 0.7f, .delay_ms = 300}};
    wc::audio::play(t, 4);
}
void gameOver()
{
    const Tone t[] = {{.f0 = 392, .ms = 160, .wave = Wave::Square, .volume = 0.6f, .delay_ms = 0},
                      {.f0 = 330, .ms = 160, .wave = Wave::Square, .volume = 0.6f, .delay_ms = 160},
                      {.f0 = 262, .ms = 320, .wave = Wave::Square, .volume = 0.6f, .delay_ms = 320}};
    wc::audio::play(t, 3);
}
}  // namespace sfx

}  // namespace

struct Jump::State {
    // ---- run
    Phase phase = READY;
    float phase_t = 0;
    int score = 0, best = 0;   // metres
    bool got_best = false;

    // ---- Hopper
    float px = 0, py = 0;      // feet
    float vx = 0, vy = 0;
    float prev_y = 0;
    int facing = 1;
    float squash = 0;          // 1 right after a bounce, decays
    float fire_t = 0;
    float blink_t = 0;
    float cam = 0;             // world y at the bottom edge of the screen
    float top_y = 0;           // highest feet position so far

    // ---- world
    std::vector<Plat> plats;
    std::vector<Monster> monsters;
    std::vector<Bullet> bullets;
    std::vector<Puff> puffs;
    Cloud clouds[6];
    Star stars[70];
    float gen_y = 0;           // next platform goes about here
    float next_monster_y = 0;
    float t = 0;               // run time, for animation

    // ---- settings
    int tilt_sens = 1;         // index into TILT_GAIN

    // ---- menu / input
    Gestures ges;
    console::ui::PauseMenu menu;

    // ---- drawing
    Canvas canvas;
    Sheet hopper, monster, ledges, spring, shot, clouds_sheet;
    uint8_t sky0 = 0;          // first of SKY_STEPS gradient entries
    uint8_t cloud_c = 0, cloud_shade = 0;
    uint8_t c_white = 0, c_black = 0, c_star = 0, c_star2 = 0, c_dim = 0, c_accent = 0, c_danger = 0;
    uint8_t c_panel = 0, c_box = 0, c_grass = 0, c_tuft = 0, c_dirt = 0, c_speck = 0, c_orange = 0;
    uint8_t c_purple = 0, c_stone = 0;
    uint32_t draw_us = 0, frames = 0;
    int64_t fps_t0 = 0;

    // ------------------------------------------------------------------ persistence
    void load()
    {
        nvs_handle_t h;
        if (nvs_open("jump", NVS_READONLY, &h) != ESP_OK) return;
        int32_t v;
        if (nvs_get_i32(h, "best", &v) == ESP_OK) best = v;
        uint8_t s;
        if (nvs_get_u8(h, "tilt", &s) == ESP_OK && s < 3) tilt_sens = s;
        nvs_close(h);
    }
    void save()
    {
        nvs_handle_t h;
        if (nvs_open("jump", NVS_READWRITE, &h) != ESP_OK) return;
        nvs_set_i32(h, "best", best);
        nvs_set_u8(h, "tilt", uint8_t(tilt_sens));
        nvs_commit(h);
        nvs_close(h);
    }

    // ------------------------------------------------------------------ assets
    bool loadAssets()
    {
        if (!canvas.init(SCALE)) return false;
        // Fixed colours first so they get stable indices.
        c_black = canvas.color(rgb(0, 0, 0));
        c_white = canvas.color(rgb(255, 255, 255));
        c_star = canvas.color(rgb(200, 205, 230));
        c_star2 = canvas.color(rgb(255, 240, 200));
        c_dim = canvas.color(rgb(220, 230, 245));
        c_accent = canvas.color(colors::yellow);
        c_danger = canvas.color(colors::red);
        c_panel = canvas.color(rgb(16, 18, 26));
        c_box = canvas.color(rgb(90, 90, 100));
        c_grass = canvas.color(rgb(80, 200, 95));
        c_tuft = canvas.color(rgb(170, 245, 150));
        c_dirt = canvas.color(rgb(150, 100, 55));
        c_speck = canvas.color(rgb(110, 70, 40));
        c_orange = canvas.color(rgb(255, 185, 40));
        c_purple = canvas.color(rgb(175, 65, 210));
        c_stone = canvas.color(rgb(130, 130, 140));
        sky0 = canvas.reserve(SKY_STEPS);

        bool ok = true;
        ok &= canvas.loadSheet(hopper, _binary_hopper_png_start, _binary_hopper_png_end - _binary_hopper_png_start, 20, 22);
        ok &= canvas.loadSheet(monster, _binary_monster_png_start, _binary_monster_png_end - _binary_monster_png_start, 18, 16);
        ok &= canvas.loadSheet(ledges, _binary_ledges_png_start, _binary_ledges_png_end - _binary_ledges_png_start, 30, 7);
        ok &= canvas.loadSheet(spring, _binary_spring_png_start, _binary_spring_png_end - _binary_spring_png_start, 12, 7);
        ok &= canvas.loadSheet(shot, _binary_shot_png_start, _binary_shot_png_end - _binary_shot_png_start, 4, 4);
        ok &= canvas.loadSheet(clouds_sheet, _binary_clouds_png_start, _binary_clouds_png_end - _binary_clouds_png_start, 20, 8);
        // The cloud colours are tinted per frame; these are the sheet's own entries.
        cloud_c = canvas.color(rgb(240, 248, 255));
        cloud_shade = canvas.color(rgb(200, 215, 240));
        return ok;
    }

    // ------------------------------------------------------------------ world building
    float difficulty() const { return clampf(top_y / PX_PER_M / 1800.0f, 0.0f, 1.0f); }

    float screenY(float wy) const { return H - (wy - cam); }

    void addPlat(float x, float y, PType type)
    {
        Plat p{};
        p.x = p.base_x = clampf(x, 20, W - 20);
        p.y = y;
        p.w = PLAT_W;
        p.type = type;
        p.phase = frand() * 2 * PI;
        p.speed = 1.2f + frand() * 1.2f + difficulty() * 1.5f;
        plats.push_back(p);
    }

    // Keep the tower going up to a screen-height above the camera.
    void generate()
    {
        const float d = difficulty();
        while (gen_y < cam + H + 40) {
            const float gap = lerp(17, 28, frand()) + d * lerp(10, 40, frand());
            gen_y += gap;
            const float r = frand();
            PType type = NORMAL;
            if (r < 0.06f + 0.02f * d) type = SPRING;
            else if (r < 0.12f + 0.30f * d) type = MOVING;
            addPlat(20 + frand() * (W - 40), gen_y, type);

            // Crumbling ledges are extras, never the only way up.
            if (frand() < 0.10f + 0.35f * d) {
                const float cy = gen_y + gap * 0.45f;
                addPlat(20 + frand() * (W - 40), cy, CRUMBLE);
            }
            if (gen_y > next_monster_y && top_y / PX_PER_M > 120) {
                Monster m{};
                m.x = m.base_x = 30 + frand() * (W - 60);
                m.y = gen_y + gap * 0.5f + 8;
                m.phase = frand() * 2 * PI;
                m.alive = true;
                monsters.push_back(m);
                next_monster_y = gen_y + lerp(470, 230, d) + frand() * 200;
            }
        }
    }

    void newGame()
    {
        plats.clear();
        monsters.clear();
        bullets.clear();
        puffs.clear();
        px = W / 2.0f;
        py = 0;
        prev_y = 0;
        vx = vy = 0;
        facing = 1;
        squash = 0;
        cam = -(H - 127);   // the ground sits at canvas y 127
        top_y = 0;
        score = 0;
        got_best = false;
        t = 0;
        // Solid ground to start on, then the tower.
        plats.push_back(Plat{W / 2.0f, 0, float(W) + 60, GROUND, W / 2.0f, 0, 0, false, 0});
        gen_y = 0;
        next_monster_y = 830;
        generate();
        for (Cloud &c : clouds) {
            c.x = frand() * W;
            c.y = cam * 0.35f + frand() * H;
            c.kind = uint8_t(rnd(2));
        }
        for (Star &s : stars) {
            s.x = uint8_t(rnd(W));
            s.y = uint8_t(rnd(H));
            s.size = uint8_t(1 + (rnd(4) == 0));
        }
        phase = READY;
        phase_t = 0;
    }

    void beginPlay()
    {
        phase = PLAYING;
        phase_t = 0;
        vy = JUMP_V;
        squash = 1;
        sfx::start();
    }

    // ------------------------------------------------------------------ simulation
    void spawnPuffs(float x, float y, int n, uint8_t c, float speed)
    {
        for (int i = 0; i < n; i++) {
            const float a = frand() * 2 * PI, s = speed * (0.4f + frand() * 0.8f);
            puffs.push_back(Puff{x, y, std::cos(a) * s, std::sin(a) * s + speed * 0.3f, 0, c});
        }
    }

    void die()
    {
        phase = DEAD;
        phase_t = 0;
        vy = 170;   // a little hop, then the long fall
        sfx::hurt();
        spawnPuffs(px, py + PLAYER_H / 2, 10, c_orange, 90);
    }

    void step(const InputState &in, float dt)
    {
        t += dt;

        // Steering: tilt the watch left or right. Full speed at about 25 degrees.
        const float gain = TILT_GAIN[tilt_sens];
        const float target = clampf(in.tilt.ax / 0.42f, -1.0f, 1.0f) * gain;
        vx += (target - vx) * std::min(1.0f, dt * 14);
        if (std::fabs(vx) > 20) facing = vx > 0 ? 1 : -1;
        px += vx * dt;
        if (px < -PLAYER_HALF) px += W + 2 * PLAYER_HALF;
        if (px > W + PLAYER_HALF) px -= W + 2 * PLAYER_HALF;

        prev_y = py;
        vy -= GRAVITY * dt;
        py += vy * dt;

        // Shooting: hold to keep firing.
        fire_t -= dt;
        if (phase == PLAYING && in.touch.down && fire_t <= 0) {
            bullets.push_back(Bullet{px, py + PLAYER_H * 0.8f});
            fire_t = 0.22f;
            sfx::shoot();
        }

        // Platforms move, crumbled ones fall away.
        for (Plat &p : plats) {
            if (p.type == MOVING) {
                p.x = p.base_x + std::sin(p.phase + t * p.speed) * 28;
                p.x = clampf(p.x, 17, W - 17);
            }
            if (p.broken) {
                p.fall_t += dt;
                p.y -= 170 * p.fall_t * dt;
            }
        }
        for (Monster &m : monsters) m.x = m.base_x + std::sin(m.phase + t * 1.3f) * 13;

        // Landing (only when falling, only on the way through a platform's top).
        if (phase == PLAYING && vy < 0) {
            for (Plat &p : plats) {
                if (p.broken) continue;
                if (prev_y < p.y || py > p.y) continue;
                if (std::fabs(px - p.x) > p.w / 2 + PLAYER_HALF * 0.6f) continue;
                if (p.type == CRUMBLE) {
                    p.broken = true;
                    p.fall_t = 0;
                    sfx::crumble();
                    spawnPuffs(p.x, p.y, 8, c_stone, 60);
                    continue;   // no bounce: it gives way
                }
                py = p.y;
                vy = p.type == SPRING ? SPRING_V : JUMP_V;
                squash = 1;
                if (p.type == SPRING) {
                    sfx::spring();
                    spawnPuffs(p.x, p.y, 6, c_white, 70);
                } else {
                    sfx::boing(clampf((p.y - top_y) / 70.0f + 0.5f, 0, 1));
                }
                break;
            }

            for (Monster &m : monsters) {
                if (!m.alive) continue;
                if (std::fabs(px - m.x) > MONSTER_W / 2 + PLAYER_HALF * 0.6f) continue;
                const float m_top = m.y + MONSTER_H;
                if (prev_y >= m_top - 4 && py <= m_top && py >= m.y) {
                    m.alive = false;
                    py = m_top;
                    vy = JUMP_V * 1.15f;
                    squash = 1;
                    sfx::stomp();
                    spawnPuffs(m.x, m.y + MONSTER_H / 2, 14, c_purple, 100);
                    break;
                }
            }
        }
        // Running into a monster from the side or below.
        if (phase == PLAYING) {
            for (Monster &m : monsters) {
                if (!m.alive) continue;
                if (std::fabs(px - m.x) < MONSTER_W / 2 + PLAYER_HALF * 0.7f && py + PLAYER_H * 0.8f > m.y + 3 &&
                    py < m.y + MONSTER_H - 3) {
                    die();
                    break;
                }
            }
        }

        // Bullets.
        for (size_t i = 0; i < bullets.size();) {
            Bullet &b = bullets[i];
            b.y += BULLET_V * dt;
            bool gone = screenY(b.y) < -4;
            for (Monster &m : monsters) {
                if (!m.alive) continue;
                if (std::fabs(b.x - m.x) < MONSTER_W / 2 && b.y > m.y && b.y < m.y + MONSTER_H) {
                    m.alive = false;
                    gone = true;
                    sfx::pop();
                    spawnPuffs(m.x, m.y + MONSTER_H / 2, 14, c_purple, 100);
                    break;
                }
            }
            if (gone) bullets.erase(bullets.begin() + i);
            else i++;
        }

        // Particles.
        for (size_t i = 0; i < puffs.size();) {
            Puff &q = puffs[i];
            q.t += dt;
            q.vy -= 300 * dt;
            q.x += q.vx * dt;
            q.y += q.vy * dt;
            if (q.t > 0.6f) puffs.erase(puffs.begin() + i);
            else i++;
        }

        squash = std::max(0.0f, squash - dt * 7);
        blink_t -= dt;
        if (blink_t < -3.0f - frand() * 3) blink_t = 0.12f;

        // Camera follows Hopper up, never down.
        if (phase == PLAYING) {
            const float limit = py - (H - PLAYER_LINE);
            if (limit > cam) cam = limit;
            if (py > top_y) top_y = py;
            score = int(top_y / PX_PER_M);
            if (score > best) {
                if (!got_best && best > 0) sfx::newBest();
                got_best = true;
                best = score;
            }
            generate();
        }

        // Tidy up what scrolled off the bottom.
        plats.erase(std::remove_if(plats.begin(), plats.end(),
                                   [&](const Plat &p) { return p.y < cam - 30 || p.fall_t > 1.0f; }),
                    plats.end());
        monsters.erase(std::remove_if(monsters.begin(), monsters.end(),
                                      [&](const Monster &m) { return m.y + MONSTER_H < cam - 15 || !m.alive; }),
                       monsters.end());

        // Fell off the bottom of the screen.
        if (py < cam - 30) {
            if (phase == PLAYING) sfx::fall();
            phase = GAME_OVER;
            phase_t = 0;
            if (got_best) save();
            sfx::gameOver();
        }
    }

    void update(Engine &e, float dt)
    {
        const InputState &in = e.input();
        ges.update(in.touch);
        phase_t += dt;

        if (menu.isOpen()) {
            switch (menu.update(e, ges, in)) {
            case 0:
                tilt_sens = (tilt_sens + 1) % 3;
                save();
                break;
            case 1: menu.toggleSound(); break;
            case 2:
                newGame();
                menu.close();
                break;
            }
            return;
        }
        if (ges.swipe_left && (phase == PLAYING || phase == READY)) {
            menu.open();
            return;
        }

        switch (phase) {
        case READY:
            if (ges.tap) beginPlay();
            break;
        case PLAYING:
        case DEAD:
            step(in, dt);
            break;
        case GAME_OVER:
            if (ges.tap && phase_t > 0.7f) newGame();
            break;
        }
    }

    // ------------------------------------------------------------------ menu
    void drawMenu(Gfx &g)
    {
        namespace ui = console::ui;
        static const char *kSens[] = {"LOW", "MED", "HIGH"};
        char buf[16];
        snprintf(buf, sizeof(buf), "%d M", best);
        const ui::PauseMenu::Row rows[] = {{"TILT", kSens[tilt_sens]},
                                           menu.soundRow(),
                                           {"NEW GAME", "GO", ui::ACCENT},
                                           {"BEST", buf, ui::LABEL}};
        menu.draw(g, rows, 4);
    }

    // ------------------------------------------------------------------ drawing
    void drawSky(Canvas &c)
    {
        // Morning at the bottom of the tower, deep space at the top. The gradient
        // is SKY_STEPS palette entries, so it costs nothing to change every frame.
        const float alt = clampf(cam / PX_PER_M / 2600.0f, 0.0f, 1.0f);
        for (int i = 0; i < SKY_STEPS; i++) {
            const float k = float(i) / (SKY_STEPS - 1);   // 0 = top of screen
            c.setColor(uint8_t(sky0 + i), mix(72 + int(78 * k), 140 + int(75 * k), 235 + int(20 * k),
                                              6 + int(34 * k), 6 + int(18 * k), 24 + int(56 * k), alt));
        }
        uint8_t *row = c.pixels();
        for (int y = 0; y < H; y++, row += W) std::memset(row, sky0 + y * SKY_STEPS / H, W);

        if (alt > 0.35f) {
            const int n = int((alt - 0.35f) / 0.65f * 70);
            const int drift = int(cam * 0.08f);
            for (int i = 0; i < n; i++) {
                const Star &s = stars[i];
                const int y = ((s.y + drift) % H + H) % H;
                const bool twinkle = ((i * 7 + int(t * 3)) % 11) == 0;
                c.fillRect(s.x, y, s.size, s.size, twinkle ? c_star2 : alt > 0.7f ? c_white : c_star);
            }
        }
        if (alt < 0.85f) {
            c.setColor(cloud_c, mix(240, 248, 255, 150, 130, 200, alt));
            c.setColor(cloud_shade, mix(200, 215, 240, 110, 90, 160, alt));
            for (Cloud &cl : clouds) {
                // Clouds live in a slower layer, so they drift by as you climb.
                float sy = H - (cl.y - cam * 0.35f);
                if (sy > H + 20) {
                    cl.y += H + 40;
                    cl.x = frand() * W;
                    cl.kind = uint8_t(rnd(2));
                    sy = H - (cl.y - cam * 0.35f);
                }
                if (sy < -20) continue;
                c.sprite(clouds_sheet, cl.kind, int(cl.x) - 10, int(sy) - 8);
            }
        }
    }

    void drawGround(Canvas &c, const Plat &p)
    {
        const int sy = int(screenY(p.y));
        if (sy > H) return;
        // Grass on top, dirt below, all the way down.
        c.fillRect(0, sy, W, 1, c_tuft);
        c.fillRect(0, sy + 1, W, 3, c_grass);
        c.fillRect(0, sy + 4, W, std::max(0, H - sy - 4), c_dirt);
        for (int x = 2; x < W; x += 10) c.fillRect(x, sy + 7 + (x * 13) % 14, 2, 1, c_speck);
        for (int x = 1; x < W; x += 8) c.fillRect(x, sy - 2, 1, 2, c_tuft);
    }

    void drawPlat(Canvas &c, const Plat &p)
    {
        if (p.type == GROUND) {
            drawGround(c, p);
            return;
        }
        const int sy = int(screenY(p.y) + 0.5f);
        if (sy < -10 || sy > H + 10) return;
        const int frame = p.type == MOVING ? 1 : p.type == CRUMBLE ? 2 : 0;
        const int x = int(p.x + 0.5f) - 15;
        c.sprite(ledges, frame, x, sy);
        if (p.type == SPRING) c.sprite(spring, 0, x + 9, sy - 6);
    }

    void drawMonster(Canvas &c, const Monster &m)
    {
        const float bob = std::sin(t * 5 + m.phase);
        const float sy = screenY(m.y);
        if (sy < -20 || sy > H + 20) return;
        // Breathes: a little wider as it squats; snaps its mouth now and then.
        const int frame = std::fmod(t * 2 + m.phase, 4.0f) < 0.5f ? 1 : 0;
        c.spriteScaled(monster, frame, m.x, sy, 1 + 0.06f * bob, 1 - 0.06f * bob, px < m.x);
    }

    void drawHopper(Canvas &c)
    {
        const float sy = screenY(py);
        if (sy < -30 || sy > H + 40) return;
        // Squash on landing, stretch when moving fast.
        const float q = squash * squash;
        float sx = 1 + 0.35f * q, sy_ = 1 - 0.30f * q;
        const float stretch = clampf(std::fabs(vy) / 900.0f, 0, 0.18f);
        sx -= stretch * 0.5f;
        sy_ += stretch;
        const bool dead = phase == DEAD || phase == GAME_OVER;
        const int frame = dead ? 2 : blink_t > 0 ? 1 : 0;
        c.spriteScaled(hopper, frame, px, sy, sx, sy_, facing < 0);
    }

    void drawHud(Canvas &c)
    {
        char buf[24];
        snprintf(buf, sizeof(buf), "%d M", score);
        c.textCentered(W / 2 + 1, 12, buf, c_black, 1, true);
        c.textCentered(W / 2, 11, buf, c_white, 1, true);
        if (best > 0) {
            snprintf(buf, sizeof(buf), "BEST %d M", best);
            c.textCentered(W / 2 + 1, 143, buf, c_black, 1, true);
            c.textCentered(W / 2, 142, buf, got_best ? c_accent : c_dim, 1, true);
        }
    }

    void banner(Canvas &c, const char *top, const char *mid, const char *bottom, uint8_t col)
    {
        const int h = bottom ? 34 : mid ? 26 : 16;
        c.fillRect(22, 68, W - 44, h, c_panel);
        c.rect(22, 68, W - 44, h, c_box);
        c.textCentered(W / 2, 76, top, col, 1, true);
        if (mid) c.textCentered(W / 2, 86, mid, c_dim, 1, false);
        if (bottom) c.textCentered(W / 2, 95, bottom, c_dim, 1, false);
    }

    void draw(Engine &e, Gfx &g)
    {
        if (menu.isOpen()) {
            drawMenu(g);
            return;
        }
        const int64_t t0 = esp_timer_get_time();
        Canvas &c = canvas;
        drawSky(c);
        for (const Plat &p : plats) drawPlat(c, p);
        for (const Monster &m : monsters) drawMonster(c, m);
        for (const Bullet &b : bullets) c.sprite(shot, 0, int(b.x) - 2, int(screenY(b.y)) - 2);
        for (const Puff &q : puffs) {
            const int r = std::max(1, int(2.5f * (1 - q.t / 0.6f)));
            c.fillRect(int(q.x) - r / 2, int(screenY(q.y)) - r / 2, r, r, q.c);
        }
        drawHopper(c);
        drawHud(c);

        char buf[32];
        switch (phase) {
        case READY: banner(c, "TAP TO JUMP", "TILT TO STEER", "TAP TO SHOOT", c_white); break;
        case GAME_OVER:
            if (phase_t > 0.5f) {
                snprintf(buf, sizeof(buf), got_best ? "NEW BEST %d M!" : "%d M", score);
                banner(c, "GAME OVER", buf, nullptr, got_best ? c_accent : c_danger);
            }
            break;
        default: break;
        }
        draw_us += uint32_t(esp_timer_get_time() - t0);
        c.present(e.presenter());

        frames++;
        const int64_t now = esp_timer_get_time();
        if (now - fps_t0 > 5000000) {
            const PresentStats &ps = e.presenter().stats();
            ESP_LOGI(TAG, "%.1f fps (draw %.2f ms | wire %.1f, vsync wait %.1f, slot wait %.1f ms)",
                     frames * 1e6f / float(now - fps_t0), draw_us / 1000.0f / frames, ps.last_xfer_us / 1000.0f,
                     ps.last_vsync_wait_us / 1000.0f, ps.last_wait_us / 1000.0f);
            frames = 0;
            draw_us = 0;
            fps_t0 = now;
        }
    }
};

Jump::Jump() : s_(new State) {}
Jump::~Jump() { delete s_; }

void Jump::begin(Engine &e)
{
    s_->load();
    if (!s_->loadAssets()) ESP_LOGE(TAG, "assets failed to load");
    s_->newGame();
    ESP_LOGI(TAG, "ready, best %d m, %d palette colours", s_->best, s_->canvas.used());
}

void Jump::enter(Engine &e)
{
    // Coming back from the home screen mid-run: pause rather than let it run blind.
    if (s_->phase == PLAYING) {
        s_->menu.open();
    }
    s_->fps_t0 = esp_timer_get_time();
}

bool Jump::keepAwake() const { return !s_->menu.isOpen() && (s_->phase == PLAYING || s_->phase == DEAD); }

void Jump::update(Engine &e, float dt) { s_->update(e, std::min(dt, 1.0f / 30)); }

void Jump::draw(Engine &e, Gfx &g)
{
    if (!s_->canvas.pixels()) return;
    s_->draw(e, g);
}

}  // namespace games
