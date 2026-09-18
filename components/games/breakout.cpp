#include "games/breakout.h"

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
#include "engine/polar.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "nvs.h"

using namespace wc;

namespace games {

namespace {

const char *TAG = "breakout";

constexpr float PI = 3.14159265f;
constexpr float TAU = 2 * PI;
constexpr float C = 233.0f;
constexpr int W = Gfx::W, H = Gfx::H;
constexpr float R = 233;

constexpr float PADDLE_R = 212, PADDLE_T = 10, BALL_R = 6.5f, LOSE_R = R + 10, CORE_R = 34;
// Drawn on a 233 x 233 canvas shown at 2x; the game itself keeps screen units.
constexpr int SCALE = 2, CW = (Gfx::W + SCALE - 1) / SCALE;
constexpr float RING_IN = CORE_R + 10, GAP = 3;
constexpr int ROWS = 4;
constexpr float FIELD_R = 120, TAPER = 0.4f, BASE_SPEED = 200;
constexpr float SPEEDS[] = {160, 200, 250};
const char *const SPEED_NAMES[] = {"SLOW", "NORMAL", "FAST"};
constexpr int MAX_BALLS = 8, MAX_PARTS = 140;
constexpr float TAP_MAX_TRAVEL = 0.08f;   // radians of drag that still counts as a tap
constexpr float MAX_DEFLECT = 0.5f;       // max paddle bounce angle off-center (rad); < field half-angle

float frand() { return esp_random() / 4294967296.0f; }
float wrap(float a)
{
    a = std::fmod(a + PI, TAU);
    if (a < 0) a += TAU;
    return a - PI;
}
float norm(float a)
{
    a = std::fmod(a, TAU);
    return a < 0 ? a + TAU : a;
}
float clampf(float v, float a, float b) { return std::max(a, std::min(b, v)); }
uint16_t toA16(float a) { return uint16_t(int(norm(a) / TAU * 65536.0f) & 0xFFFF); }

struct RGB {
    float r, g, b;
};
constexpr RGB hex(uint32_t h) { return {float((h >> 16) & 255), float((h >> 8) & 255), float(h & 255)}; }
RGB lerp(RGB a, RGB b, float t) { return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t}; }
Color col(RGB c, float k = 1) { return rgb(uint8_t(c.r * k), uint8_t(c.g * k), uint8_t(c.b * k)); }

constexpr RGB PALETTE[] = {hex(0xff4f9a), hex(0xff8a3d), hex(0xffd23f), hex(0x5ee6a8), hex(0x4fb3ff)};
constexpr RGB WHITE = hex(0xffffff);

enum CapType { CAP_WIDE, CAP_MULTI, CAP_SLOW, CAP_SHIELD };
constexpr struct {
    char letter;
    RGB color;
} CAPS[] = {{'W', hex(0xffd23f)}, {'M', hex(0xff4f9a)}, {'S', hex(0x5ee6a8)}, {'O', hex(0x7df9ff)}};

namespace sfx {
using wc::audio::Tone;
using wc::audio::Wave;

void launch() { wc::audio::play({.f0 = 420, .f1 = 900, .ms = 70, .wave = Wave::Square, .volume = 0.7f}); }

void paddle()
{
    const Tone t[] = {{.f0 = 330, .f1 = 240, .ms = 45, .wave = Wave::Square, .volume = 0.8f},
                      {.f0 = 140, .f1 = 90, .ms = 40, .wave = Wave::Triangle, .volume = 0.5f}};
    wc::audio::play(t, 2);
}

// Each brick in a rally is a semitone higher: a rising run while the ball works.
void brick(int combo, bool destroyed)
{
    const float f = 523.0f * std::pow(1.0595f, float(std::min(combo, 18)));
    if (destroyed) {
        const Tone t[] = {{.f0 = f, .f1 = f * 1.5f, .ms = 55, .wave = Wave::Square, .volume = 0.75f},
                          {.f0 = 180, .f1 = 120, .ms = 35, .wave = Wave::Noise, .volume = 0.35f}};
        wc::audio::play(t, 2);
    } else {
        wc::audio::play({.f0 = f * 0.5f, .f1 = f * 0.4f, .ms = 45, .wave = Wave::Triangle, .volume = 0.6f});
    }
}

void powerUp()
{
    const Tone t[] = {{.f0 = 660, .ms = 70, .wave = Wave::Triangle, .volume = 0.7f, .delay_ms = 0},
                      {.f0 = 880, .ms = 70, .wave = Wave::Triangle, .volume = 0.7f, .delay_ms = 70},
                      {.f0 = 1320, .ms = 110, .wave = Wave::Triangle, .volume = 0.7f, .delay_ms = 140}};
    wc::audio::play(t, 3);
}

void shield() { wc::audio::play({.f0 = 900, .f1 = 1600, .ms = 120, .wave = Wave::Triangle, .volume = 0.6f}); }

void lifeLost()
{
    const Tone t[] = {{.f0 = 420, .f1 = 110, .ms = 320, .wave = Wave::Square, .volume = 0.8f},
                      {.f0 = 200, .f1 = 60, .ms = 260, .wave = Wave::Noise, .volume = 0.4f}};
    wc::audio::play(t, 2);
}

void levelClear()
{
    const Tone t[] = {{.f0 = 523, .ms = 90, .wave = Wave::Square, .volume = 0.7f, .delay_ms = 0},
                      {.f0 = 659, .ms = 90, .wave = Wave::Square, .volume = 0.7f, .delay_ms = 90},
                      {.f0 = 784, .ms = 90, .wave = Wave::Square, .volume = 0.7f, .delay_ms = 180},
                      {.f0 = 1047, .ms = 220, .wave = Wave::Square, .volume = 0.8f, .delay_ms = 270}};
    wc::audio::play(t, 4);
}

void gameOver()
{
    const Tone t[] = {{.f0 = 392, .ms = 160, .wave = Wave::Square, .volume = 0.7f, .delay_ms = 0},
                      {.f0 = 330, .ms = 160, .wave = Wave::Square, .volume = 0.7f, .delay_ms = 160},
                      {.f0 = 262, .ms = 320, .wave = Wave::Square, .volume = 0.7f, .delay_ms = 320}};
    wc::audio::play(t, 3);
}
}  // namespace sfx

enum Mode { MODE_DRAG, MODE_FOLLOW, MODE_TILT };
const char *const MODE_NAMES[] = {"DRAG", "FOLLOW", "TILT"};

struct Rect {
    int x, y, w, h;
    bool intersects(const Rect &o) const
    {
        return x < o.x + o.w && o.x < x + w && y < o.y + o.h && o.y < y + h;
    }
};

struct Ring {
    float r0, r1, gapPx, off, spd;
    int n, hp;
    RGB color;
    std::vector<int8_t> bricks;
    // shading, precomputed (palette indices)
    uint8_t full, dim, outline, shade, dim_shade;
    uint16_t r0_16, r1_16, gap_frac, outline_frac, off16;
};

struct Ball {
    float x, y, vx, vy;
    bool stuck, dead;
};
struct Cap {
    float a, r, v;
    CapType type;
    bool done;
};
struct Part {
    float x, y, vx, vy, life;
    RGB color;
};

// Little "+30" that floats up from a broken brick.
struct Pop {
    float x, y, life;
    int score;
    RGB color;
};

// A ring that snaps outward where the ball hit.
struct Shock {
    float x, y, life;
    RGB color;
};

// Pixel-accurate bbox of an annular arc, with a small safety margin.
Rect arcBox(float a0, float a1, float rin, float rout)
{
    float minx = 1e9, miny = 1e9, maxx = -1e9, maxy = -1e9;
    const int steps = std::max(2, int(std::ceil((a1 - a0) / 0.08f)));
    for (int i = 0; i <= steps; i++) {
        const float a = a0 + (a1 - a0) * i / steps, ca = std::cos(a), sa = std::sin(a);
        for (float r : {rin, rout}) {
            minx = std::min(minx, C + ca * r);
            maxx = std::max(maxx, C + ca * r);
            miny = std::min(miny, C + sa * r);
            maxy = std::max(maxy, C + sa * r);
        }
    }
    const int x0 = int(std::floor(minx)) - 2, y0 = int(std::floor(miny)) - 2;
    return {x0, y0, int(std::ceil(maxx)) + 2 - x0, int(std::ceil(maxy)) + 2 - y0};
}

}  // namespace

