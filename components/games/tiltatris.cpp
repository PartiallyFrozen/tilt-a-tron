#include "games/tiltatris.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "audio/audio.h"
#include "console/ui.h"
#include "engine/canvas.h"
#include "engine/gestures.h"
#include "engine/polar.h"
#include "esp_log.h"
#include "esp_random.h"
#include "nvs.h"

using namespace wc;

namespace games {

namespace {

const char *TAG = "tiltatris";

constexpr float PI = 3.14159265f, TAU = 2 * PI;
constexpr int SCALE = 2, CW = (Gfx::W + SCALE - 1) / SCALE;
constexpr int W = Gfx::W;

// The well: RINGS layers of N wedge cells between the core and the rim (screen px).
constexpr int N = 24, RINGS = 10;
constexpr float CORE_R = 46.0f, RING_H = 16.5f, RIM_R = CORE_R + RINGS * RING_H;   // rim at 211
constexpr int SPAWN_RING = RINGS + 1;   // pieces start just outside the rim, like Tetris spawns above the top
constexpr float LOCK_DELAY = 0.45f;     // seconds a landed piece can still be turned
constexpr uint32_t CELL16 = 65536u / N;

// Tetrominoes as (row, col) cells; row 0 is the outermost. Rotation is the usual
// 90-degree turn about the piece's own grid.
struct Shape {
    int8_t cells[4][2];
    int size;   // bounding grid for rotation
};
constexpr Shape SHAPES[7] = {
    {{{1, 0}, {1, 1}, {1, 2}, {1, 3}}, 4},   // I
    {{{0, 0}, {0, 1}, {1, 0}, {1, 1}}, 2},   // O
    {{{0, 1}, {1, 0}, {1, 1}, {1, 2}}, 3},   // T
    {{{0, 2}, {1, 0}, {1, 1}, {1, 2}}, 3},   // L
    {{{0, 0}, {1, 0}, {1, 1}, {1, 2}}, 3},   // J
    {{{0, 1}, {0, 2}, {1, 0}, {1, 1}}, 3},   // S
    {{{0, 0}, {0, 1}, {1, 1}, {1, 2}}, 3},   // Z
};
constexpr uint8_t PIECE_RGB[7][3] = {
    {80, 220, 240}, {255, 215, 60}, {190, 90, 240}, {255, 150, 40}, {70, 130, 255}, {90, 230, 110}, {255, 80, 90},
};

uint32_t rnd(uint32_t n) { return esp_random() % n; }
float clampf(float v, float a, float b) { return std::max(a, std::min(b, v)); }
int wrapCol(int c) { return ((c % N) + N) % N; }

enum Phase { READY, PLAYING, CLEARING, GAME_OVER };

namespace sfx {
using wc::audio::Tone;
using wc::audio::Wave;
void turn() { wc::audio::play({.f0 = 700, .f1 = 900, .ms = 30, .wave = Wave::Square, .volume = 0.3f}); }
void lock() { wc::audio::play({.f0 = 180, .f1 = 90, .ms = 60, .wave = Wave::Triangle, .volume = 0.6f}); }
void drop() { wc::audio::play({.f0 = 400, .f1 = 120, .ms = 90, .wave = Wave::Triangle, .volume = 0.6f}); }
void clear(int n)
{
    const Tone t[] = {{.f0 = 660, .ms = 70, .wave = Wave::Square, .volume = 0.6f, .delay_ms = 0},
                      {.f0 = 880, .ms = 70, .wave = Wave::Square, .volume = 0.6f, .delay_ms = 70},
                      {.f0 = 1100, .ms = 70, .wave = Wave::Square, .volume = 0.6f, .delay_ms = 140},
                      {.f0 = 1320, .ms = 200, .wave = Wave::Square, .volume = 0.7f, .delay_ms = 210}};
    wc::audio::play(t, std::min(4, 1 + n));
}
void levelUp()
{
    const Tone t[] = {{.f0 = 523, .f1 = 1046, .ms = 160, .wave = Wave::Triangle, .volume = 0.6f},
                      {.f0 = 784, .f1 = 1568, .ms = 220, .wave = Wave::Triangle, .volume = 0.6f, .delay_ms = 120}};
    wc::audio::play(t, 2);
}
void start() { wc::audio::play({.f0 = 500, .f1 = 900, .ms = 100, .wave = Wave::Triangle, .volume = 0.6f}); }
void gameOver()
{
    const Tone t[] = {{.f0 = 392, .ms = 160, .wave = Wave::Square, .volume = 0.6f, .delay_ms = 0},
                      {.f0 = 330, .ms = 160, .wave = Wave::Square, .volume = 0.6f, .delay_ms = 160},
                      {.f0 = 262, .ms = 320, .wave = Wave::Square, .volume = 0.6f, .delay_ms = 320}};
    wc::audio::play(t, 3);
}
}  // namespace sfx

}  // namespace

struct Tiltatris::State {
    // ---- game
    Phase phase = READY;
    float phase_t = 0;
    uint8_t board[RINGS][N] = {};   // 0 empty, else piece index + 1
    int score = 0, best = 0, rings_cleared = 0, level = 1;
    bool got_best = false;
    int piece = 0, next = 0, rot = 0;
    int p_ring = 0, p_col = 0;      // the piece's grid origin: ring of its row 0, column of its col 0
    float fall_t = 0, lock_t = 0;
    bool soft = false;
    bool clearing[RINGS] = {};
    // Some levels flip gravity: pieces rise from the core and the pile builds
    // against the rim. The board is kept floor-first either way; only the
    // drawing mirrors the rings.
    bool outward = false;
    float flip_t = 0;

