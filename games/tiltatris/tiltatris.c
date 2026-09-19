// TILT-A-TRIS - Tetris bent into a circle.
//
// The well is a ring of wedge cells around a core. Pieces fall inward from the rim, and
// you aim them by turning the watch: the pile is fixed to the screen, so real-world "up"
// slides around the rim as you rotate it. Fill a whole ring and it clears. Some levels
// turn the well inside out and pieces rise from the core instead.
//
//   turn the watch - aim
//   tap            - rotate the piece
//   hold           - soft drop
//   PWR            - hard drop
//   swipe left     - pause menu
//
// Written against tat_api.h alone: it includes nothing else from the console and calls
// nothing by name.
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "tat/tat_api.h"

static const tat_api_t *T;

// ---------------------------------------------------------------- the well

#define PI 3.14159265f
#define TAU (2 * PI)
#define SCALE 2
#define CW ((TAT_SCREEN + SCALE - 1) / SCALE)   /* 233 */

// RINGS layers of N wedge cells between the core and the rim, in screen pixels.
#define N 24
#define RINGS 10
#define CORE_R 46.0f
#define RING_H 16.5f
#define RIM_R (CORE_R + RINGS * RING_H)   /* 211 */
#define SPAWN_RING (RINGS + 1)            /* pieces start outside the rim, like Tetris spawns above the top */
#define LOCK_DELAY 0.45f                  /* seconds a landed piece can still be turned */

// Tetrominoes as (row, col) cells; row 0 is the outermost. Rotation is the usual
// 90-degree turn about the piece's own grid.
typedef struct {
    int8_t cells[4][2];
    int size;   /* bounding grid for rotation */
} Shape;

static const Shape SHAPES[7] = {
    {{{1, 0}, {1, 1}, {1, 2}, {1, 3}}, 4},   /* I */
    {{{0, 0}, {0, 1}, {1, 0}, {1, 1}}, 2},   /* O */
    {{{0, 1}, {1, 0}, {1, 1}, {1, 2}}, 3},   /* T */
    {{{0, 2}, {1, 0}, {1, 1}, {1, 2}}, 3},   /* L */
    {{{0, 0}, {1, 0}, {1, 1}, {1, 2}}, 3},   /* J */
    {{{0, 1}, {0, 2}, {1, 0}, {1, 1}}, 3},   /* S */
    {{{0, 0}, {0, 1}, {1, 1}, {1, 2}}, 3},   /* Z */
};

static const uint8_t PIECE_RGB[7][3] = {
    {80, 220, 240}, {255, 215, 60}, {190, 90, 240}, {255, 150, 40},
    {70, 130, 255}, {90, 230, 110}, {255, 80, 90},
};

typedef enum { READY, PLAYING, CLEARING, GAME_OVER } Phase;

static struct {
    tat_canvas_t *cv;
    const uint16_t *pol_a, *pol_r;   /* the screen's polar tables, fetched once */

    /* game */
    Phase phase;
    float phase_t;
    uint8_t board[RINGS][N];   /* 0 empty, else piece index + 1 */
    int score, best, rings_cleared, level;
    bool got_best;
    int piece, next, rot;
    int p_ring, p_col;         /* the piece's grid origin */
    float fall_t, lock_t;
    bool soft;
    bool clearing[RINGS];
    // Some levels flip gravity: pieces rise from the core and the pile builds against the
    // rim. The board is kept floor-first either way; only the drawing mirrors the rings.
    bool outward;
    float flip_t;

    // tilt. The pile is part of the watch: it turns with it. The falling piece hangs from
    // the real world's "up", so turning the watch slides the piece around the rim relative
    // to the pile. `up` is the screen angle of real-world up.
    float grav_x, grav_y;
    float up;
    // The gyro carries the rotation when the watch lies flat (gravity can't tell); gravity
    // corrects the gyro's drift whenever the watch is upright enough. The gyro's sign in
    // screen space is learned from gravity the first time you turn it.
    float gyro_sign, sign_score, last_target;
    bool had_target;

    /* input */
    int64_t touch_t0;

    /* drawing */
    uint8_t merged[RINGS + 4][N];   /* board + falling piece + ghost, for the shader */
    uint8_t c_bg, c_grid, c_core, c_core_edge, c_rim, c_white, c_black, c_dim;
    uint8_t c_gap, c_ghost, c_panel, c_box, c_yellow, c_cyan, c_danger, c_go;
    uint8_t c_full[7], c_lit[7], c_dark[7];
} g;