struct Breakout::State {
    // --- game state (mirrors the prototype)
    int score = 0, lives = 3, level = 1;
    float paddle = PI / 2, pv = 0, hw = 0.3f, target = 0;
    bool has_target = false;
    float wideT = 0, slowT = 0, speed = BASE_SPEED, flash = 0;
    bool shield = false, over = false;
    std::vector<Ring> rings;
    std::vector<Ball> balls;
    std::vector<Cap> caps;
    std::vector<Part> parts;
    std::vector<Pop> pops;
    std::vector<Shock> rings_fx;
    int combo = 0;          // bricks hit since the last paddle touch

    // --- settings (persisted in NVS)
    Mode mode = MODE_TILT;
    bool tilt_invert = false;
    int speed_idx = 1;
    float base_speed = BASE_SPEED;

    // --- menu
    console::ui::PauseMenu menu;

    // --- input
    Gestures ges;
    float lastA = 0, moved = 0;
    bool ptr_down = false, ptr_skip = false;
    float mode_label_t = 0;
    struct { int ring = -1, idx = 0; float time = 0; } flash_brick_;
    float grav_x = 0, grav_y = 1;   // smoothed gravity in screen axes (tilt mode)

    // --- rendering
    Canvas canvas;
    uint8_t *scene = nullptr;         // rings + core + backdrop, rebuilt when a brick changes
    bool full_redraw = true;          // scene is stale
    uint8_t c_black = 0, c_white = 0, c_track = 0, c_core = 0, c_core_edge = 0, c_shield = 0, c_star = 0;
    uint8_t c_star2 = 0, c_pad = 0, c_pad_flash = 0, c_pad_edge = 0, c_ball_rim = 0, c_life_off = 0;
    uint8_t c_cyan = 0, c_yellow = 0, c_panel = 0;

    // =================================================================== setup