    // ---- tilt: the pile is locked to the real world
    float grav_x = 0, grav_y = 1;
    float roll = 0;                 // board rotation on screen (radians)
    uint16_t off16 = 0;
    // The gyro carries the rotation when the watch lies flat (gravity can't tell);
    // gravity corrects the gyro's drift whenever the watch is upright enough. The
    // gyro's sign in screen space is learned from gravity the first time you turn it.
    float gyro_sign = 1, sign_score = 0, last_target = 0;
    bool had_target = false;
    uint8_t piece_mask[RINGS + 4][N];   // the falling piece in screen columns (north = up)

    // ---- input
    Gestures ges;
    int64_t touch_t0 = 0;
    bool touch_was = false;
    bool menu = false, menu_dirty = false;

    // ---- drawing
    Canvas canvas;
    uint8_t merged[RINGS + 4][N];   // board + falling piece + ghost, for the per-pixel shader
    uint8_t c_bg = 0, c_grid = 0, c_core = 0, c_core_edge = 0, c_rim = 0, c_white = 0, c_black = 0, c_dim = 0;
    uint8_t c_gap = 0, c_ghost = 0, c_panel = 0, c_box = 0, c_yellow = 0, c_cyan = 0, c_danger = 0, c_go = 0;
    uint8_t c_full[7], c_lit[7], c_dark[7];

    // ------------------------------------------------------------------ persistence
    void load()
    {
        nvs_handle_t h;
        if (nvs_open("tiltatris", NVS_READONLY, &h) != ESP_OK) return;
        int32_t v;
        if (nvs_get_i32(h, "best", &v) == ESP_OK) best = v;
        nvs_close(h);
    }
    void save()
    {
        nvs_handle_t h;
        if (nvs_open("tiltatris", NVS_READWRITE, &h) != ESP_OK) return;
        nvs_set_i32(h, "best", best);
        nvs_commit(h);
        nvs_close(h);
    }

