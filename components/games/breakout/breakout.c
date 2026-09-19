// BREAKOUT - four rings of bricks around a core, and an arc of a paddle on the rim.
//
// The board is round, so the paddle runs around the edge instead of along the bottom and
// the ball falls off in every direction at once. By default the paddle is gravity-locked:
// it sits at the real-world bottom of the watch, so you aim by turning the watch like a
// wheel and the rings appear to rotate around a paddle that never moves. Drag and follow
// are there for playing it flat on a table.
//
//   turn the watch - move the paddle (tilt mode)
//   drag / touch   - move the paddle (drag and follow modes)
//   tap            - launch the ball
//   PWR            - cycle which of the three controls is in use
//   swipe left     - pause menu
//
// Written against tat_api.h alone: it includes nothing else from the console and calls
// nothing by name.
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "tat/tat_api.h"

static const tat_api_t *T;

// ---------------------------------------------------------------- the board

#define PI 3.14159265f
#define TAU (2 * PI)

// Drawn on a 233 x 233 canvas shown at 2x; the game itself keeps screen units, so every
// radius below is in screen pixels and the drawing halves them. CW is spelled out rather
// than asked for with canvas_width() because the shading indexes the console's polar
// tables in screen space - that arithmetic only works for a scale-2 canvas that covers
// the screen, so a different width would be wrong rather than merely surprising.
#define SCALE 2
#define CW ((TAT_SCREEN + SCALE - 1) / SCALE)   /* 233 */
#define C 233.0f                                /* the centre, in screen pixels */
#define R 233.0f

#define PADDLE_R 212.0f
#define PADDLE_T 10.0f
#define BALL_R 6.5f
#define LOSE_R (R + 10)
#define CORE_R 34.0f
#define RING_IN (CORE_R + 10)
#define GAP 3.0f
#define ROWS 4
#define FIELD_R 120.0f
#define TAPER 0.4f
#define BASE_SPEED 200.0f
#define TAP_MAX_TRAVEL 0.08f   /* radians of drag that still counts as a tap */
#define MAX_DEFLECT 0.5f       /* max paddle bounce angle off-center (rad); < field half-angle */

static const float SPEEDS[3] = {160, 200, 250};
static const char *const SPEED_NAMES[3] = {"SLOW", "NORMAL", "FAST"};

// Fixed arrays instead of the C++ version's vectors. Each bound is a number the board's
// own geometry or timing makes it hard to reach, and going over one costs a spark rather
// than a ball:
//
// MAX_BRICKS   bricks in one ring. n is worked out from ROWS, RING_IN, FIELD_R, GAP and
//              TAPER, all fixed above, so today's four rings come out at 16, 17, 18 and
//              19 wedges. 24 leaves room to retune those constants without having to
//              work the new count out by hand first, and build_rings clamps to it anyway.
// MAX_CAPS     power-ups in flight. One drops from 12% of broken bricks and leaves the
//              board about 1.5 s later, so a dozen at once needs forty-odd bricks broken
//              inside a second and a half.
// MAX_POPS     "+30" score labels. A pop fades in about 0.6 s (life 1, at 1.6 a second).
// MAX_SHOCKS   impact rings. One per brick touched, gone in 0.2 s (life 1, at 5 a second).
//
// MAX_BALLS and MAX_PARTS are the caps the C++ already had, and mean the same thing.
#define MAX_BALLS 8
#define MAX_PARTS 140
#define MAX_BRICKS 24
#define MAX_CAPS 12
#define MAX_POPS 24
#define MAX_SHOCKS 24

typedef struct {
    float r, g, b;
} RGB;

// The brick and power-up colours. The C++ folded these out of hex literals at compile
// time; C gets the same numbers written out, with the hex kept alongside so they can
// still be read as colours.
static const RGB PALETTE[5] = {
    {255, 79, 154},    /* ff4f9a */
    {255, 138, 61},    /* ff8a3d */
    {255, 210, 63},    /* ffd23f */
    {94, 230, 168},    /* 5ee6a8 */
    {79, 179, 255},    /* 4fb3ff */
};
static const RGB WHITE = {255, 255, 255};

typedef enum { CAP_WIDE, CAP_MULTI, CAP_SLOW, CAP_SHIELD } CapType;

static const struct {
    char letter;
    RGB color;
} CAPS[4] = {
    {'W', {255, 210, 63}},    /* ffd23f */
    {'M', {255, 79, 154}},    /* ff4f9a */
    {'S', {94, 230, 168}},    /* 5ee6a8 */
    {'O', {125, 249, 255}},   /* 7df9ff */
};

typedef enum { MODE_DRAG, MODE_FOLLOW, MODE_TILT } Mode;
static const char *const MODE_NAMES[3] = {"DRAG", "FOLLOW", "TILT"};

typedef struct {
    int x, y, w, h;
} Rect;

typedef struct {
    float r0, r1, gapPx, off, spd;
    int n, hp;
    RGB color;
    int8_t bricks[MAX_BRICKS];   /* only the first n are in play */
    /* shading, precomputed (palette indices) */
    uint8_t full, dim, outline, shade, dim_shade;
    uint16_t r0_16, r1_16, gap_frac, outline_frac, off16;
} Ring;

typedef struct {
    float x, y, vx, vy;
    bool stuck, dead;
} Ball;

typedef struct {
    float a, r, v;
    CapType type;
    bool done;
} Cap;

typedef struct {
    float x, y, vx, vy, life;
    RGB color;
} Part;

// Little "+30" that floats up from a broken brick.
typedef struct {
    float x, y, life;
    int score;
    RGB color;
} Pop;

// A ring that snaps outward where the ball hit.
typedef struct {
    float x, y, life;
    RGB color;
} Shock;

static struct {
    tat_canvas_t *cv;
    // The screen's polar tables, fetched once: an atan2 and a sqrt per pixel to shade the
    // board would be 54,000 of each a frame. Either can be NULL, and then the board simply
    // is not drawn - the score and the banners still are.
    const uint16_t *pol_a, *pol_r;

    /* game state */
    int score, lives, level;
    float paddle, pv, hw, target;
    bool has_target;
    float wideT, slowT, speed, flash;
    bool shield, over;
    Ring rings[ROWS];   /* always exactly ROWS of them, so there is no count to carry */
    Ball balls[MAX_BALLS];
    int n_balls;
    Cap caps[MAX_CAPS];
    int n_caps;
    Part parts[MAX_PARTS];
    int n_parts;
    Pop pops[MAX_POPS];
    int n_pops;
    Shock shocks[MAX_SHOCKS];
    int n_shocks;
    int combo;   /* bricks hit since the last paddle touch */