    void buildRings()
    {
        rings.clear();
        float w[ROWS], total = 0;
        for (int i = 0; i < ROWS; i++) {
            w[i] = 1 + TAPER * 1.5f * (ROWS > 1 ? float(i) / (ROWS - 1) : 0);
            total += w[i];
        }
        const float span = FIELD_R - RING_IN;
        float r0 = RING_IN;
        for (int i = 0; i < ROWS; i++) {
            const float next = r0 + span * w[i] / total, r1 = next - GAP, th = r1 - r0, mid = (r0 + r1) / 2;
            Ring g{};
            g.r0 = r0;
            g.r1 = r1;
            g.n = std::max(8, int(std::lround(TAU * mid / (th * 1.7f))));
            g.gapPx = clampf(th / 10, 0.8f, 1.6f);
            g.hp = (i == 0 ? 2 : 1) + (level > 1 && i < 2 ? 1 : 0);
            const float t = ROWS > 1 ? float(i) / (ROWS - 1) * 4 : 0;
            const int k = std::min(int(t), 3);
            g.color = lerp(PALETTE[k], PALETTE[k + 1], t - k);
            g.off = frand() * TAU;
            g.off16 = toA16(g.off);
            g.spd = (i % 2 ? 1 : -1) * (0.1f + 0.03f * i);
            g.bricks.assign(g.n, int8_t(g.hp));

            g.full = canvas.color(col(g.color));
            g.dim = canvas.color(col(g.color, 0.85f));
            g.outline = canvas.color(col(lerp(g.color, WHITE, 0.75f)));
            g.shade = canvas.color(col(g.color, 0.55f));
            g.dim_shade = canvas.color(col(g.color, 0.5f));
            g.r0_16 = uint16_t(r0 * 16);
            g.r1_16 = uint16_t(r1 * 16);
            const float frac_per_rad = g.n / TAU * 65536.0f;
            g.gap_frac = uint16_t(g.gapPx / mid * frac_per_rad);
            g.outline_frac = uint16_t(1.5f / mid * frac_per_rad);
            rings.push_back(std::move(g));
            r0 = next;
        }
    }

    void newGame()
    {
        score = 0;
        lives = 3;
        level = 1;
        paddle = PI / 2;
        has_target = false;
        pv = 0;
        hw = 0.3f;
        wideT = slowT = flash = 0;
        shield = over = false;
        speed = base_speed;
        caps.clear();
        parts.clear();
        pops.clear();
        rings_fx.clear();
        combo = 0;
        buildRings();
        stickBall();
        full_redraw = true;
    }

    void stickBall()
    {
        balls.assign(1, Ball{0, 0, 0, 0, true, false});
        placeStuck();
    }

    void placeStuck()
    {
        const float rr = PADDLE_R - BALL_R - 2;
        for (auto &b : balls)
            if (b.stuck) {
                b.x = C + std::cos(paddle) * rr;
                b.y = C + std::sin(paddle) * rr;
            }
    }

    float spd() const { return speed * (slowT > 0 ? 0.65f : 1); }

    void launch()
    {
        bool any_stuck = false;
        for (const auto &b : balls) any_stuck |= b.stuck;
        if (any_stuck) sfx::launch();
        placeStuck();
        for (auto &b : balls)
            if (b.stuck) {
                const float dir = paddle + PI + (frand() - 0.5f) * 0.4f;
                b.vx = std::cos(dir) * spd();
                b.vy = std::sin(dir) * spd();
                b.stuck = false;
            }
    }

    // =================================================================== simulation

    static void reflect(Ball &b, float nx, float ny)
    {
        const float d = b.vx * nx + b.vy * ny;
        b.vx -= 2 * d * nx;
        b.vy -= 2 * d * ny;
    }

    void burst(float x, float y, RGB color, int n)
    {
        const float dx = x - C, dy = y - C, r = std::max(1.0f, std::hypot(dx, dy));
        for (int i = 0; i < n && int(parts.size()) < MAX_PARTS; i++) {
            const float a = frand() * TAU, v = 40 + frand() * 90;
            parts.push_back({x, y, std::cos(a) * v + dx / r * 60, std::sin(a) * v + dy / r * 60,
                             0.5f + frand() * 0.3f, color});
        }
    }

    Rect brickBox(const Ring &g, int idx) const
    {
        const float sec = TAU / g.n;
        return arcBox(g.off + idx * sec, g.off + (idx + 1) * sec, g.r0 - 1, g.r1 + 1);
    }

    // Paint one brick sector solid, so a hit flashes before the scene redraws it.
    void flashBrick(Canvas &c, const Ring &g, int idx, uint8_t col)
    {
        const Rect box = brickBox(g, idx);
        const uint32_t sec = 65536u / uint32_t(g.n);
        uint8_t *px = c.pixels();
        for (int y = std::max(0, box.y / 2); y < std::min(CW, (box.y + box.h) / 2 + 1); y++) {
            for (int x = std::max(0, box.x / 2); x < std::min(CW, (box.x + box.w) / 2 + 1); x++) {
                const int i = (2 * y) * W + 2 * x;
                const uint16_t r16 = Polar::radius16(i);
                if (r16 < g.r0_16 || r16 > g.r1_16) continue;
                const uint16_t rel = uint16_t(Polar::angle(i) - g.off16);
                if (rel / sec != uint32_t(idx)) continue;
                px[y * CW + x] = col;
            }
        }
    }