// ---------------------------------------------------------------- odds and ends

static uint32_t rnd(uint32_t n) { return T->random() % n; }
static float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
static int wrap_col(int c) { return ((c % N) + N) % N; }
static float minf(float a, float b) { return a < b ? a : b; }

static float wrap_angle(float a)
{
    while (a > PI) a -= TAU;
    while (a < -PI) a += TAU;
    return a;
}

// ---------------------------------------------------------------- sound

static void tone1(float f0, float f1, uint16_t ms, uint8_t wave, float vol, uint16_t delay)
{
    const tat_tone_t t = {f0, f1, ms, wave, vol, delay};
    T->tone(&t);
}

static void sfx_turn(void) { tone1(700, 900, 30, TAT_SQUARE, 0.3f, 0); }
static void sfx_lock(void) { tone1(180, 90, 60, TAT_TRIANGLE, 0.6f, 0); }
static void sfx_drop(void) { tone1(400, 120, 90, TAT_TRIANGLE, 0.6f, 0); }
static void sfx_start(void) { tone1(500, 900, 100, TAT_TRIANGLE, 0.6f, 0); }

static void sfx_clear(int n)
{
    static const float f[4] = {660, 880, 1100, 1320};
    const int count = n + 1 > 4 ? 4 : n + 1;
    for (int i = 0; i < count; i++)
        tone1(f[i], 0, i == 3 ? 200 : 70, TAT_SQUARE, i == 3 ? 0.7f : 0.6f, (uint16_t)(i * 70));
}

static void sfx_level_up(void)
{
    tone1(523, 1046, 160, TAT_TRIANGLE, 0.6f, 0);
    tone1(784, 1568, 220, TAT_TRIANGLE, 0.6f, 120);
}

static void sfx_game_over(void)
{
    tone1(392, 0, 160, TAT_SQUARE, 0.6f, 0);
    tone1(330, 0, 160, TAT_SQUARE, 0.6f, 160);
    tone1(262, 0, 320, TAT_SQUARE, 0.6f, 320);
}

// ---------------------------------------------------------------- pieces

// Cell (ring, col) of the current piece for rotation `r` at origin (ring0, col0).
static void piece_cells(int shape, int r, int ring0, int col0, int out[4][2])
{
    const Shape *s = &SHAPES[shape];
    for (int i = 0; i < 4; i++) {
        int row = s->cells[i][0], col = s->cells[i][1];
        for (int k = 0; k < r; k++) {
            const int nr = col, nc = s->size - 1 - row;
            row = nr, col = nc;
        }
        out[i][0] = ring0 - row;   /* rows go inward */
        out[i][1] = wrap_col(col0 + col);
    }
}

static bool fits(int shape, int r, int ring0, int col0)
{
    int c[4][2];
    piece_cells(shape, r, ring0, col0, c);
    for (int i = 0; i < 4; i++) {
        if (c[i][0] < 0) return false;
        if (c[i][0] >= RINGS) continue;   /* above the rim is fine while spawning */
        if (g.board[c[i][0]][c[i][1]]) return false;
    }
    return true;
}

// Which column real-world "up" points at (the piece drops from there).
static int top_column(void)
{
    return wrap_col((int)floorf(g.up / TAU * N) - SHAPES[g.piece].size / 2);
}

static void save_best(void) { T->save_set("best", g.best); }

static void spawn(void)
{
    g.piece = g.next;
    g.next = (int)rnd(7);
    g.rot = 0;
    g.p_ring = SPAWN_RING;
    g.p_col = top_column();
    g.fall_t = 0;
    g.lock_t = 0;
    if (!fits(g.piece, g.rot, g.p_ring, g.p_col)) {
        g.phase = GAME_OVER;
        g.phase_t = 0;
        if (g.got_best) save_best();
        sfx_game_over();
    }
}

static void new_game(void)
{
    memset(g.board, 0, sizeof(g.board));
    memset(g.clearing, 0, sizeof(g.clearing));
    g.score = 0;
    g.rings_cleared = 0;
    g.level = 1;
    g.outward = false;
    g.flip_t = 0;
    g.got_best = false;
    g.next = (int)rnd(7);
    g.phase = READY;
    g.phase_t = 0;
}