    /* settings (persisted under the descriptor's id) */
    Mode mode;
    bool tilt_invert;
    int speed_idx;
    float base_speed;

    /* input */
    float lastA, moved;
    bool ptr_down, ptr_skip;
    float mode_label_t;
    struct {
        int ring, idx;
        float time;
    } brick_flash;   /* the sector lit for a moment after a hit; -1 when there is none */
    float grav_x, grav_y;   /* smoothed gravity in screen axes (tilt mode) */

    /* rendering */
    uint8_t *scene;    /* rings + core + backdrop, rebuilt when a brick changes */
    bool full_redraw;  /* scene is stale */
    uint8_t c_black, c_white, c_track, c_core, c_core_edge, c_shield, c_star;
    uint8_t c_star2, c_pad, c_pad_flash, c_pad_edge, c_ball_rim, c_life_off;
    uint8_t c_cyan, c_yellow, c_panel;
} g;

// ---------------------------------------------------------------- odds and ends

static float frand(void) { return (float)T->random() / 4294967296.0f; }

static float wrap(float a)
{
    a = fmodf(a + PI, TAU);
    if (a < 0) a += TAU;
    return a - PI;
}

static float norm(float a)
{
    a = fmodf(a, TAU);
    return a < 0 ? a + TAU : a;
}

static float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
static float minf(float a, float b) { return a < b ? a : b; }
static float maxf(float a, float b) { return a > b ? a : b; }
static int mini(int a, int b) { return a < b ? a : b; }
static int maxi(int a, int b) { return a > b ? a : b; }

static uint16_t toA16(float a) { return (uint16_t)((int)(norm(a) / TAU * 65536.0f) & 0xFFFF); }

static RGB lerp_rgb(RGB a, RGB b, float t)
{
    const RGB out = {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t};
    return out;
}

// The C++ built its colour tables with a constexpr rgb(); T->rgb is a function pointer, so
// every colour here is mixed at runtime instead, and the palette entries are taken in
// begin() where the C++ took them in loadAssets().
static tat_color_t col(RGB c, float k)
{
    return T->rgb((uint8_t)(c.r * k), (uint8_t)(c.g * k), (uint8_t)(c.b * k));
}

// ---------------------------------------------------------------- sound

static void tone1(float f0, float f1, uint16_t ms, uint8_t wave, float vol, uint16_t delay)
{
    const tat_tone_t t = {f0, f1, ms, wave, vol, delay};
    T->tone(&t);
}

static void sfx_launch(void) { tone1(420, 900, 70, TAT_SQUARE, 0.7f, 0); }

static void sfx_paddle(void)
{
    tone1(330, 240, 45, TAT_SQUARE, 0.8f, 0);
    tone1(140, 90, 40, TAT_TRIANGLE, 0.5f, 0);
}

// Each brick in a rally is a semitone higher: a rising run while the ball works.
static void sfx_brick(int combo, bool destroyed)
{
    const float f = 523.0f * powf(1.0595f, (float)mini(combo, 18));
    if (destroyed) {
        tone1(f, f * 1.5f, 55, TAT_SQUARE, 0.75f, 0);
        tone1(180, 120, 35, TAT_NOISE, 0.35f, 0);
    } else {
        tone1(f * 0.5f, f * 0.4f, 45, TAT_TRIANGLE, 0.6f, 0);
    }
}

static void sfx_power_up(void)
{
    tone1(660, 0, 70, TAT_TRIANGLE, 0.7f, 0);
    tone1(880, 0, 70, TAT_TRIANGLE, 0.7f, 70);
    tone1(1320, 0, 110, TAT_TRIANGLE, 0.7f, 140);
}

static void sfx_shield(void) { tone1(900, 1600, 120, TAT_TRIANGLE, 0.6f, 0); }

static void sfx_life_lost(void)
{
    tone1(420, 110, 320, TAT_SQUARE, 0.8f, 0);
    tone1(200, 60, 260, TAT_NOISE, 0.4f, 0);
}

static void sfx_level_clear(void)
{
    tone1(523, 0, 90, TAT_SQUARE, 0.7f, 0);
    tone1(659, 0, 90, TAT_SQUARE, 0.7f, 90);
    tone1(784, 0, 90, TAT_SQUARE, 0.7f, 180);
    tone1(1047, 0, 220, TAT_SQUARE, 0.8f, 270);
}

static void sfx_game_over(void)
{
    tone1(392, 0, 160, TAT_SQUARE, 0.7f, 0);
    tone1(330, 0, 160, TAT_SQUARE, 0.7f, 160);
    tone1(262, 0, 320, TAT_SQUARE, 0.7f, 320);
}

// ---------------------------------------------------------------- setup

// Pixel-accurate bbox of an annular arc, with a small safety margin.
static Rect arc_box(float a0, float a1, float rin, float rout)
{
    float minx = 1e9f, miny = 1e9f, maxx = -1e9f, maxy = -1e9f;
    const float radii[2] = {rin, rout};   /* the C++ ranged over a braced list of the two */
    const int steps = maxi(2, (int)ceilf((a1 - a0) / 0.08f));
    for (int i = 0; i <= steps; i++) {
        const float a = a0 + (a1 - a0) * i / steps, ca = cosf(a), sa = sinf(a);
        for (int k = 0; k < 2; k++) {
            minx = minf(minx, C + ca * radii[k]);
            maxx = maxf(maxx, C + ca * radii[k]);
            miny = minf(miny, C + sa * radii[k]);
            maxy = maxf(maxy, C + sa * radii[k]);
        }
    }
    const int x0 = (int)floorf(minx) - 2, y0 = (int)floorf(miny) - 2;
    const Rect out = {x0, y0, (int)ceilf(maxx) + 2 - x0, (int)ceilf(maxy) + 2 - y0};
    return out;
}