    void hitBrick(int gi, int idx)
    {
        Ring &g = rings[gi];
        g.bricks[idx]--;
        const float mid = g.off + (idx + 0.5f) * TAU / g.n, rm = (g.r0 + g.r1) / 2;
        const float x = C + std::cos(mid) * rm, y = C + std::sin(mid) * rm;
        const bool destroyed = g.bricks[idx] <= 0;
        combo++;
        sfx::brick(combo, destroyed);
        rings_fx.push_back({x, y, 1.0f, destroyed ? g.color : WHITE});
        flash_brick_ = {gi, idx, 0.06f};
        if (destroyed) {
            const int points = 10 * (int(rings.size()) - gi) * (combo >= 4 ? 2 : 1);   // rallies pay double
            score += points;
            burst(x, y, g.color, 14);
            pops.push_back({x, y, 1.0f, points, g.color});
            if (frand() < 0.12f) caps.push_back({mid, rm, 15, CapType(esp_random() % 4), false});
        } else {
            score += 2;
            burst(x, y, WHITE, 5);
        }
        full_redraw = true;
    }

    void applyCap(CapType t)
    {
        if (t == CAP_WIDE) wideT = 12;
        if (t == CAP_SLOW) slowT = 8;
        if (t == CAP_SHIELD) shield = true;
        if (t == CAP_MULTI) {
            std::vector<Ball> live;
            for (auto &b : balls)
                if (!b.stuck) live.push_back(b);
            if (live.empty()) {
                launch();
                return;
            }
            for (auto &b : live)
                for (float t2 : {0.4f, -0.4f}) {
                    if (int(balls.size()) >= MAX_BALLS) return;
                    const float c = std::cos(t2), sn = std::sin(t2);
                    balls.push_back({b.x, b.y, b.vx * c - b.vy * sn, b.vx * sn + b.vy * c, false, false});
                }
        }
    }

    void stepBall(Ball &b, float h)
    {
        const float px = b.x, py = b.y;
        b.x += b.vx * h;
        b.y += b.vy * h;
        const float dx = b.x - C, dy = b.y - C;
        const float r = std::max(0.001f, std::hypot(dx, dy)), pr = std::hypot(px - C, py - C);
        const float nx = dx / r, ny = dy / r;
        const float vr = b.vx * nx + b.vy * ny;

        // The center orb is a display (score/lives) only; the ball passes through it.

        if (shield && r >= PADDLE_R + 5 && pr < PADDLE_R + 5) {
            if (vr > 0) reflect(b, nx, ny);
            b.x = px;
            b.y = py;
            shield = false;
            sfx::shield();
            burst(b.x, b.y, CAPS[CAP_SHIELD].color, 14);
            return;
        }

        if (r + BALL_R >= PADDLE_R && pr + BALL_R < PADDLE_R && vr > 0) {
            const float th = std::atan2(dy, dx), d = wrap(th - paddle);
            if (std::fabs(d) <= hw + BALL_R / PADDLE_R) {
                const float k = clampf(d / hw, -1, 1);
                // Aim back toward the center, steered by where it hit the paddle.
                // Turning the watch in tilt mode moves the paddle on screen without the
                // player "swiping" it, so paddle-motion spin only applies to touch modes.
                const float spin = mode == MODE_TILT ? 0 : clampf(pv * 0.05f, -0.25f, 0.25f);
                // Cap the deflection so the ball always heads into the brick field:
                // from the paddle (r=212) the field (r=120) spans ±asin(120/212) ≈ ±0.60 rad.
                const float deflect = clampf(k * 0.45f + spin, -MAX_DEFLECT, MAX_DEFLECT);
                const float dir = th + PI - deflect;
                b.vx = std::cos(dir);
                b.vy = std::sin(dir);
                b.x = px;
                b.y = py;
                speed = std::min(speed + 2, base_speed * 1.6f);
                flash = 0.12f;
                combo = 0;
                sfx::paddle();
                return;
            }
        }

        if (r > LOSE_R) {
            b.dead = true;
            return;
        }

        const float th = std::atan2(dy, dx);
        for (int gi = 0; gi < int(rings.size()); gi++) {
            const Ring &g = rings[gi];
            if (r + BALL_R < g.r0 || r - BALL_R > g.r1) continue;
            const float sec = TAU / g.n, pad = BALL_R / r;
            for (float o : {0.0f, pad, -pad}) {
                const int idx = int(std::floor(norm(th + o - g.off) / sec)) % g.n;
                if (g.bricks[idx] > 0) {
                    const bool prevInBand = pr + BALL_R > g.r0 && pr - BALL_R < g.r1;
                    if (prevInBand) reflect(b, -ny, nx);
                    else if ((pr < g.r0 && vr > 0) || (pr > g.r1 && vr < 0)) reflect(b, nx, ny);
                    b.x = px;
                    b.y = py;
                    hitBrick(gi, idx);
                    return;
                }
            }
        }
    }

    // =================================================================== settings

    void loadSettings()
    {
        nvs_handle_t h;
        if (nvs_open("breakout", NVS_READONLY, &h) != ESP_OK) return;
        uint8_t v;
        if (nvs_get_u8(h, "mode", &v) == ESP_OK && v < 3) mode = Mode(v);
        if (nvs_get_u8(h, "invert", &v) == ESP_OK) tilt_invert = v;
        if (nvs_get_u8(h, "speed", &v) == ESP_OK && v < 3) speed_idx = v;
        nvs_close(h);
        base_speed = SPEEDS[speed_idx];
    }