static float fall_interval(void)
{
    const float v = 0.9f * powf(0.82f, (float)(g.level - 1));
    return v < 0.12f ? 0.12f : v;
}

static void lock_piece(void)
{
    int c[4][2];
    piece_cells(g.piece, g.rot, g.p_ring, g.p_col, c);
    bool over = false;
    for (int i = 0; i < 4; i++) {
        if (c[i][0] >= RINGS) over = true;
        else g.board[c[i][0]][c[i][1]] = (uint8_t)(g.piece + 1);
    }
    if (over) {
        g.phase = GAME_OVER;
        g.phase_t = 0;
        if (g.got_best) save_best();
        sfx_game_over();
        return;
    }

    /* full rings clear */
    int n = 0;
    for (int r = 0; r < RINGS; r++) {
        bool full = true;
        for (int k = 0; k < N && full; k++) full = g.board[r][k] != 0;
        g.clearing[r] = full;
        n += full;
    }
    if (n) {
        static const int kPoints[5] = {0, 100, 300, 500, 800};
        g.score += kPoints[n < 4 ? n : 4] * g.level;
        g.rings_cleared += n;
        g.phase = CLEARING;
        g.phase_t = 0;
        sfx_clear(n);
    } else {
        sfx_lock();
        spawn();
    }
    if (g.score > g.best) {
        g.got_best = true;
        g.best = g.score;
    }
}

static void finish_clear(void)
{
    /* drop everything above a cleared ring inward by one */
    int dst = 0;
    for (int r = 0; r < RINGS; r++) {
        if (g.clearing[r]) continue;
        if (dst != r) memcpy(g.board[dst], g.board[r], N);
        dst++;
    }
    for (int r = dst; r < RINGS; r++) memset(g.board[r], 0, N);
    memset(g.clearing, 0, sizeof(g.clearing));

    const int new_level = 1 + g.rings_cleared / 8;
    if (new_level > g.level) {
        g.level = new_level;
        sfx_level_up();
        if (rnd(2)) {   /* half the levels turn the well inside out */
            g.outward = !g.outward;
            g.flip_t = 1.5f;
        }
    }
    spawn();
}

static int ghost_ring(void)
{
    int r = g.p_ring;
    while (r > 0 && fits(g.piece, g.rot, r - 1, g.p_col)) r--;
    return r;
}

// ---------------------------------------------------------------- update

static void update_tilt(const tat_input_t *in, float dt)
{
    /* gyro about the screen's normal: the world turns the other way on screen */
    const float gz = in->tilt.gz * (PI / 180.0f) * dt;
    g.up = wrap_angle(g.up - g.gyro_sign * gz);

    const float k = minf(1.0f, dt / 0.08f);
    g.grav_x += (in->tilt.ax - g.grav_x) * k;
    g.grav_y += (in->tilt.ay - g.grav_y) * k;
    const float upright = g.grav_x * g.grav_x + g.grav_y * g.grav_y;   /* 1 = on edge, 0 = flat */
    if (upright > 0.16f) {
        /* real-world up is the opposite of where gravity points on the screen */
        const float target = atan2f(-g.grav_y, -g.grav_x);
        if (g.had_target && fabsf(gz) > 0.004f) {
            /* gravity and gyro should agree on the direction of a turn */
            g.sign_score += wrap_angle(target - g.last_target) * (-g.gyro_sign * gz) * 400;
            if (g.sign_score < -1.0f) {
                g.gyro_sign = -g.gyro_sign;
                g.sign_score = 0;
            }
            g.sign_score = minf(g.sign_score, 3.0f);
        }
        g.last_target = target;
        g.had_target = true;
        g.up = wrap_angle(g.up + wrap_angle(target - g.up) * minf(1.0f, dt / 0.15f));
    } else {
        g.had_target = false;
    }
}