    bool loadAssets()
    {
        if (!canvas.init(SCALE)) return false;
        c_black = canvas.color(rgb(0, 0, 0));
        c_bg = canvas.color(rgb(12, 12, 20));
        c_grid = canvas.color(rgb(26, 26, 40));
        c_core = canvas.color(rgb(16, 19, 28));
        c_core_edge = canvas.color(rgb(74, 80, 100));
        c_rim = canvas.color(rgb(90, 96, 120));
        c_white = canvas.color(rgb(255, 255, 255));
        c_dim = canvas.color(rgb(150, 150, 160));
        c_gap = canvas.color(rgb(8, 8, 12));
        c_ghost = canvas.color(rgb(70, 74, 92));
        c_panel = canvas.color(rgb(16, 18, 26));
        c_box = canvas.color(rgb(90, 90, 100));
        c_yellow = canvas.color(colors::yellow);
        c_cyan = canvas.color(colors::cyan);
        c_danger = canvas.color(colors::red);
        c_go = canvas.color(rgb(40, 200, 110));
        for (int i = 0; i < 7; i++) {
            const uint8_t r = PIECE_RGB[i][0], g = PIECE_RGB[i][1], b = PIECE_RGB[i][2];
            c_full[i] = canvas.color(rgb(r, g, b));
            c_lit[i] = canvas.color(rgb(uint8_t(r + (255 - r) * 0.55f), uint8_t(g + (255 - g) * 0.55f), uint8_t(b + (255 - b) * 0.55f)));
            c_dark[i] = canvas.color(rgb(uint8_t(r * 0.5f), uint8_t(g * 0.5f), uint8_t(b * 0.5f)));
        }
        return true;
    }

    // ------------------------------------------------------------------ pieces
    // Cell (ring, col) of the current piece for rotation `r` at origin (ring0, col0).
    void pieceCells(int shape, int r, int ring0, int col0, int out[4][2]) const
    {
        const Shape &s = SHAPES[shape];
        for (int i = 0; i < 4; i++) {
            int row = s.cells[i][0], col = s.cells[i][1];
            for (int k = 0; k < r; k++) {
                const int nr = col, nc = s.size - 1 - row;
                row = nr, col = nc;
            }
            out[i][0] = ring0 - row;        // rows go inward
            out[i][1] = wrapCol(col0 + col);
        }
    }

    bool fits(int shape, int r, int ring0, int col0) const
    {
        int c[4][2];
        pieceCells(shape, r, ring0, col0, c);
        for (int i = 0; i < 4; i++) {
            if (c[i][0] < 0) return false;
            if (c[i][0] >= RINGS) continue;   // above the rim is fine while spawning
            if (board[c[i][0]][c[i][1]]) return false;
        }
        return true;
    }

    // Which column the top of the screen points at in board space.
    int topColumn() const
    {
        const float a = -PI / 2 - roll;
        return wrapCol(int(std::floor(a / TAU * N)) - SHAPES[piece].size / 2);
    }

    void spawn()
    {
        piece = next;
        next = int(rnd(7));
        rot = 0;
        p_ring = SPAWN_RING;
        p_col = topColumn();
        fall_t = 0;
        lock_t = 0;
        if (!fits(piece, rot, p_ring, p_col)) {
            phase = GAME_OVER;
            phase_t = 0;
            if (got_best) save();
            sfx::gameOver();
        }
    }

    void newGame()
    {
        std::memset(board, 0, sizeof(board));
        std::memset(clearing, 0, sizeof(clearing));
        score = 0;
        rings_cleared = 0;
        level = 1;
        outward = false;
        flip_t = 0;
        got_best = false;
        next = int(rnd(7));
        phase = READY;
        phase_t = 0;
    }

    float fallInterval() const { return std::max(0.12f, 0.9f * std::pow(0.82f, float(level - 1))); }