    void saveSettings()
    {
        nvs_handle_t h;
        if (nvs_open("breakout", NVS_READWRITE, &h) != ESP_OK) return;
        nvs_set_u8(h, "mode", uint8_t(mode));
        nvs_set_u8(h, "invert", tilt_invert);
        nvs_set_u8(h, "speed", uint8_t(speed_idx));
        nvs_commit(h);
        nvs_close(h);
    }

    void setMode(Mode m)
    {
        mode = m;
        has_target = false;
        mode_label_t = 2.0f;
    }

    void openMenu()
    {
        menu.open();
        ptr_down = false;
    }

    void closeMenu()
    {
        menu.close();
        full_redraw = true;
        base_speed = SPEEDS[speed_idx];
        speed = std::min(base_speed * (1 + 0.08f * (level - 1)), base_speed * 1.5f);
        saveSettings();
    }

    void handleInput(Engine &e, float dt)
    {
        const InputState &in = e.input();
        const Touch &t = in.touch;
        const float ta = std::atan2(t.y + 0.5f - C, t.x + 0.5f - C);

        ges.update(t);

        if (menu.isOpen()) {
            const int row = menu.update(e, ges, in);
            if (row == 0) setMode(Mode((mode + 1) % 3));
            else if (row == 1) tilt_invert = !tilt_invert;
            else if (row == 2) speed_idx = (speed_idx + 1) % 3;
            else if (row == 3) menu.toggleSound();
            // RESUME and HOME are handled inside update(); either way the game resumes
            // through closeMenu()'s settings save.
            if (!menu.isOpen()) closeMenu();
            return;
        }
        if (ges.swipe_left && !over) {
            openMenu();
            return;
        }

        // PWR changes control; BOOT is the console's "home" (handled by the engine).
        if (in.clicked & BTN_B) {
            setMode(Mode((mode + 1) % 3));
            saveSettings();
        }

        if (t.pressed) {
            ptr_down = true;
            lastA = ta;
            moved = 0;
            ptr_skip = false;
            if (over) {
                newGame();
                ptr_skip = true;
            }
        }
        if (ptr_down && (t.down || t.released)) {
            if (mode == MODE_FOLLOW) {
                target = ta;
                has_target = true;
            } else if (mode == MODE_DRAG) {
                paddle = wrap(paddle + wrap(ta - lastA));
            }
            moved += std::fabs(wrap(ta - lastA));
            lastA = ta;
        }
        if (t.released && ptr_down) {
            if (!ptr_skip && moved < TAP_MAX_TRAVEL && !over) launch();
            ptr_down = false;
        }

        if (mode == MODE_TILT) {
            // Gravity-locked paddle: it always sits at the real-world bottom of the
            // watch. Turn the watch like a wheel and the paddle stays put while the
            // rings rotate around it.
            // 1) Low-pass the gravity vector (~90 ms) to strip hand tremor and
            //    sensor noise before it becomes an angle.
            const float gx = in.tilt.ax * (tilt_invert ? -1 : 1), gy = in.tilt.ay;
            const float lp = 1.0f - std::exp(-dt / 0.09f);
            grav_x += (gx - grav_x) * lp;
            grav_y += (gy - grav_y) * lp;
            const float mag = std::sqrt(grav_x * grav_x + grav_y * grav_y);
            // Lying flat there is no "down" in the screen plane; hold position.
            if (mag > 0.25f) {
                // 2) Adaptive follow: lazy for tiny wobbles, snappy for real turns.
                //    ~2° off -> ~5/s (glides), ~30° off -> ~35/s (near-instant).
                const float err = wrap(std::atan2(grav_y, grav_x) - paddle);
                const float k = 4.0f + 60.0f * std::fabs(err);
                paddle = wrap(paddle + err * std::min(1.0f, k * dt));
            }
            has_target = false;
        }
    }