static void build_rings(void)
{
    float w[ROWS], total = 0;
    for (int i = 0; i < ROWS; i++) {
        w[i] = 1 + TAPER * 1.5f * (ROWS > 1 ? (float)i / (ROWS - 1) : 0);
        total += w[i];
    }
    const float span = FIELD_R - RING_IN;
    float r0 = RING_IN;
    for (int i = 0; i < ROWS; i++) {
        const float next = r0 + span * w[i] / total, r1 = next - GAP, th = r1 - r0, mid = (r0 + r1) / 2;
        Ring *rg = &g.rings[i];
        memset(rg, 0, sizeof(*rg));
        rg->r0 = r0;
        rg->r1 = r1;
        rg->n = mini(MAX_BRICKS, maxi(8, (int)lroundf(TAU * mid / (th * 1.7f))));
        rg->gapPx = clampf(th / 10, 0.8f, 1.6f);
        rg->hp = (i == 0 ? 2 : 1) + (g.level > 1 && i < 2 ? 1 : 0);
        const float t = ROWS > 1 ? (float)i / (ROWS - 1) * 4 : 0;
        const int k = mini((int)t, 3);
        rg->color = lerp_rgb(PALETTE[k], PALETTE[k + 1], t - k);
        rg->off = frand() * TAU;
        rg->off16 = toA16(rg->off);
        rg->spd = (i % 2 ? 1 : -1) * (0.1f + 0.03f * i);
        for (int b = 0; b < rg->n; b++) rg->bricks[b] = (int8_t)rg->hp;

        rg->full = T->canvas_color(g.cv, col(rg->color, 1));
        rg->dim = T->canvas_color(g.cv, col(rg->color, 0.85f));
        rg->outline = T->canvas_color(g.cv, col(lerp_rgb(rg->color, WHITE, 0.75f), 1));
        rg->shade = T->canvas_color(g.cv, col(rg->color, 0.55f));
        rg->dim_shade = T->canvas_color(g.cv, col(rg->color, 0.5f));
        rg->r0_16 = (uint16_t)(r0 * 16);
        rg->r1_16 = (uint16_t)(r1 * 16);
        const float frac_per_rad = rg->n / TAU * 65536.0f;
        rg->gap_frac = (uint16_t)(rg->gapPx / mid * frac_per_rad);
        rg->outline_frac = (uint16_t)(1.5f / mid * frac_per_rad);
        r0 = next;
    }
}

static void place_stuck(void)
{
    const float rr = PADDLE_R - BALL_R - 2;
    for (int i = 0; i < g.n_balls; i++)
        if (g.balls[i].stuck) {
            g.balls[i].x = C + cosf(g.paddle) * rr;
            g.balls[i].y = C + sinf(g.paddle) * rr;
        }
}

static void stick_ball(void)
{
    g.n_balls = 1;
    memset(&g.balls[0], 0, sizeof(Ball));
    g.balls[0].stuck = true;
    place_stuck();
}

static void new_game(void)
{
    g.score = 0;
    g.lives = 3;
    g.level = 1;
    g.paddle = PI / 2;
    g.has_target = false;
    g.pv = 0;
    g.hw = 0.3f;
    g.wideT = g.slowT = g.flash = 0;
    g.shield = g.over = false;
    g.speed = g.base_speed;
    g.n_caps = 0;
    g.n_parts = 0;
    g.n_pops = 0;
    g.n_shocks = 0;
    g.combo = 0;
    build_rings();
    stick_ball();
    g.full_redraw = true;
}

static float spd(void) { return g.speed * (g.slowT > 0 ? 0.65f : 1); }

static void launch(void)
{
    bool any_stuck = false;
    for (int i = 0; i < g.n_balls; i++) any_stuck |= g.balls[i].stuck;
    if (any_stuck) sfx_launch();
    place_stuck();
    for (int i = 0; i < g.n_balls; i++) {
        Ball *b = &g.balls[i];
        if (!b->stuck) continue;
        const float dir = g.paddle + PI + (frand() - 0.5f) * 0.4f;
        b->vx = cosf(dir) * spd();
        b->vy = sinf(dir) * spd();
        b->stuck = false;
    }
}

// ---------------------------------------------------------------- simulation

static void reflect(Ball *b, float nx, float ny)
{
    const float d = b->vx * nx + b->vy * ny;
    b->vx -= 2 * d * nx;
    b->vy -= 2 * d * ny;
}

static void burst(float x, float y, RGB color, int n)
{
    const float dx = x - C, dy = y - C, r = maxf(1.0f, hypotf(dx, dy));
    for (int i = 0; i < n && g.n_parts < MAX_PARTS; i++) {
        const float a = frand() * TAU, v = 40 + frand() * 90;
        Part *p = &g.parts[g.n_parts++];
        p->x = x;
        p->y = y;
        p->vx = cosf(a) * v + dx / r * 60;
        p->vy = sinf(a) * v + dy / r * 60;
        p->life = 0.5f + frand() * 0.3f;
        p->color = color;
    }
}

static Rect brick_box(const Ring *rg, int idx)
{
    const float sec = TAU / rg->n;
    return arc_box(rg->off + idx * sec, rg->off + (idx + 1) * sec, rg->r0 - 1, rg->r1 + 1);
}

static void hit_brick(int gi, int idx)
{
    Ring *rg = &g.rings[gi];
    rg->bricks[idx]--;
    const float mid = rg->off + (idx + 0.5f) * TAU / rg->n, rm = (rg->r0 + rg->r1) / 2;
    const float x = C + cosf(mid) * rm, y = C + sinf(mid) * rm;
    const bool destroyed = rg->bricks[idx] <= 0;
    g.combo++;
    sfx_brick(g.combo, destroyed);
    if (g.n_shocks < MAX_SHOCKS) {
        Shock *s = &g.shocks[g.n_shocks++];
        s->x = x;
        s->y = y;
        s->life = 1.0f;
        s->color = destroyed ? rg->color : WHITE;
    }
    g.brick_flash.ring = gi;
    g.brick_flash.idx = idx;
    g.brick_flash.time = 0.06f;
    if (destroyed) {
        const int points = 10 * (ROWS - gi) * (g.combo >= 4 ? 2 : 1);   /* rallies pay double */
        g.score += points;
        burst(x, y, rg->color, 14);
        if (g.n_pops < MAX_POPS) {
            Pop *p = &g.pops[g.n_pops++];
            p->x = x;
            p->y = y;
            p->life = 1.0f;
            p->score = points;
            p->color = rg->color;
        }
        if (frand() < 0.12f && g.n_caps < MAX_CAPS) {
            Cap *cp = &g.caps[g.n_caps++];
            cp->a = mid;
            cp->r = rm;
            cp->v = 15;
            cp->type = (CapType)(T->random() % 4);
            cp->done = false;
        }
    } else {
        g.score += 2;
        burst(x, y, WHITE, 5);
    }
    g.full_redraw = true;
}