    void lockPiece()
    {
        int c[4][2];
        pieceCells(piece, rot, p_ring, p_col, c);
        bool over = false;
        for (int i = 0; i < 4; i++) {
            if (c[i][0] >= RINGS) over = true;
            else board[c[i][0]][c[i][1]] = uint8_t(piece + 1);
        }
        if (over) {
            phase = GAME_OVER;
            phase_t = 0;
            if (got_best) save();
            sfx::gameOver();
            return;
        }
        // Full rings clear.
        int n = 0;
        for (int r = 0; r < RINGS; r++) {
            bool full = true;
            for (int k = 0; k < N && full; k++) full = board[r][k] != 0;
            clearing[r] = full;
            n += full;
        }
        if (n) {
            static const int kPoints[5] = {0, 100, 300, 500, 800};
            score += kPoints[std::min(n, 4)] * level;
            rings_cleared += n;
            phase = CLEARING;
            phase_t = 0;
            sfx::clear(n);
        } else {
            sfx::lock();
            spawn();
        }
        if (score > best) {
            got_best = true;
            best = score;
        }
    }

    void finishClear()
    {
        // Drop everything above a cleared ring inward by one.
        int dst = 0;
        for (int r = 0; r < RINGS; r++) {
            if (clearing[r]) continue;
            if (dst != r) std::memcpy(board[dst], board[r], N);
            dst++;
        }
        for (int r = dst; r < RINGS; r++) std::memset(board[r], 0, N);
        std::memset(clearing, 0, sizeof(clearing));
        const int new_level = 1 + rings_cleared / 8;
        if (new_level > level) {
            level = new_level;
            sfx::levelUp();
            if (rnd(2)) {   // half the levels turn the well inside out
                outward = !outward;
                flip_t = 1.5f;
            }
        }
        spawn();
    }

    int ghostRing() const
    {
        int r = p_ring;
        while (r > 0 && fits(piece, rot, r - 1, p_col)) r--;
        return r;
    }

    // ------------------------------------------------------------------ update
    static float wrapAngle(float a)
    {
        while (a > PI) a -= TAU;
        while (a < -PI) a += TAU;
        return a;
    }

    void updateTilt(const InputState &in, float dt)
    {
        // Gyro about the screen's normal: the world turns the other way on screen.
        const float gz = in.tilt.gz * (PI / 180.0f) * dt;
        roll = wrapAngle(roll - gyro_sign * gz);

        const float k = std::min(1.0f, dt / 0.10f);
        grav_x += (in.tilt.ax - grav_x) * k;
        grav_y += (in.tilt.ay - grav_y) * k;
        const float upright = grav_x * grav_x + grav_y * grav_y;   // 1 = on edge, 0 = flat
        if (upright > 0.16f) {
            // "Down" on the screen is where gravity points; the pile keeps that as its down.
            const float target = std::atan2(grav_y, grav_x) - PI / 2;
            if (had_target && std::fabs(gz) > 0.004f) {
                // Gravity and gyro should agree on the direction of a turn.
                sign_score += wrapAngle(target - last_target) * (-gyro_sign * gz) * 400;
                if (sign_score < -1.0f) {
                    gyro_sign = -gyro_sign;
                    sign_score = 0;
                }
                sign_score = std::min(sign_score, 3.0f);
            }
            last_target = target;
            had_target = true;
            roll = wrapAngle(roll + wrapAngle(target - roll) * std::min(1.0f, dt / 0.25f));
        } else {
            had_target = false;
        }
        off16 = uint16_t(int(std::floor(roll / TAU * 65536.0f)) & 0xFFFF);
    }