static void play(const tat_input_t *in, const tat_gestures_t *ges, float dt)
{
    // Turning the watch moves where "up" is: the piece follows it around the rim one
    // column at a time, as far as the pile allows.
    const int want = top_column();
    int guard = N;
    while (g.p_col != want && guard-- > 0) {
        const int step = wrap_col(want - g.p_col) <= N / 2 ? 1 : -1;
        if (!fits(g.piece, g.rot, g.p_ring, wrap_col(g.p_col + step))) break;
        g.p_col = wrap_col(g.p_col + step);
    }

    /* tap = rotate (nudging either way if it doesn't fit), hold = soft drop, PWR = hard drop */
    if (in->touch.pressed) g.touch_t0 = T->now_us();
    g.soft = in->touch.down && T->now_us() - g.touch_t0 > 250000;
    if (ges->tap && T->now_us() - g.touch_t0 < 250000) {
        static const int NUDGE[5] = {0, 1, -1, 2, -2};
        const int nr = (g.rot + 1) % 4;
        for (int i = 0; i < 5; i++) {
            if (fits(g.piece, nr, g.p_ring, wrap_col(g.p_col + NUDGE[i]))) {
                g.rot = nr;
                g.p_col = wrap_col(g.p_col + NUDGE[i]);
                sfx_turn();
                break;
            }
        }
    }
    if (in->pressed & TAT_BTN_B) {
        const int ring = ghost_ring();
        g.score += (g.p_ring - ring) * 2;
        g.p_ring = ring;
        sfx_drop();
        lock_piece();
        return;
    }

    g.fall_t += dt * (g.soft ? 10.0f : 1.0f);
    const float interval = fall_interval();
    while (g.fall_t >= interval) {
        g.fall_t -= interval;
        if (fits(g.piece, g.rot, g.p_ring - 1, g.p_col)) {
            g.p_ring--;
            g.lock_t = 0;
            if (g.soft) g.score += 1;
        } else {
            break;
        }
    }
    if (!fits(g.piece, g.rot, g.p_ring - 1, g.p_col)) {
        g.lock_t += dt;
        if (g.lock_t >= LOCK_DELAY) lock_piece();
    } else {
        g.lock_t = 0;
    }
}

// ---------------------------------------------------------------- drawing

static void build_merged(void)
{
    memset(g.merged, 0, sizeof(g.merged));
    for (int r = 0; r < RINGS; r++)
        for (int k = 0; k < N; k++) g.merged[r][k] = g.clearing[r] && g.board[r][k] ? 8 : g.board[r][k];
    if (g.phase == PLAYING) {
        int c[4][2];
        piece_cells(g.piece, g.rot, ghost_ring(), g.p_col, c);
        for (int i = 0; i < 4; i++)
            if (c[i][0] >= 0 && c[i][0] < RINGS + 4 && !g.merged[c[i][0]][c[i][1]]) g.merged[c[i][0]][c[i][1]] = 9;
        piece_cells(g.piece, g.rot, g.p_ring, g.p_col, c);
        for (int i = 0; i < 4; i++)
            if (c[i][0] >= 0 && c[i][0] < RINGS + 4) g.merged[c[i][0]][c[i][1]] = (uint8_t)(g.piece + 1);
    }
}

// Every pixel of the well, shaded from its distance and angle. This is why the console
// hands out the polar tables: an atan2 and a sqrt per pixel here would be 54,000 of each
// per frame.
static void draw_well(void)
{
    uint8_t *px = T->canvas_pixels(g.cv);
    if (!px || !g.pol_a || !g.pol_r) return;
    const bool blink = fmodf(g.phase_t, 0.12f) < 0.06f;
    for (int y = 0; y < CW; y++) {
        for (int x = 0; x < CW; x++) {
            const int i = (SCALE * y) * TAT_SCREEN + SCALE * x;
            const float r = g.pol_r[i] / 16.0f;
            uint8_t c = g.c_bg;
            if (r < CORE_R) {
                c = r > CORE_R - 2 ? g.c_core_edge : g.c_core;
            } else {
                int pring = (int)((r - CORE_R) / RING_H);
                if (pring > RINGS + 3) pring = RINGS + 3;
                const int ring = g.outward ? RINGS - 1 - pring : pring;   /* floor-first index */
                const float rf = r - CORE_R - pring * RING_H;
                const uint32_t rel = (uint32_t)g.pol_a[i] * N;   /* the pile is fixed to the screen */
                const int col = rel >> 16;
                const uint32_t frac = rel & 0xFFFF;
                /* one screen pixel of gap between wedges, scaled to this radius */
                const uint32_t gap = (uint32_t)(65536.0f / (TAU * r / N));
                uint8_t v = ring >= 0 ? g.merged[ring][col] : 0;
                if (pring >= RINGS && v >= 8) v = 0;   /* only the falling piece shows outside the rim */
                if (r >= RIM_R && v == 0) {
                    c = r < RIM_R + 1.5f ? g.c_rim : g.c_bg;
                } else if (v == 0) {
                    c = (rf < 1.0f || frac < gap / 2) ? g.c_grid : g.c_bg;
                } else if (v == 8) {
                    c = blink ? g.c_white : g.c_dim;
                } else if (v == 9) {
                    /* ghost: where the piece will land, in a dark tint of its colour */
                    c = (frac < gap || frac > 65535 - gap || rf < 1.0f) ? g.c_gap : g.c_dark[g.piece];
                } else {
                    const int p = v - 1;
                    if (frac < gap || frac > 65535 - gap || rf < 1.0f) c = g.c_gap;
                    else if (rf > RING_H - 2.0f || frac < gap * 3) c = g.c_lit[p];
                    else if (rf < 3.0f || frac > 65535 - gap * 3) c = g.c_dark[p];
                    else c = g.c_full[p];
                }
            }
            px[y * CW + x] = c;
        }
    }
}