static void apply_cap(CapType t)
{
    if (t == CAP_WIDE) g.wideT = 12;
    if (t == CAP_SLOW) g.slowT = 8;
    if (t == CAP_SHIELD) g.shield = true;
    if (t == CAP_MULTI) {
        // The C++ copied the live balls into a second vector first, so the balls it added
        // were not themselves split again. Here the snapshot is just the count taken
        // before any are appended - new ones land past it and are left alone.
        static const float ANG[2] = {0.4f, -0.4f};
        const int n0 = g.n_balls;
        bool any_live = false;
        for (int i = 0; i < n0; i++) any_live |= !g.balls[i].stuck;
        if (!any_live) {
            launch();
            return;
        }
        for (int i = 0; i < n0; i++) {
            if (g.balls[i].stuck) continue;
            const Ball src = g.balls[i];
            for (int k = 0; k < 2; k++) {
                if (g.n_balls >= MAX_BALLS) return;
                const float c = cosf(ANG[k]), sn = sinf(ANG[k]);
                Ball *nb = &g.balls[g.n_balls++];
                nb->x = src.x;
                nb->y = src.y;
                nb->vx = src.vx * c - src.vy * sn;
                nb->vy = src.vx * sn + src.vy * c;
                nb->stuck = false;
                nb->dead = false;
            }
        }
    }
}

static void step_ball(Ball *b, float h)
{
    const float px = b->x, py = b->y;
    b->x += b->vx * h;
    b->y += b->vy * h;
    const float dx = b->x - C, dy = b->y - C;
    const float r = maxf(0.001f, hypotf(dx, dy)), pr = hypotf(px - C, py - C);
    const float nx = dx / r, ny = dy / r;
    const float vr = b->vx * nx + b->vy * ny;

    // The center orb is a display (score/lives) only; the ball passes through it.

    if (g.shield && r >= PADDLE_R + 5 && pr < PADDLE_R + 5) {
        if (vr > 0) reflect(b, nx, ny);
        b->x = px;
        b->y = py;
        g.shield = false;
        sfx_shield();
        burst(b->x, b->y, CAPS[CAP_SHIELD].color, 14);
        return;
    }

    if (r + BALL_R >= PADDLE_R && pr + BALL_R < PADDLE_R && vr > 0) {
        const float th = atan2f(dy, dx), d = wrap(th - g.paddle);
        if (fabsf(d) <= g.hw + BALL_R / PADDLE_R) {
            const float k = clampf(d / g.hw, -1, 1);
            // Aim back toward the center, steered by where it hit the paddle.
            // Turning the watch in tilt mode moves the paddle on screen without the
            // player "swiping" it, so paddle-motion spin only applies to touch modes.
            const float spin = g.mode == MODE_TILT ? 0 : clampf(g.pv * 0.05f, -0.25f, 0.25f);
            // Cap the deflection so the ball always heads into the brick field:
            // from the paddle (r=212) the field (r=120) spans +/-asin(120/212) ~ +/-0.60 rad.
            const float deflect = clampf(k * 0.45f + spin, -MAX_DEFLECT, MAX_DEFLECT);
            const float dir = th + PI - deflect;
            b->vx = cosf(dir);
            b->vy = sinf(dir);
            b->x = px;
            b->y = py;
            g.speed = minf(g.speed + 2, g.base_speed * 1.6f);
            g.flash = 0.12f;
            g.combo = 0;
            sfx_paddle();
            return;
        }
    }

    if (r > LOSE_R) {
        b->dead = true;
        return;
    }

    const float th = atan2f(dy, dx);
    for (int gi = 0; gi < ROWS; gi++) {
        const Ring *rg = &g.rings[gi];
        if (r + BALL_R < rg->r0 || r - BALL_R > rg->r1) continue;
        const float sec = TAU / rg->n, pad = BALL_R / r;
        const float offs[3] = {0.0f, pad, -pad};   /* the C++ ranged over a braced list */
        for (int k = 0; k < 3; k++) {
            const int idx = (int)floorf(norm(th + offs[k] - rg->off) / sec) % rg->n;
            if (rg->bricks[idx] > 0) {
                const bool prevInBand = pr + BALL_R > rg->r0 && pr - BALL_R < rg->r1;
                if (prevInBand) reflect(b, -ny, nx);
                else if ((pr < rg->r0 && vr > 0) || (pr > rg->r1 && vr < 0)) reflect(b, nx, ny);
                b->x = px;
                b->y = py;
                hit_brick(gi, idx);
                return;
            }
        }
    }
}

// ---------------------------------------------------------------- settings

static void load_settings(void)
{
    int m = (int)g.mode;
    T->save_get("mode", &m, 3);
    g.mode = (Mode)m;
    // save_get/save_set are int-only, so the tilt_invert flag lives as 0 or 1. That is
    // what the C++ Store wrote for a bool too, so an existing save still reads back.
    int inv = g.tilt_invert ? 1 : 0;
    T->save_get("invert", &inv, 0);
    g.tilt_invert = inv != 0;
    T->save_get("speed", &g.speed_idx, 3);
    g.base_speed = SPEEDS[g.speed_idx];
}

static void save_settings(void)
{
    T->save_set("mode", (int)g.mode);
    T->save_set("invert", g.tilt_invert ? 1 : 0);
    T->save_set("speed", g.speed_idx);
}

static void set_mode(Mode m)
{
    g.mode = m;
    g.has_target = false;
    g.mode_label_t = 2.0f;
}

static void open_menu(void)
{
    T->menu_open();
    g.ptr_down = false;
}

static void close_menu(void)
{
    T->menu_close();
    g.full_redraw = true;
    g.base_speed = SPEEDS[g.speed_idx];
    g.speed = minf(g.base_speed * (1 + 0.08f * (g.level - 1)), g.base_speed * 1.5f);
    save_settings();
}