    void update(Engine &e, float dt)
    {
        for (auto &p : parts) {
            p.x += p.vx * dt;
            p.y += p.vy * dt;
            p.vx *= 0.96f;
            p.vy *= 0.96f;
            p.life -= dt;
        }
        parts.erase(std::remove_if(parts.begin(), parts.end(), [](const Part &p) { return p.life <= 0; }),
                    parts.end());
        for (auto &p : pops) {
            p.y -= 34 * dt;   // drifts up as it fades
            p.life -= dt * 1.6f;
        }
        pops.erase(std::remove_if(pops.begin(), pops.end(), [](const Pop &p) { return p.life <= 0; }), pops.end());
        if (flash_brick_.ring >= 0) {
            flash_brick_.time -= dt;
            if (flash_brick_.time <= 0) {
                full_redraw = true;
                flash_brick_.ring = -1;
            }
        }
        for (auto &r : rings_fx) r.life -= dt * 5.0f;
        rings_fx.erase(std::remove_if(rings_fx.begin(), rings_fx.end(), [](const Shock &r) { return r.life <= 0; }),
                       rings_fx.end());
        if (flash > 0) flash -= dt;
        if (mode_label_t > 0) mode_label_t -= dt;

        const float prev = paddle;
        handleInput(e, dt);
        if (over || menu.isOpen()) return;

        if (has_target) paddle += wrap(target - paddle) * std::min(1.0f, dt * 20);
        paddle = wrap(paddle);
        pv = wrap(paddle - prev) / std::max(dt, 1e-3f);

        if (wideT > 0) wideT -= dt;
        if (slowT > 0) slowT -= dt;
        hw += ((wideT > 0 ? 0.48f : 0.3f) - hw) * std::min(1.0f, dt * 8);

        for (auto &b : balls) {
            if (b.stuck) {
                const float rr = PADDLE_R - BALL_R - 2;
                b.x = C + std::cos(paddle) * rr;
                b.y = C + std::sin(paddle) * rr;
                continue;
            }
            const int steps = std::max(1, int(std::ceil(spd() * dt / 3)));
            for (int i = 0; i < steps && !b.dead; i++) {
                stepBall(b, dt / steps);
                const float m = std::max(1e-3f, std::hypot(b.vx, b.vy));
                b.vx = b.vx / m * spd();
                b.vy = b.vy / m * spd();
            }
        }
        balls.erase(std::remove_if(balls.begin(), balls.end(), [](const Ball &b) { return b.dead; }), balls.end());
        if (balls.empty()) {
            lives--;
            caps.clear();
            wideT = slowT = 0;
            combo = 0;
            if (lives <= 0) {
                over = true;
                sfx::gameOver();
                ESP_LOGI(TAG, "game over, score %d", score);
            } else {
                sfx::lifeLost();
                stickBall();
            }
        }

        for (auto &c : caps) {
            c.v += 150 * dt;
            c.r += c.v * dt;
            if (!c.done && c.r >= PADDLE_R - 8 && c.r <= PADDLE_R + PADDLE_T &&
                std::fabs(wrap(c.a - paddle)) <= hw + 0.06f) {
                c.done = true;
                sfx::powerUp();
                applyCap(c.type);
                burst(C + std::cos(c.a) * c.r, C + std::sin(c.a) * c.r, CAPS[c.type].color, 10);
            }
        }
        caps.erase(std::remove_if(caps.begin(), caps.end(), [](const Cap &c) { return c.done || c.r >= R + 12; }),
                   caps.end());

        bool cleared = true;
        for (auto &g : rings)
            for (auto h : g.bricks)
                if (h > 0) cleared = false;
        if (cleared) {
            sfx::levelClear();
            level++;
            speed = std::min(base_speed * (1 + 0.08f * (level - 1)), base_speed * 1.5f);
            buildRings();
            caps.clear();
            stickBall();
            full_redraw = true;
        }
    }

    // =================================================================== rendering
    bool loadAssets()
    {
        if (!canvas.init(SCALE)) return false;
        scene = static_cast<uint8_t *>(heap_caps_malloc(CW * CW, MALLOC_CAP_SPIRAM));
        if (!scene) return false;
        c_black = canvas.color(rgb(0, 0, 0));
        c_white = canvas.color(rgb(255, 255, 255));
        c_track = canvas.color(rgb(22, 22, 28));
        c_core = canvas.color(rgb(0x10, 0x13, 0x1a));
        c_core_edge = canvas.color(rgb(0x4a, 0x50, 0x60));
        c_shield = canvas.color(rgb(0x5a, 0xb0, 0xb4));
        c_star = canvas.color(rgb(40, 42, 56));
        c_star2 = canvas.color(rgb(70, 74, 96));
        c_pad = canvas.color(rgb(235, 238, 245));
        c_pad_flash = canvas.color(col(hex(0xffd23f)));
        c_pad_edge = canvas.color(rgb(120, 130, 160));
        c_ball_rim = canvas.color(rgb(170, 190, 220));
        c_life_off = canvas.color(rgb(60, 60, 60));
        c_cyan = canvas.color(colors::cyan);
        c_yellow = canvas.color(colors::yellow);
        c_panel = canvas.color(rgb(16, 18, 26));
        return true;
    }

    // Colours that fade (particles, pops, shockwaves) are quantised to four
    // brightness steps so they don't eat the palette.
    uint8_t fade(RGB c, float k)
    {
        const int q = std::max(1, std::min(4, int(k * 4 + 0.999f)));
        return canvas.color(col(c, q / 4.0f));
    }

    // The static scene at one canvas pixel: backdrop, brick rings and the core.
    inline uint8_t shadeScene(int x, int y) const
    {
        const int i = (2 * y) * W + 2 * x;
        const uint16_t r16 = Polar::radius16(i);
        if (r16 > uint16_t(R * 16)) return c_black;
        const uint16_t a = Polar::angle(i);

        if (r16 <= uint16_t(CORE_R * 16)) {
            return r16 >= uint16_t((CORE_R - 2.0f) * 16) ? c_core_edge : c_core;
        }
        if (r16 < uint16_t(FIELD_R * 16)) {
            for (const Ring &g : rings) {
                if (r16 < g.r0_16 || r16 > g.r1_16) continue;
                const uint32_t prod = uint32_t(uint16_t(a - g.off16)) * uint32_t(g.n);
                const int idx = prod >> 16;
                const uint16_t frac = prod & 0xFFFF;
                const int8_t hp = g.bricks[idx];
                if (hp <= 0 || frac < g.gap_frac || frac > 65535 - g.gap_frac) break;
                // Bevel: lit along the outer edge and leading side, dark along the
                // inner edge and trailing side, so each brick reads as a little tile.
                const uint16_t edge = g.gap_frac + g.outline_frac;
                const bool outer = r16 > g.r1_16 - 32, inner = r16 < g.r0_16 + 32;
                const bool lead = frac < edge, trail = frac > 65535 - edge;
                if (hp > 1) {
                    if (outer || lead) return g.outline;
                    if (inner || trail) return g.shade;
                    return g.full;
                }
                if (inner || trail) return g.dim_shade;
                return g.dim;
            }
        }
        // Backdrop: a faint scatter of stars in the empty space.
        const unsigned h = unsigned(x * 2654435761u) ^ unsigned(y * 40503u);
        if ((h >> 9) % 97 == 0) return ((h >> 3) & 3) ? c_star : c_star2;
        return c_black;
    }