    void play(Engine &e, const InputState &in, float dt)
    {
        // Turning the watch moves the pile under the piece: the piece follows the
        // top of the screen one column at a time, as far as the pile allows.
        const int want = topColumn();
        int guard = N;
        while (p_col != want && guard-- > 0) {
            const int step = wrapCol(want - p_col) <= N / 2 ? 1 : -1;
            if (!fits(piece, rot, p_ring, wrapCol(p_col + step))) break;
            p_col = wrapCol(p_col + step);
        }

        // Tap = rotate (with a nudge either way if it doesn't fit), hold = soft drop, PWR = hard drop.
        if (in.touch.pressed) {
            touch_t0 = e.nowUs();
            touch_was = true;
        }
        const bool held = in.touch.down && e.nowUs() - touch_t0 > 250000;
        soft = held;
        if (ges.tap && e.nowUs() - touch_t0 < 250000) {
            const int nr = (rot + 1) % 4;
            for (int nudge : {0, 1, -1, 2, -2}) {
                if (fits(piece, nr, p_ring, wrapCol(p_col + nudge))) {
                    rot = nr;
                    p_col = wrapCol(p_col + nudge);
                    sfx::turn();
                    break;
                }
            }
        }
        if (in.pressed & BTN_B) {
            const int g = ghostRing();
            score += (p_ring - g) * 2;
            p_ring = g;
            sfx::drop();
            lockPiece();
            return;
        }

        fall_t += dt * (soft ? 10.0f : 1.0f);
        const float interval = fallInterval();
        while (fall_t >= interval) {
            fall_t -= interval;
            if (fits(piece, rot, p_ring - 1, p_col)) {
                p_ring--;
                lock_t = 0;
                if (soft) score += 1;
            } else {
                break;
            }
        }
        if (!fits(piece, rot, p_ring - 1, p_col)) {
            lock_t += dt;
            if (lock_t >= LOCK_DELAY) lockPiece();
        } else {
            lock_t = 0;
        }
    }

    void update(Engine &e, float dt)
    {
        const InputState &in = e.input();
        ges.update(in.touch);
        phase_t += dt;
        updateTilt(in, dt);

        if (menu) {
            if (ges.tap) menuTap(e, ges.x, ges.y);
            if (ges.swipe_right || (in.clicked & BTN_B)) menu = false;
            return;
        }
        if (ges.swipe_left && phase != GAME_OVER) {
            menu = true;
            menu_dirty = true;
            return;
        }

        switch (phase) {
        case READY:
            if (ges.tap) {
                phase = PLAYING;
                sfx::start();
                spawn();
            }
            break;
        case PLAYING: play(e, in, dt); break;
        case CLEARING:
            if (phase_t > 0.35f) {
                phase = PLAYING;
                finishClear();
            }
            break;
        case GAME_OVER:
            if (ges.tap && phase_t > 0.8f) newGame();
            break;
        }
    }

    // ------------------------------------------------------------------ menu
    void menuTap(Engine &e, int x, int y)
    {
        namespace ui = console::ui;
        if (ui::rowRect(0).hit(x, y)) {
            wc::audio::setVolume(wc::audio::volume() == 0 ? 2 : 0);
            if (wc::audio::volume() > 0) sfx::start();
            menu_dirty = true;
        } else if (ui::rowRect(1).hit(x, y)) {
            newGame();
            menu = false;
        } else if (ui::buttonRect(0, 2).hit(x, y)) {
            menu = false;
        } else if (ui::buttonRect(1, 2).hit(x, y)) {
            menu = false;
            e.goHome();
        }
    }

    void drawMenu(Gfx &g)
    {
        namespace ui = console::ui;
        ui::clearScreen(g);
        ui::title(g, "PAUSED");
        ui::row(g, 0, "SOUND", wc::audio::volume() ? "ON" : "OFF", wc::audio::volume() ? ui::GO : ui::DIM);
        ui::row(g, 1, "NEW GAME", "GO", ui::ACCENT);
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", best);
        ui::row(g, 2, "BEST", buf, ui::LABEL);
        ui::button(g, ui::buttonRect(0, 2), "RESUME");
        ui::outlineButton(g, ui::buttonRect(1, 2), "HOME");
    }