static void handle_input(float dt)
{
    const tat_input_t *in = T->input();
    const tat_gestures_t *ges = T->gestures();
    const tat_touch_t *t = &in->touch;
    const float ta = atan2f(t->y + 0.5f - C, t->x + 0.5f - C);

    if (T->menu_is_open()) {
        switch (T->menu_update()) {
        case 0: set_mode((Mode)((g.mode + 1) % 3)); break;
        case 1: g.tilt_invert = !g.tilt_invert; break;
        case 2: g.speed_idx = (g.speed_idx + 1) % 3; break;
        case 3: T->menu_toggle_sound(); break;
        }
        // RESUME and HOME are handled inside menu_update(); either way the game resumes
        // through close_menu()'s settings save.
        if (!T->menu_is_open()) close_menu();
        return;
    }
    if (ges->swipe_left && !g.over) {
        open_menu();
        return;
    }

    // PWR changes control; BOOT is the console's "home" (handled by the engine).
    if (in->clicked & TAT_BTN_B) {
        set_mode((Mode)((g.mode + 1) % 3));
        save_settings();
    }

    if (t->pressed) {
        g.ptr_down = true;
        g.lastA = ta;
        g.moved = 0;
        g.ptr_skip = false;
        if (g.over) {
            new_game();
            g.ptr_skip = true;
        }
    }
    if (g.ptr_down && (t->down || t->released)) {
        if (g.mode == MODE_FOLLOW) {
            g.target = ta;
            g.has_target = true;
        } else if (g.mode == MODE_DRAG) {
            g.paddle = wrap(g.paddle + wrap(ta - g.lastA));
        }
        g.moved += fabsf(wrap(ta - g.lastA));
        g.lastA = ta;
    }
    if (t->released && g.ptr_down) {
        if (!g.ptr_skip && g.moved < TAP_MAX_TRAVEL && !g.over) launch();
        g.ptr_down = false;
    }

    if (g.mode == MODE_TILT) {
        // Gravity-locked paddle: it always sits at the real-world bottom of the
        // watch. Turn the watch like a wheel and the paddle stays put while the
        // rings rotate around it.
        // 1) Low-pass the gravity vector (~90 ms) to strip hand tremor and
        //    sensor noise before it becomes an angle.
        const float gx = in->tilt.ax * (g.tilt_invert ? -1 : 1), gy = in->tilt.ay;
        const float lp = 1.0f - expf(-dt / 0.09f);
        g.grav_x += (gx - g.grav_x) * lp;
        g.grav_y += (gy - g.grav_y) * lp;
        const float mag = sqrtf(g.grav_x * g.grav_x + g.grav_y * g.grav_y);
        // Lying flat there is no "down" in the screen plane; hold position.
        if (mag > 0.25f) {
            // 2) Adaptive follow: lazy for tiny wobbles, snappy for real turns.
            //    ~2 deg off -> ~5/s (glides), ~30 deg off -> ~35/s (near-instant).
            const float err = wrap(atan2f(g.grav_y, g.grav_x) - g.paddle);
            const float k = 4.0f + 60.0f * fabsf(err);
            g.paddle = wrap(g.paddle + err * minf(1.0f, k * dt));
        }
        g.has_target = false;
    }
}

static void sim(float dt)
{
    // The three effect lists are stepped and culled in one pass each. Removing means
    // moving the last element into the hole, so `i` must NOT advance afterwards: the
    // element that just arrived has not been stepped yet and is only reached by going
    // round again on the same index.
    for (int i = 0; i < g.n_parts;) {
        Part *p = &g.parts[i];
        p->x += p->vx * dt;
        p->y += p->vy * dt;
        p->vx *= 0.96f;
        p->vy *= 0.96f;
        p->life -= dt;
        if (p->life <= 0) g.parts[i] = g.parts[--g.n_parts];
        else i++;
    }
    for (int i = 0; i < g.n_pops;) {
        Pop *p = &g.pops[i];
        p->y -= 34 * dt;   /* drifts up as it fades */
        p->life -= dt * 1.6f;
        if (p->life <= 0) g.pops[i] = g.pops[--g.n_pops];
        else i++;
    }
    if (g.brick_flash.ring >= 0) {
        g.brick_flash.time -= dt;
        if (g.brick_flash.time <= 0) {
            g.full_redraw = true;
            g.brick_flash.ring = -1;
        }
    }
    for (int i = 0; i < g.n_shocks;) {
        g.shocks[i].life -= dt * 5.0f;
        if (g.shocks[i].life <= 0) g.shocks[i] = g.shocks[--g.n_shocks];
        else i++;
    }
    if (g.flash > 0) g.flash -= dt;
    if (g.mode_label_t > 0) g.mode_label_t -= dt;

    const float prev = g.paddle;
    handle_input(dt);
    if (g.over || T->menu_is_open()) return;

    if (g.has_target) g.paddle += wrap(g.target - g.paddle) * minf(1.0f, dt * 20);
    g.paddle = wrap(g.paddle);
    g.pv = wrap(g.paddle - prev) / maxf(dt, 1e-3f);

    if (g.wideT > 0) g.wideT -= dt;
    if (g.slowT > 0) g.slowT -= dt;
    g.hw += ((g.wideT > 0 ? 0.48f : 0.3f) - g.hw) * minf(1.0f, dt * 8);

    for (int i = 0; i < g.n_balls; i++) {
        Ball *b = &g.balls[i];
        if (b->stuck) {
            const float rr = PADDLE_R - BALL_R - 2;
            b->x = C + cosf(g.paddle) * rr;
            b->y = C + sinf(g.paddle) * rr;
            continue;
        }
        const int steps = maxi(1, (int)ceilf(spd() * dt / 3));
        for (int k = 0; k < steps && !b->dead; k++) {
            step_ball(b, dt / steps);
            const float m = maxf(1e-3f, hypotf(b->vx, b->vy));
            b->vx = b->vx / m * spd();
            b->vy = b->vy / m * spd();
        }
    }
    // Culling the dead is its own pass, as it was in the C++: stepping is what may add
    // balls (the multiball power-up), so nothing is removed while that is going on.
    for (int i = 0; i < g.n_balls;) {
        if (g.balls[i].dead) g.balls[i] = g.balls[--g.n_balls];
        else i++;
    }
    if (g.n_balls == 0) {
        g.lives--;
        g.n_caps = 0;
        g.wideT = g.slowT = 0;
        g.combo = 0;
        if (g.lives <= 0) {
            g.over = true;
            sfx_game_over();
            T->log("game over, score %d", g.score);
        } else {
            sfx_life_lost();
            stick_ball();
        }
    }

    for (int i = 0; i < g.n_caps;) {
        Cap *cp = &g.caps[i];
        cp->v += 150 * dt;
        cp->r += cp->v * dt;
        if (!cp->done && cp->r >= PADDLE_R - 8 && cp->r <= PADDLE_R + PADDLE_T &&
            fabsf(wrap(cp->a - g.paddle)) <= g.hw + 0.06f) {
            cp->done = true;
            sfx_power_up();
            apply_cap(cp->type);
            burst(C + cosf(cp->a) * cp->r, C + sinf(cp->a) * cp->r, CAPS[cp->type].color, 10);
        }
        if (cp->done || cp->r >= R + 12) g.caps[i] = g.caps[--g.n_caps];
        else i++;
    }

    bool cleared = true;
    for (int gi = 0; gi < ROWS; gi++)
        for (int k = 0; k < g.rings[gi].n; k++)
            if (g.rings[gi].bricks[k] > 0) cleared = false;
    if (cleared) {
        sfx_level_clear();
        g.level++;
        g.speed = minf(g.base_speed * (1 + 0.08f * (g.level - 1)), g.base_speed * 1.5f);
        build_rings();
        g.n_caps = 0;
        stick_ball();
        g.full_redraw = true;
    }
}