    void buildScene()
    {
        for (int y = 0; y < CW; y++)
            for (int x = 0; x < CW; x++) scene[y * CW + x] = shadeScene(x, y);
    }

    // Paddle track, shield and paddle, painted over the scene every frame.
    void drawPaddle(Canvas &c)
    {
        const uint16_t pad_a16 = toA16(paddle), pad_hw16 = uint16_t(hw / TAU * 65536.0f);
        const float cr = PADDLE_R + PADDLE_T / 2, cr2 = (PADDLE_T / 2) * (PADDLE_T / 2);
        float cap_x[2], cap_y[2];
        for (int k = 0; k < 2; k++) {
            const float a = paddle + (k ? hw : -hw);
            cap_x[k] = C + std::cos(a) * cr;
            cap_y[k] = C + std::sin(a) * cr;
        }
        const uint8_t pad = flash > 0 ? c_pad_flash : c_pad;
        const int y0 = int((C - PADDLE_R - PADDLE_T - 2) / 2), y1 = int((C + PADDLE_R + PADDLE_T + 2) / 2);
        uint8_t *px = c.pixels();
        for (int y = std::max(0, y0); y <= std::min(CW - 1, y1); y++) {
            for (int x = 0; x < CW; x++) {
                const int i = (2 * y) * W + 2 * x;
                const uint16_t r16 = Polar::radius16(i);
                if (r16 < uint16_t((PADDLE_R - 1) * 16) || r16 > uint16_t((PADDLE_R + PADDLE_T + 1) * 16)) continue;
                uint8_t col = c_track;
                if (shield && r16 >= uint16_t((PADDLE_R + 4) * 16) && r16 <= uint16_t((PADDLE_R + 6) * 16)) col = c_shield;
                const int16_t d = int16_t(Polar::angle(i) - pad_a16);
                bool on = (d < 0 ? -d : d) <= pad_hw16 && r16 >= uint16_t(PADDLE_R * 16) &&
                          r16 <= uint16_t((PADDLE_R + PADDLE_T) * 16);
                if (!on) {
                    const float fx = 2 * x + 1.0f, fy = 2 * y + 1.0f;
                    for (int k = 0; k < 2 && !on; k++) {
                        const float ex = fx - cap_x[k], ey = fy - cap_y[k];
                        on = ex * ex + ey * ey <= cr2;
                    }
                }
                if (on) {
                    // Lit outer face, darker inner face.
                    col = r16 > uint16_t((PADDLE_R + PADDLE_T - 2.5f) * 16) ? c_pad_edge
                          : r16 < uint16_t((PADDLE_R + 2.5f) * 16)          ? c_white
                                                                             : pad;
                }
                px[y * CW + x] = col;
            }
        }
    }

    void disc(Canvas &c, float cx, float cy, float r, uint8_t col)
    {
        cx /= 2, cy /= 2, r /= 2;
        const int y0 = int(std::floor(cy - r)), y1 = int(std::ceil(cy + r));
        for (int y = y0; y <= y1; y++) {
            const float dy = y + 0.5f - cy;
            if (dy * dy > r * r) continue;
            const float half = std::sqrt(r * r - dy * dy);
            const int xa = int(std::ceil(cx - half - 0.5f)), xb = int(std::floor(cx + half - 0.5f));
            if (xb >= xa) c.fillRect(xa, y, xb - xa + 1, 1, col);
        }
    }

    // Text in screen units: scale 2 -> canvas 1, scale 4 -> canvas 2.
    void textCentered(Canvas &c, float cx, float cy, const char *s, uint8_t col, int scale)
    {
        c.textCentered(int(cx / 2), int(cy / 2), s, col, std::max(1, scale / 2), true);
    }

    void drawCore(Canvas &c)
    {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", score);
        textCentered(c, C, C - 7, buf, c_white, std::strlen(buf) > 3 ? 2 : 3);
        for (int i = 0; i < 3; i++) disc(c, C - 11 + i * 11, C + 19, 3.4f, i < lives ? c_white : c_life_off);
    }

    void drawMenu(Gfx &g)
    {
        namespace ui = console::ui;
        const ui::PauseMenu::Row rows[] = {{"CONTROL", MODE_NAMES[mode]},
                                           {"TILT DIR", tilt_invert ? "MIRROR" : "NORMAL"},
                                           {"SPEED", SPEED_NAMES[speed_idx]},
                                           menu.soundRow()};
        menu.draw(g, rows, 4);
    }