static void draw_next(int cx, int cy)
{
    const Shape *s = &SHAPES[g.next];
    const int cell = 4;
    const int ox = cx - s->size * cell / 2, oy = cy - cell;
    for (int i = 0; i < 4; i++)
        T->canvas_fill_rect(g.cv, ox + s->cells[i][1] * cell, oy + s->cells[i][0] * cell, cell - 1, cell - 1,
                            g.c_full[g.next]);
}

static void banner(const char *top, const char *mid, const char *bottom, uint8_t col)
{
    const tat_banner_t b = {
        .top = top, .mid = mid, .bottom = bottom,
        .top_color = col, .mid_color = g.c_dim, .bottom_color = g.c_dim,
        .panel = g.c_panel, .border = g.c_box,
    };
    T->canvas_banner(g.cv, CW / 2, 100, CW - 60, &b);
}

// ---------------------------------------------------------------- the game

static void tt_begin(const tat_api_t *api)
{
    T = api;
    memset(&g, 0, sizeof(g));

    g.cv = T->canvas_create(SCALE, 0);
    if (!g.cv) {
        T->log("no memory for the canvas");
        return;
    }
    g.pol_a = T->polar_angles();
    g.pol_r = T->polar_radii();
    if (!g.pol_a || !g.pol_r) T->log("no polar tables; the well cannot be drawn");

    g.c_black = T->canvas_color(g.cv, T->rgb(0, 0, 0));
    g.c_bg = T->canvas_color(g.cv, T->rgb(12, 12, 20));
    g.c_grid = T->canvas_color(g.cv, T->rgb(26, 26, 40));
    g.c_core = T->canvas_color(g.cv, T->rgb(16, 19, 28));
    g.c_core_edge = T->canvas_color(g.cv, T->rgb(74, 80, 100));
    g.c_rim = T->canvas_color(g.cv, T->rgb(90, 96, 120));
    g.c_white = T->canvas_color(g.cv, T->rgb(255, 255, 255));
    g.c_dim = T->canvas_color(g.cv, T->rgb(150, 150, 160));
    g.c_gap = T->canvas_color(g.cv, T->rgb(8, 8, 12));
    g.c_ghost = T->canvas_color(g.cv, T->rgb(70, 74, 92));
    g.c_panel = T->canvas_color(g.cv, T->rgb(16, 18, 26));
    g.c_box = T->canvas_color(g.cv, T->rgb(90, 90, 100));
    g.c_yellow = T->canvas_color(g.cv, T->rgb(255, 217, 61));
    g.c_cyan = T->canvas_color(g.cv, T->rgb(80, 220, 240));
    g.c_danger = T->canvas_color(g.cv, T->rgb(255, 70, 70));
    g.c_go = T->canvas_color(g.cv, T->rgb(40, 200, 110));
    for (int i = 0; i < 7; i++) {
        const uint8_t r = PIECE_RGB[i][0], gr = PIECE_RGB[i][1], b = PIECE_RGB[i][2];
        g.c_full[i] = T->canvas_color(g.cv, T->rgb(r, gr, b));
        g.c_lit[i] = T->canvas_color(g.cv, T->rgb((uint8_t)(r + (255 - r) * 0.55f),
                                                  (uint8_t)(gr + (255 - gr) * 0.55f),
                                                  (uint8_t)(b + (255 - b) * 0.55f)));
        g.c_dark[i] = T->canvas_color(g.cv, T->rgb((uint8_t)(r * 0.5f), (uint8_t)(gr * 0.5f), (uint8_t)(b * 0.5f)));
    }

    g.grav_y = 1;
    g.up = -PI / 2;
    g.gyro_sign = 1;
    T->save_get("best", &g.best, 0);
    new_game();
    T->log("ready, best %d", g.best);
}