// ---------------------------------------------------------------- rendering

// Colours that fade (particles, pops, shockwaves) are quantised to four
// brightness steps so they don't eat the palette.
static uint8_t fade(RGB c, float k)
{
    const int q = maxi(1, mini(4, (int)(k * 4 + 0.999f)));
    return T->canvas_color(g.cv, col(c, q / 4.0f));
}

// Paint one brick sector solid, so a hit flashes before the scene redraws it.
static void flash_brick(const Ring *rg, int idx, uint8_t colour)
{
    uint8_t *px = T->canvas_pixels(g.cv);
    if (!px || !g.pol_a || !g.pol_r) return;
    const Rect box = brick_box(rg, idx);
    const uint32_t sec = 65536u / (uint32_t)rg->n;
    for (int y = maxi(0, box.y / 2); y < mini(CW, (box.y + box.h) / 2 + 1); y++) {
        for (int x = maxi(0, box.x / 2); x < mini(CW, (box.x + box.w) / 2 + 1); x++) {
            const int i = (2 * y) * TAT_SCREEN + 2 * x;
            const uint16_t r16 = g.pol_r[i];
            if (r16 < rg->r0_16 || r16 > rg->r1_16) continue;
            const uint16_t rel = (uint16_t)(g.pol_a[i] - rg->off16);
            if (rel / sec != (uint32_t)idx) continue;
            px[y * CW + x] = colour;
        }
    }
}

// The static scene at one canvas pixel: backdrop, brick rings and the core.
static uint8_t shade_scene(int x, int y)
{
    const int i = (2 * y) * TAT_SCREEN + 2 * x;
    const uint16_t r16 = g.pol_r[i];
    if (r16 > (uint16_t)(R * 16)) return g.c_black;
    const uint16_t a = g.pol_a[i];

    if (r16 <= (uint16_t)(CORE_R * 16)) {
        return r16 >= (uint16_t)((CORE_R - 2.0f) * 16) ? g.c_core_edge : g.c_core;
    }
    if (r16 < (uint16_t)(FIELD_R * 16)) {
        for (int gi = 0; gi < ROWS; gi++) {
            const Ring *rg = &g.rings[gi];
            if (r16 < rg->r0_16 || r16 > rg->r1_16) continue;
            const uint32_t prod = (uint32_t)(uint16_t)(a - rg->off16) * (uint32_t)rg->n;
            const int idx = prod >> 16;
            const uint16_t frac = prod & 0xFFFF;
            const int8_t hp = rg->bricks[idx];
            if (hp <= 0 || frac < rg->gap_frac || frac > 65535 - rg->gap_frac) break;
            // Bevel: lit along the outer edge and leading side, dark along the
            // inner edge and trailing side, so each brick reads as a little tile.
            const uint16_t edge = rg->gap_frac + rg->outline_frac;
            const bool outer = r16 > rg->r1_16 - 32, inner = r16 < rg->r0_16 + 32;
            const bool lead = frac < edge, trail = frac > 65535 - edge;
            if (hp > 1) {
                if (outer || lead) return rg->outline;
                if (inner || trail) return rg->shade;
                return rg->full;
            }
            if (inner || trail) return rg->dim_shade;
            return rg->dim;
        }
    }
    // Backdrop: a faint scatter of stars in the empty space.
    const unsigned h = (unsigned)(x * 2654435761u) ^ (unsigned)(y * 40503u);
    if ((h >> 9) % 97 == 0) return ((h >> 3) & 3) ? g.c_star : g.c_star2;
    return g.c_black;
}

static void build_scene(void)
{
    // Without the polar tables there is no board to build; leave it black rather than
    // fill it with nonsense, and the score and banners still come out on top.
    if (!g.pol_a || !g.pol_r) {
        memset(g.scene, g.c_black, CW * CW);
        return;
    }
    for (int y = 0; y < CW; y++)
        for (int x = 0; x < CW; x++) g.scene[y * CW + x] = shade_scene(x, y);
}