    // ------------------------------------------------------------------ drawing
    void buildMerged()
    {
        std::memset(merged, 0, sizeof(merged));
        for (int r = 0; r < RINGS; r++)
            for (int k = 0; k < N; k++) merged[r][k] = clearing[r] && board[r][k] ? 8 : board[r][k];
        if (phase == PLAYING) {
            int c[4][2];
            const int g = ghostRing();
            pieceCells(piece, rot, g, p_col, c);
            for (int i = 0; i < 4; i++)
                if (c[i][0] >= 0 && c[i][0] < RINGS + 4 && !merged[c[i][0]][c[i][1]]) merged[c[i][0]][c[i][1]] = 9;
        }
        // The falling piece never turns with the pile: it sits at the top of the
        // screen, in screen columns, and snaps into the pile's grid when it lands.
        std::memset(piece_mask, 0, sizeof(piece_mask));
        if (phase == PLAYING) {
            int c[4][2];
            pieceCells(piece, rot, p_ring, p_col, c);
            const int north = N * 3 / 4;   // screen column straight up (angles run clockwise from +x)
            for (int i = 0; i < 4; i++) {
                const int rel = wrapCol(c[i][1] - p_col);   // 0..size-1 within the piece
                const int sc = wrapCol(north - SHAPES[piece].size / 2 + rel);
                if (c[i][0] >= 0 && c[i][0] < RINGS + 4) piece_mask[c[i][0]][sc] = uint8_t(piece + 1);
            }
        }
    }

    void drawWell(Canvas &cv)
    {
        uint8_t *px = cv.pixels();
        const bool blink = std::fmod(phase_t, 0.12f) < 0.06f;
        for (int y = 0; y < CW; y++) {
            for (int x = 0; x < CW; x++) {
                const int i = (2 * y) * W + 2 * x;
                const float r = Polar::radius16(i) / 16.0f;
                uint8_t c = c_bg;
                if (r < CORE_R) {
                    c = r > CORE_R - 2 ? c_core_edge : c_core;
                } else {
                    const int pring = std::min(RINGS + 3, int((r - CORE_R) / RING_H));
                    const int ring = outward ? RINGS - 1 - pring : pring;   // floor-first index
                    const float rf = r - CORE_R - pring * RING_H;
                    const uint32_t rel = uint32_t(uint16_t(Polar::angle(i) - off16)) * N;
                    const int col = rel >> 16;
                    const uint32_t frac = rel & 0xFFFF;
                    // One screen pixel of gap between wedges, scaled to this radius.
                    const uint32_t gap = uint32_t(65536.0f / (TAU * r / N));
                    uint8_t v = ring >= 0 ? merged[ring][col] : 0;
                    if (pring >= RINGS && v >= 8) v = 0;   // only the falling piece shows outside the rim
                    if (r >= RIM_R && v == 0) {
                        c = r < RIM_R + 1.5f ? c_rim : c_bg;
                    } else if (v == 0) {
                        c = (rf < 1.0f || frac < gap / 2) ? c_grid : c_bg;
                    } else if (v == 8) {
                        c = blink ? c_white : c_dim;
                    } else if (v == 9) {
                        // Ghost: where the piece will land, in a dark tint of its colour.
                        c = (frac < gap || frac > 65535 - gap || rf < 1.0f) ? c_gap : c_dark[piece];
                    } else {
                        const int p = v - 1;
                        if (frac < gap || frac > 65535 - gap || rf < 1.0f) c = c_gap;
                        else if (rf > RING_H - 2.0f || frac < gap * 3) c = c_lit[p];
                        else if (rf < 3.0f || frac > 65535 - gap * 3) c = c_dark[p];
                        else c = c_full[p];
                    }
                    // The falling piece, in screen columns, on top of everything.
                    const uint32_t srel = uint32_t(Polar::angle(i)) * N;
                    const uint8_t pv = (ring >= 0 && ring < RINGS + 4) ? piece_mask[ring][srel >> 16] : 0;
                    if (pv) {
                        const uint32_t sfrac = srel & 0xFFFF;
                        const int p = pv - 1;
                        if (sfrac < gap || sfrac > 65535 - gap || rf < 1.0f) c = c_gap;
                        else if (rf > RING_H - 2.0f || sfrac < gap * 3) c = c_lit[p];
                        else if (rf < 3.0f || sfrac > 65535 - gap * 3) c = c_dark[p];
                        else c = c_full[p];
                    }
                }
                px[y * CW + x] = c;
            }
        }
    }