static void tt_enter(void)
{
    if (g.phase == PLAYING) T->menu_open();
    else T->menu_invalidate();
}

static void tt_update(float dt)
{
    if (!g.cv) return;
    if (dt > 1.0f / 30) dt = 1.0f / 30;

    const tat_input_t *in = T->input();
    const tat_gestures_t *ges = T->gestures();
    g.phase_t += dt;
    update_tilt(in, dt);

    if (T->menu_is_open()) {
        switch (T->menu_update()) {
        case 0: T->menu_toggle_sound(); break;
        case 1:
            new_game();
            T->menu_close();
            break;
        }
        return;
    }
    if (ges->swipe_left && g.phase != GAME_OVER) {
        T->menu_open();
        return;
    }

    switch (g.phase) {
    case READY:
        if (ges->tap) {
            g.phase = PLAYING;
            sfx_start();
            spawn();
        }
        break;
    case PLAYING: play(in, ges, dt); break;
    case CLEARING:
        if (g.phase_t > 0.35f) {
            g.phase = PLAYING;
            finish_clear();
        }
        break;
    case GAME_OVER:
        if (ges->tap && g.phase_t > 0.8f) new_game();
        break;
    }
}

static void tt_draw(void)
{
    if (!g.cv) return;

    if (T->menu_is_open()) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", g.best);
        const tat_menu_row_t rows[] = {
            T->menu_sound_row(),
            {"NEW GAME", "GO", T->ui_color(TAT_UI_ACCENT)},
            {"BEST", buf, T->ui_color(TAT_UI_LABEL)},
        };
        T->menu_draw(rows, 3, "PAUSED");
        return;
    }

    build_merged();
    draw_well();

    /* the core: score, level and the next piece */
    char buf[24];
    snprintf(buf, sizeof(buf), "%d", g.score);
    T->canvas_text_centered(g.cv, CW / 2, CW / 2 - 11, buf, g.c_white, 1, true);
    snprintf(buf, sizeof(buf), "L%d", g.level);
    T->canvas_text_centered(g.cv, CW / 2, CW / 2, buf, g.c_cyan, 1, false);
    if (g.phase == PLAYING || g.phase == CLEARING) draw_next(CW / 2, CW / 2 + 13);

    if (g.phase == READY) {
        banner("TAP TO DROP", "TURN THE WATCH TO AIM", "TAP: TURN  HOLD: DROP", g.c_white);
    } else if (g.phase == PLAYING && g.flip_t > 0) {
        g.flip_t -= 1.0f / 60;
        banner("GRAVITY FLIP!", g.outward ? "PIECES RISE FROM THE CORE" : "PIECES FALL FROM THE RIM", NULL,
               g.c_yellow);
    } else if (g.phase == GAME_OVER && g.phase_t > 0.5f) {
        snprintf(buf, sizeof(buf), g.got_best ? "NEW BEST %d!" : "SCORE %d", g.score);
        banner("GAME OVER", buf, "TAP TO PLAY", g.got_best ? g.c_yellow : g.c_danger);
    }

    T->canvas_present(g.cv);
}

static bool tt_keep_awake(void)
{
    return !T->menu_is_open() && (g.phase == PLAYING || g.phase == CLEARING);
}

const tat_game_t tat_game = {
    .magic = TAT_GAME_MAGIC,
    .api_major = TAT_API_MAJOR,
    .api_minor = TAT_API_MINOR,
    .id = "tiltatris",
    .name = "TILT-A-TRIS",
    .accent_r = 80, .accent_g = 220, .accent_b = 240,
    .assets = NULL,
    .asset_count = 0,
    .begin = tt_begin,
    .enter = tt_enter,
    .update = tt_update,
    .draw = tt_draw,
    .leave = NULL,
    .unload = NULL,
    .redraw = NULL,   /* the well is redrawn from scratch every frame */
    .keep_awake = tt_keep_awake,
};