// Paddle track, shield and paddle, painted over the scene every frame.
static void draw_paddle(void)
{
    uint8_t *px = T->canvas_pixels(g.cv);
    if (!px || !g.pol_a || !g.pol_r) return;
    const uint16_t pad_a16 = toA16(g.paddle), pad_hw16 = (uint16_t)(g.hw / TAU * 65536.0f);
    const float cr = PADDLE_R + PADDLE_T / 2, cr2 = (PADDLE_T / 2) * (PADDLE_T / 2);
    float cap_x[2], cap_y[2];
    for (int k = 0; k < 2; k++) {
        const float a = g.paddle + (k ? g.hw : -g.hw);
        cap_x[k] = C + cosf(a) * cr;
        cap_y[k] = C + sinf(a) * cr;
    }
    const uint8_t pad = g.flash > 0 ? g.c_pad_flash : g.c_pad;
    const int y0 = (int)((C - PADDLE_R - PADDLE_T - 2) / 2), y1 = (int)((C + PADDLE_R + PADDLE_T + 2) / 2);
    for (int y = maxi(0, y0); y <= mini(CW - 1, y1); y++) {
        for (int x = 0; x < CW; x++) {
            const int i = (2 * y) * TAT_SCREEN + 2 * x;
            const uint16_t r16 = g.pol_r[i];
            if (r16 < (uint16_t)((PADDLE_R - 1) * 16) || r16 > (uint16_t)((PADDLE_R + PADDLE_T + 1) * 16)) continue;
            uint8_t colour = g.c_track;
            if (g.shield && r16 >= (uint16_t)((PADDLE_R + 4) * 16) && r16 <= (uint16_t)((PADDLE_R + 6) * 16))
                colour = g.c_shield;
            const int16_t d = (int16_t)(g.pol_a[i] - pad_a16);
            bool on = (d < 0 ? -d : d) <= pad_hw16 && r16 >= (uint16_t)(PADDLE_R * 16) &&
                      r16 <= (uint16_t)((PADDLE_R + PADDLE_T) * 16);
            if (!on) {
                const float fx = 2 * x + 1.0f, fy = 2 * y + 1.0f;
                for (int k = 0; k < 2 && !on; k++) {
                    const float ex = fx - cap_x[k], ey = fy - cap_y[k];
                    on = ex * ex + ey * ey <= cr2;
                }
            }
            if (on) {
                // Lit outer face, darker inner face.
                colour = r16 > (uint16_t)((PADDLE_R + PADDLE_T - 2.5f) * 16) ? g.c_pad_edge
                         : r16 < (uint16_t)((PADDLE_R + 2.5f) * 16)          ? g.c_white
                                                                            : pad;
            }
            px[y * CW + x] = colour;
        }
    }
}

static void disc(float cx, float cy, float r, uint8_t colour)
{
    cx /= 2, cy /= 2, r /= 2;
    const int y0 = (int)floorf(cy - r), y1 = (int)ceilf(cy + r);
    for (int y = y0; y <= y1; y++) {
        const float dy = y + 0.5f - cy;
        if (dy * dy > r * r) continue;
        const float half = sqrtf(r * r - dy * dy);
        const int xa = (int)ceilf(cx - half - 0.5f), xb = (int)floorf(cx + half - 0.5f);
        if (xb >= xa) T->canvas_fill_rect(g.cv, xa, y, xb - xa + 1, 1, colour);
    }
}

// Text in screen units: scale 2 -> canvas 1, scale 4 -> canvas 2.
static void text_c(float cx, float cy, const char *s, uint8_t colour, int scale)
{
    T->canvas_text_centered(g.cv, (int)(cx / 2), (int)(cy / 2), s, colour, maxi(1, scale / 2), true);
}

static void draw_core(void)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", g.score);
    text_c(C, C - 7, buf, g.c_white, strlen(buf) > 3 ? 2 : 3);
    for (int i = 0; i < 3; i++) disc(C - 11 + i * 11, C + 19, 3.4f, i < g.lives ? g.c_white : g.c_life_off);
}

static bool any_stuck_ball(void)
{
    for (int i = 0; i < g.n_balls; i++)
        if (g.balls[i].stuck) return true;
    return false;
}

static bool any_moving_ball(void)
{
    for (int i = 0; i < g.n_balls; i++)
        if (!g.balls[i].stuck) return true;
    return false;
}

static void draw_board(void)
{
    if (g.full_redraw) {
        build_scene();
        g.full_redraw = false;
    }
    uint8_t *px = T->canvas_pixels(g.cv);
    if (px) memcpy(px, g.scene, CW * CW);
    draw_paddle();
    if (!g.over) draw_core();

    if (g.brick_flash.ring >= 0 && g.brick_flash.time > 0)
        flash_brick(&g.rings[g.brick_flash.ring], g.brick_flash.idx, g.c_white);

    /* ---- sprites */
    for (int i = 0; i < g.n_caps; i++) {
        const Cap *cp = &g.caps[i];
        const float x = C + cosf(cp->a) * cp->r, y = C + sinf(cp->a) * cp->r;
        disc(x, y, 12, T->canvas_color(g.cv, col(CAPS[cp->type].color, 1)));
        disc(x - 3, y - 3, 4, T->canvas_color(g.cv, col(lerp_rgb(CAPS[cp->type].color, WHITE, 0.5f), 1)));
        const char s[2] = {CAPS[cp->type].letter, 0};
        text_c(x, y, s, g.c_black, 2);
    }
    for (int i = 0; i < g.n_balls; i++) {
        disc(g.balls[i].x, g.balls[i].y, BALL_R, g.c_ball_rim);
        disc(g.balls[i].x, g.balls[i].y, BALL_R - 1.6f, g.c_white);
    }
    for (int i = 0; i < g.n_parts; i++) {
        const Part *p = &g.parts[i];
        T->canvas_fill_rect(g.cv, (int)(p->x / 2) - 1, (int)(p->y / 2) - 1, 2, 2,
                            fade(p->color, clampf(p->life * 2, 0, 1)));
    }

    // Impact rings: a quick shockwave where the ball struck.
    for (int i = 0; i < g.n_shocks; i++) {
        const Shock *s = &g.shocks[i];
        const int rad = (int)((6 + (1.0f - s->life) * 26) / 2);
        const uint8_t colour = fade(s->color, clampf(s->life, 0, 1));
        for (int k = 0; k < 24; k++) {
            const float a = k * TAU / 24;
            T->canvas_pixel(g.cv, (int)(s->x / 2 + cosf(a) * rad), (int)(s->y / 2 + sinf(a) * rad), colour);
        }
    }

    // Score pops floating up from broken bricks.
    for (int i = 0; i < g.n_pops; i++) {
        const Pop *p = &g.pops[i];
        char buf[12];
        snprintf(buf, sizeof(buf), "+%d", p->score);
        text_c(p->x, p->y, buf, fade(p->color, clampf(p->life, 0, 1)), 2);
    }

    const bool stuck = any_stuck_ball();
    // Tooltips live in the empty band between the brick field and the paddle
    // track: two lines at the top, two at the bottom.
    char mode_line[32];
    snprintf(mode_line, sizeof(mode_line), "CONTROL: %s", MODE_NAMES[g.mode]);
    if (!g.over && stuck) {
        char top[32];
        if (g.level > 1) snprintf(top, sizeof(top), "LEVEL %d", g.level);
        else snprintf(top, sizeof(top), "%s", mode_line);
        text_c(C, 58, top, g.c_cyan, 2);
        text_c(C, 84, g.level > 1 ? mode_line : "PWR: CHANGE", g.level > 1 ? g.c_cyan : g.c_yellow, 2);
        text_c(C, 382, "TAP TO LAUNCH", g.c_white, 2);
        text_c(C, 408, "SWIPE < MENU", g.c_yellow, 2);
    } else if (g.mode_label_t > 0 && !g.over) {
        text_c(C, 70, mode_line, g.c_cyan, 2);
    }
    if (g.over) {
        char buf[32];
        snprintf(buf, sizeof(buf), "SCORE %d", g.score);
        T->canvas_fill_rect(g.cv, CW / 2 - 60, CW / 2 - 34, 120, 68, g.c_panel);
        T->canvas_rect(g.cv, CW / 2 - 60, CW / 2 - 34, 120, 68, g.c_core_edge);
        text_c(C, C - 40, "GAME OVER", g.c_white, 4);
        text_c(C, C + 2, buf, g.c_yellow, 3);
        text_c(C, C + 42, "TAP TO PLAY", g.c_white, 2);
    }
    T->canvas_present(g.cv);
}