    void draw(Engine &e, Gfx &g)
    {
        if (menu.isOpen()) {
            drawMenu(g);
            return;
        }
        Canvas &c = canvas;
        if (full_redraw) {
            buildScene();
            full_redraw = false;
        }
        std::memcpy(c.pixels(), scene, CW * CW);
        drawPaddle(c);
        if (!over) drawCore(c);

        if (flash_brick_.ring >= 0 && flash_brick_.time > 0)
            flashBrick(c, rings[flash_brick_.ring], flash_brick_.idx, c_white);

        // ---- sprites
        for (const Cap &cp : caps) {
            const float x = C + std::cos(cp.a) * cp.r, y = C + std::sin(cp.a) * cp.r;
            disc(c, x, y, 12, canvas.color(col(CAPS[cp.type].color)));
            disc(c, x - 3, y - 3, 4, canvas.color(col(lerp(CAPS[cp.type].color, WHITE, 0.5f))));
            const char s[2] = {CAPS[cp.type].letter, 0};
            textCentered(c, x, y, s, c_black, 2);
        }
        for (const Ball &b : balls) {
            disc(c, b.x, b.y, BALL_R, c_ball_rim);
            disc(c, b.x, b.y, BALL_R - 1.6f, c_white);
        }
        for (const Part &p : parts) c.fillRect(int(p.x / 2) - 1, int(p.y / 2) - 1, 2, 2, fade(p.color, clampf(p.life * 2, 0, 1)));

        // Impact rings: a quick shockwave where the ball struck.
        for (const Shock &r : rings_fx) {
            const int rad = int((6 + (1.0f - r.life) * 26) / 2);
            const uint8_t col = fade(r.color, clampf(r.life, 0, 1));
            for (int k = 0; k < 24; k++) {
                const float a = k * TAU / 24;
                c.pixel(int(r.x / 2 + std::cos(a) * rad), int(r.y / 2 + std::sin(a) * rad), col);
            }
        }

        // Score pops floating up from broken bricks.
        for (const Pop &p : pops) {
            char buf[12];
            snprintf(buf, sizeof(buf), "+%d", p.score);
            textCentered(c, p.x, p.y, buf, fade(p.color, clampf(p.life, 0, 1)), 2);
        }

        const bool stuck = std::any_of(balls.begin(), balls.end(), [](const Ball &b) { return b.stuck; });
        // Tooltips live in the empty band between the brick field and the paddle
        // track: two lines at the top, two at the bottom.
        char mode_line[32];
        snprintf(mode_line, sizeof(mode_line), "CONTROL: %s", MODE_NAMES[mode]);
        if (!over && stuck) {
            char top[32];
            if (level > 1) snprintf(top, sizeof(top), "LEVEL %d", level);
            else snprintf(top, sizeof(top), "%s", mode_line);
            textCentered(c, C, 58, top, c_cyan, 2);
            textCentered(c, C, 84, level > 1 ? mode_line : "PWR: CHANGE", level > 1 ? c_cyan : c_yellow, 2);
            textCentered(c, C, 382, "TAP TO LAUNCH", c_white, 2);
            textCentered(c, C, 408, "SWIPE < MENU", c_yellow, 2);
        } else if (mode_label_t > 0 && !over) {
            textCentered(c, C, 70, mode_line, c_cyan, 2);
        }
        if (over) {
            char buf[32];
            snprintf(buf, sizeof(buf), "SCORE %d", score);
            c.fillRect(CW / 2 - 60, CW / 2 - 34, 120, 68, c_panel);
            c.rect(CW / 2 - 60, CW / 2 - 34, 120, 68, c_core_edge);
            textCentered(c, C, C - 40, "GAME OVER", c_white, 4);
            textCentered(c, C, C + 2, buf, c_yellow, 3);
            textCentered(c, C, C + 42, "TAP TO PLAY", c_white, 2);
        }
        c.present(e.presenter());
    }
};

Breakout::Breakout() : s_(new State) {}
Breakout::~Breakout() { delete s_; }

void Breakout::begin(Engine &e)
{
    if (!Polar::init()) ESP_LOGE(TAG, "polar tables alloc failed");
    if (!s_->loadAssets()) ESP_LOGE(TAG, "no memory for the scene");
    s_->loadSettings();
    s_->newGame();
    ESP_LOGI(TAG, "ready: control=%s, tap/BOOT to launch, PWR cycles control, swipe left for settings",
             MODE_NAMES[s_->mode]);
}

void Breakout::enter(Engine &e)
{
    // Coming back from the home screen: repaint, and pause a game that was mid-rally.
    s_->full_redraw = true;
    const bool in_play = std::any_of(s_->balls.begin(), s_->balls.end(), [](const Ball &b) { return !b.stuck; });
    if (in_play && !s_->over) s_->openMenu();
}

bool Breakout::keepAwake() const
{
    if (s_->over || s_->menu.isOpen()) return false;
    return std::any_of(s_->balls.begin(), s_->balls.end(), [](const Ball &b) { return !b.stuck; });
}

void Breakout::update(Engine &e, float dt)
{
    s_->update(e, std::min(dt, 1.0f / 30));
}

void Breakout::draw(Engine &e, Gfx &g)
{
    if (s_->scene) s_->draw(e, g);
}

}  // namespace games