    void drawNext(Canvas &cv, int cx, int cy)
    {
        const Shape &s = SHAPES[next];
        const int cell = 4;
        const int ox = cx - s.size * cell / 2, oy = cy - cell;
        for (int i = 0; i < 4; i++) {
            const int x = ox + s.cells[i][1] * cell, y = oy + s.cells[i][0] * cell;
            cv.fillRect(x, y, cell - 1, cell - 1, c_full[next]);
        }
    }

    void banner(Canvas &c, const char *top, const char *mid, const char *bottom, uint8_t col)
    {
        const int h = bottom ? 34 : mid ? 26 : 16;
        c.fillRect(30, 100, CW - 60, h, c_panel);
        c.rect(30, 100, CW - 60, h, c_box);
        c.textCentered(CW / 2, 108, top, col, 1, true);
        if (mid) c.textCentered(CW / 2, 118, mid, c_dim, 1, false);
        if (bottom) c.textCentered(CW / 2, 127, bottom, c_dim, 1, false);
    }

    void draw(Engine &e, Gfx &g)
    {
        if (menu) {
            if (menu_dirty) {
                drawMenu(g);
                menu_dirty = false;
            }
            return;
        }
        Canvas &c = canvas;
        buildMerged();
        drawWell(c);

        // The core: score, level and the next piece.
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", score);
        c.textCentered(CW / 2, CW / 2 - 11, buf, c_white, 1, true);
        snprintf(buf, sizeof(buf), "L%d", level);
        c.textCentered(CW / 2, CW / 2, buf, c_cyan, 1, false);
        if (phase == PLAYING || phase == CLEARING) drawNext(c, CW / 2, CW / 2 + 13);

        switch (phase) {
        case READY: banner(c, "TAP TO DROP", "TURN WATCH: SPIN PILE", "TAP: TURN  HOLD: DROP", c_white); break;
        case PLAYING:
            if (flip_t > 0) {
                flip_t -= 1.0f / 60;
                banner(c, "GRAVITY FLIP!", outward ? "PIECES RISE FROM THE CORE" : "PIECES FALL FROM THE RIM", nullptr,
                       c_yellow);
            }
            break;
        case GAME_OVER:
            if (phase_t > 0.5f) {
                snprintf(buf, sizeof(buf), got_best ? "NEW BEST %d!" : "SCORE %d", score);
                banner(c, "GAME OVER", buf, "TAP TO PLAY", got_best ? c_yellow : c_danger);
            }
            break;
        default: break;
        }
        c.present(e.presenter());
    }
};

Tiltatris::Tiltatris() : s_(new State) {}
Tiltatris::~Tiltatris() { delete s_; }

void Tiltatris::begin(Engine &e)
{
    if (!Polar::init()) ESP_LOGE(TAG, "polar tables alloc failed");
    if (!s_->loadAssets()) ESP_LOGE(TAG, "no memory for the canvas");
    s_->load();
    s_->newGame();
    ESP_LOGI(TAG, "ready, best %d", s_->best);
}

void Tiltatris::enter(Engine &e)
{
    if (s_->phase == PLAYING) {
        s_->menu = true;
        s_->menu_dirty = true;
    }
    s_->menu_dirty = s_->menu;
}

bool Tiltatris::keepAwake() const { return !s_->menu && (s_->phase == PLAYING || s_->phase == CLEARING); }

void Tiltatris::update(Engine &e, float dt) { s_->update(e, std::min(dt, 1.0f / 30)); }

void Tiltatris::draw(Engine &e, Gfx &g)
{
    if (!s_->canvas.pixels()) return;
    s_->draw(e, g);
}

}  // namespace games