// ---------------------------------------------------------------- the game

static void bk_begin(const tat_api_t *api)
{
    T = api;
    memset(&g, 0, sizeof(g));

    // Asked for first, as the C++ called Polar::init() first: the tables are the better
    // part of a megabyte and the console builds them on the first request, so they should
    // meet an empty heap rather than one the canvas has already been cut out of.
    g.pol_a = T->polar_angles();
    g.pol_r = T->polar_radii();
    if (!g.pol_a || !g.pol_r) T->log("polar tables alloc failed");

    g.cv = T->canvas_create(SCALE, 0);
    if (!g.cv) {
        T->log("no memory for the canvas");
        return;
    }
    // The scene is the board without the moving parts, kept so a frame in which nothing
    // broke is a memcpy rather than 54,000 shaded pixels. Everything below depends on it,
    // and update() and draw() do nothing while it is missing.
    g.scene = T->alloc(CW * CW);
    if (!g.scene) {
        T->log("no memory for the scene");
        return;
    }

    g.c_black = T->canvas_color(g.cv, T->rgb(0, 0, 0));
    g.c_white = T->canvas_color(g.cv, T->rgb(255, 255, 255));
    g.c_track = T->canvas_color(g.cv, T->rgb(22, 22, 28));
    g.c_core = T->canvas_color(g.cv, T->rgb(0x10, 0x13, 0x1a));
    g.c_core_edge = T->canvas_color(g.cv, T->rgb(0x4a, 0x50, 0x60));
    g.c_shield = T->canvas_color(g.cv, T->rgb(0x5a, 0xb0, 0xb4));
    g.c_star = T->canvas_color(g.cv, T->rgb(40, 42, 56));
    g.c_star2 = T->canvas_color(g.cv, T->rgb(70, 74, 96));
    g.c_pad = T->canvas_color(g.cv, T->rgb(235, 238, 245));
    g.c_pad_flash = T->canvas_color(g.cv, T->rgb(255, 210, 63));   /* ffd23f */
    g.c_pad_edge = T->canvas_color(g.cv, T->rgb(120, 130, 160));
    g.c_ball_rim = T->canvas_color(g.cv, T->rgb(170, 190, 220));
    g.c_life_off = T->canvas_color(g.cv, T->rgb(60, 60, 60));
    g.c_cyan = T->canvas_color(g.cv, T->rgb(40, 230, 230));    /* the console's own cyan */
    g.c_yellow = T->canvas_color(g.cv, T->rgb(255, 220, 40));   /* and its yellow */
    g.c_panel = T->canvas_color(g.cv, T->rgb(16, 18, 26));

    // The C++ gave these their values as member initialisers. memset cannot, and
    // save_get leaves a value alone when nothing is stored, so the defaults have to be
    // in place before the settings are read.
    g.mode = MODE_TILT;
    g.speed_idx = 1;
    g.base_speed = BASE_SPEED;
    g.speed = BASE_SPEED;
    g.grav_y = 1;
    g.brick_flash.ring = -1;

    load_settings();
    new_game();
    T->log("ready: control=%s, tap/BOOT to launch, PWR cycles control, swipe left for settings",
           MODE_NAMES[g.mode]);
}

static void bk_enter(void)
{
    // Coming back from the home screen: repaint, and pause a game that was mid-rally.
    g.full_redraw = true;
    if (any_moving_ball() && !g.over) open_menu();
}

static void bk_update(float dt)
{
    if (!g.scene) return;
    sim(minf(dt, 1.0f / 30));
}

static void bk_draw(void)
{
    if (!g.scene) return;
    if (T->menu_is_open()) {
        const tat_menu_row_t rows[] = {
            {"CONTROL", MODE_NAMES[g.mode], 0},
            {"TILT DIR", g.tilt_invert ? "MIRROR" : "NORMAL", 0},
            {"SPEED", SPEED_NAMES[g.speed_idx], 0},
            T->menu_sound_row(),
        };
        T->menu_draw(rows, 4, "PAUSED");
        return;
    }
    draw_board();
}

static bool bk_keep_awake(void)
{
    if (g.over || T->menu_is_open()) return false;
    return any_moving_ball();
}

static void bk_unload(void)
{
    if (g.scene) T->free(g.scene);
    g.scene = NULL;
    if (g.cv) T->canvas_destroy(g.cv);
    g.cv = NULL;
}

const tat_game_t tat_game = {
    .magic = TAT_GAME_MAGIC,
    .api_major = TAT_API_MAJOR,
    .api_minor = TAT_API_MINOR,
    .id = "breakout",   /* also the save namespace, so the old control settings survive */
    .name = "BREAKOUT",
    .accent_r = 255, .accent_g = 210, .accent_b = 63,
    .assets = NULL,
    .asset_count = 0,
    .begin = bk_begin,
    .enter = bk_enter,
    .update = bk_update,
    .draw = bk_draw,
    .leave = NULL,
    .unload = bk_unload,
    .redraw = NULL,   /* the board is drawn from the scene buffer every frame anyway */
    .keep_awake = bk_keep_awake,
};
