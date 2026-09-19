// SKATER GIRLZ - a top-down skate run across an endless lot.
//
// You are always at the middle of the screen and always rolling forward, which is up the
// screen. Turning the watch like a wheel steers; tipping it forward is the throttle. Tap
// to ollie over the cracks and the cones, and land on a rail to grind it.
//
//   turn the watch - steer
//   tip forward    - faster, tip back to slow
//   tap            - ollie; hold for more air
//   swipe left     - pause menu
//
// The world is turned into the skater's own frame here rather than by rotating the
// finished picture. Rotating the picture is cheaper and is what GRAND PRIX does, but it
// spins the score along with everything else - fine for a few degrees of roll, useless
// when the heading can be anywhere on the circle.
//
// Written against tat_api.h alone. Built as a package with tools/mktat.py.
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "tat/tat_api.h"

static const tat_api_t *T;

#define SCALE 2
#define CW ((TAT_SCREEN + SCALE - 1) / SCALE)   /* 233 */
#define CC (CW / 2)
#define PI 3.14159265f
#define TAU (2 * PI)

// The lot is endless, so things are kept in a disc around the skater: new ones appear at
// the far edge in the direction of travel, old ones are dropped once they fall behind.
// Thirty-odd things spread over a disc twice the width of the screen. The first cut had
// three times as many in a disc barely bigger than the screen: a dozen obstacles were
// visible at once and a run lasted about six seconds. A skate lot wants room.
#define KEEP_R 250.0f
#define SPAWN_R 225.0f
#define MAX_THINGS 34
#define CLEAR_R 80.0f        /* nothing starts this close, so a run begins on open ground */

#define MAX_SPEED 118.0f     /* world units a second */
#define ACCEL 70.0f
#define TURN_RATE 2.6f       /* radians a second at full lock */
#define FULL_LOCK 0.62f      /* radians of wheel for full steering */
#define GRAVITY 260.0f       /* for the ollie, in height units */
#define OLLIE_V 78.0f
#define OLLIE_HOLD_V 122.0f
#define RAIL_H 9.0f          /* how high a rail sits: you have to be above it to land */
#define LIVES 3

typedef enum { T_CRACK, T_CONE, T_BENCH, T_RAIL, T_GAP } Kind;

typedef struct {
    float x, y;       /* world */
    float a;          /* which way it lies, for the long ones */
    float len;        /* half length for bench and rail */
    uint8_t kind;
} Thing;

typedef enum { READY, SKATING, BAIL, OVER } Phase;

static struct {
    tat_canvas_t *cv;

    /* palette */
    uint8_t c_road, c_line, c_crack, c_gap, c_gap_lip;
    uint8_t c_cone, c_cone2, c_bench, c_bench2, c_rail, c_rail_hi;
    uint8_t c_skater, c_skater2, c_board, c_shadow;
    uint8_t c_text, c_dim, c_accent, c_danger, c_panel;

    /* the run */
    Phase phase;
    float phase_t;
    float px, py;        /* where the skater is on the lot */
    float heading;       /* which way she is pointing */
    float speed;
    float z, vz;         /* height off the ground, for the ollie */
    bool grinding;
    float lean;          /* smoothed steering, for how far the board tips over */
    float dist;
    int score, bonus, best;
    int lives;
    int combo;

    /* held for steering and throttle */
    float grav_x, grav_y, grav_z;
    float pitch, pitch_neutral;

    Thing things[MAX_THINGS];
    int n_things;

    uint32_t seed;
    bool dirty;
} g;

// ---------------------------------------------------------------- odds and ends

static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

static float frand(float lo, float hi)
{
    g.seed = g.seed * 1664525u + 1013904223u;
    return lo + (hi - lo) * ((g.seed >> 8 & 0xFFFF) / 65535.0f);
}

// Distance from a point to a line segment, which is what every long thing needs: a bench
// and a rail are both "am I near this line".
static float dist_to_segment(float px, float py, float ax, float ay, float bx, float by)
{
    const float vx = bx - ax, vy = by - ay;
    const float wx = px - ax, wy = py - ay;
    const float len2 = vx * vx + vy * vy;
    float t = len2 > 0.0001f ? (wx * vx + wy * vy) / len2 : 0;
    t = clampf(t, 0, 1);
    const float dx = px - (ax + vx * t), dy = py - (ay + vy * t);
    return sqrtf(dx * dx + dy * dy);
}

static void thing_ends(const Thing *t, float *ax, float *ay, float *bx, float *by)
{
    const float c = cosf(t->a) * t->len, s = sinf(t->a) * t->len;
    *ax = t->x - c, *ay = t->y - s;
    *bx = t->x + c, *by = t->y + s;
}

// ---------------------------------------------------------------- sound

static void tone1(float f0, float f1, uint16_t ms, uint8_t wave, float vol, uint16_t delay)
{
    const tat_tone_t t = {f0, f1, ms, wave, vol, delay};
    T->tone(&t);
}

static void sfx_ollie(void) { tone1(300, 700, 70, TAT_TRIANGLE, 0.45f, 0); }
static void sfx_land(void) { tone1(180, 110, 50, TAT_NOISE, 0.35f, 0); }
static void sfx_grind(void) { tone1(150, 190, 120, TAT_NOISE, 0.30f, 0); }
static void sfx_clip(void) { tone1(220, 90, 110, TAT_NOISE, 0.6f, 0); }

static void sfx_bail(void)
{
    tone1(260, 70, 240, TAT_NOISE, 0.7f, 0);
    tone1(150, 60, 220, TAT_SQUARE, 0.5f, 90);
}

static void sfx_start(void) { tone1(440, 880, 110, TAT_TRIANGLE, 0.6f, 0); }

static void sfx_over(void)
{
    tone1(392, 0, 150, TAT_SQUARE, 0.6f, 0);
    tone1(330, 0, 150, TAT_SQUARE, 0.6f, 150);
    tone1(247, 0, 320, TAT_SQUARE, 0.6f, 300);
}

// ---------------------------------------------------------------- the lot

static void add_thing(float x, float y)
{
    if (g.n_things >= MAX_THINGS) return;
    Thing *t = &g.things[g.n_things++];
    memset(t, 0, sizeof(*t));
    t->x = x;
    t->y = y;
    t->a = frand(0, TAU);
    const float r = frand(0, 1);
    // Cracks are decoration and everything else is in the way. The mix keeps the lot
    // looking lived-in without making it impassable.
    if (r < 0.40f) {
        t->kind = T_CRACK;
        t->len = frand(4, 14);
    } else if (r < 0.62f) {
        t->kind = T_CONE;
    } else if (r < 0.74f) {
        t->kind = T_BENCH;
        t->len = frand(10, 18);
    } else if (r < 0.94f) {
        t->kind = T_RAIL;   /* the thing you actually want to find */
        t->len = frand(20, 38);
    } else {
        t->kind = T_GAP;
        t->len = frand(8, 12);
    }
}

// Keep the disc around the skater stocked, dropping whatever has fallen behind.
static void restock(void)
{
    for (int i = 0; i < g.n_things;) {
        const float dx = g.things[i].x - g.px, dy = g.things[i].y - g.py;
        if (dx * dx + dy * dy > KEEP_R * KEEP_R) g.things[i] = g.things[--g.n_things];
        else i++;
    }
    // New things go in ahead of her, spread across a wide arc so that turning does not
    // run into an empty lot.
    while (g.n_things < MAX_THINGS) {
        const float a = g.heading + frand(-1.5f, 1.5f);
        const float r = frand(SPAWN_R * 0.75f, SPAWN_R);
        add_thing(g.px + cosf(a) * r, g.py + sinf(a) * r);
    }
}

static void start_run(void)
{
    g.px = g.py = 0;
    g.heading = -PI / 2;   /* up the lot */
    g.speed = 0;
    g.z = g.vz = 0;
    g.grinding = false;
    g.lean = 0;
    g.dist = 0;
    g.score = g.bonus = 0;
    g.combo = 0;
    g.lives = LIVES;
    g.n_things = 0;
    g.phase = READY;
    g.phase_t = 0;
    g.dirty = true;
    restock();
    // Clear the ground she starts on. Rolling straight into something before the first
    // ollie is not a difficulty setting, it is a bad start.
    for (int i = 0; i < g.n_things;) {
        const float dx = g.things[i].x, dy = g.things[i].y;
        if (dx * dx + dy * dy < CLEAR_R * CLEAR_R) g.things[i] = g.things[--g.n_things];
        else i++;
    }
}

static void bail(void)
{
    g.lives--;
    g.combo = 0;
    g.grinding = false;
    g.speed *= 0.25f;
    g.z = g.vz = 0;
    sfx_bail();
    if (g.lives > 0) {
        g.phase = BAIL;
        g.phase_t = 0;
    } else {
        g.phase = OVER;
        g.phase_t = 0;
        if (g.score > g.best) {
            g.best = g.score;
            T->save_set("best", g.best);
        }
        sfx_over();
    }
}

// ---------------------------------------------------------------- skating

static void step(const tat_input_t *in, float dt)
{
    // Steering and throttle are read the way GRAND PRIX reads them, because gravity does
    // not drift and a gyro does: the wheel angle comes from where down is, and the tip
    // forward and back from how far the face has leaned away from you.
    const float k = 1.0f - expf(-dt / 0.12f);
    g.grav_x += (in->tilt.ax - g.grav_x) * k;
    g.grav_y += (in->tilt.ay - g.grav_y) * k;
    g.grav_z += (in->tilt.az - g.grav_z) * k;
    g.pitch = atan2f(g.grav_z, sqrtf(g.grav_x * g.grav_x + g.grav_y * g.grav_y));

    float wheel = 0;
    if (g.grav_x * g.grav_x + g.grav_y * g.grav_y > 0.09f) wheel = atan2f(g.grav_x, g.grav_y);
    float s = clampf(wheel / FULL_LOCK, -1, 1);
    const float dead = 0.07f;
    s = fabsf(s) < dead ? 0 : (s - copysignf(dead, s)) / (1 - dead);
    g.lean += (s - g.lean) * (1.0f - expf(-dt / 0.09f));
    // You cannot carve while your wheels are off the ground.
    if (g.z <= 0.01f) g.heading += g.lean * TURN_RATE * dt * clampf(g.speed / MAX_SPEED + 0.25f, 0, 1);
    if (g.heading > PI) g.heading -= TAU;
    if (g.heading < -PI) g.heading += TAU;

    const float want = clampf(0.45f + (g.pitch - g.pitch_neutral) * 1.9f, 0.0f, 1.0f) * MAX_SPEED;
    g.speed += clampf(want - g.speed, -ACCEL * 1.6f * dt, ACCEL * dt);
    g.speed = clampf(g.speed, 0, MAX_SPEED);

    /* the ollie */
    if (in->touch.pressed && g.z <= 0.01f) {
        g.vz = OLLIE_V;
        g.grinding = false;
        sfx_ollie();
    }
    // Holding on through the first part of the jump gets you higher, which is what makes
    // a rail reachable and a wide gap clearable.
    if (in->touch.down && g.vz > 0 && g.vz < OLLIE_HOLD_V) g.vz += 150.0f * dt;

    const bool was_air = g.z > 0.01f;
    if (g.z > 0 || g.vz > 0) {
        g.vz -= GRAVITY * dt;
        g.z += g.vz * dt;
        if (g.z <= 0) {
            g.z = 0;
            g.vz = 0;
            if (was_air) sfx_land();
        }
    }

    const float fx = cosf(g.heading), fy = sinf(g.heading);
    g.px += fx * g.speed * dt;
    g.py += fy * g.speed * dt;
    g.dist += g.speed * dt;

    /* what is underfoot */
    bool on_rail = false;
    for (int i = 0; i < g.n_things; i++) {
        Thing *t = &g.things[i];
        float ax, ay, bx, by;
        if (t->kind == T_CONE) {
            const float dx = t->x - g.px, dy = t->y - g.py;
            if (g.z < 6.0f && dx * dx + dy * dy < 7.0f * 7.0f) {
                // A cone is a clip, not a fall: it costs your speed and your combo, and it
                // is knocked flat so you do not hit the same one twice.
                t->kind = T_CRACK;
                t->len = 5;
                g.speed *= 0.55f;
                g.combo = 0;
                sfx_clip();
            }
        } else if (t->kind == T_BENCH) {
            thing_ends(t, &ax, &ay, &bx, &by);
            if (g.z < 8.0f && dist_to_segment(g.px, g.py, ax, ay, bx, by) < 6.0f) {
                g.speed *= 0.45f;
                g.combo = 0;
                sfx_clip();
            }
        } else if (t->kind == T_RAIL) {
            thing_ends(t, &ax, &ay, &bx, &by);
            if (dist_to_segment(g.px, g.py, ax, ay, bx, by) < 6.5f) {
                // Coming down onto it from above is a grind; ploughing into it is not.
                if (g.z >= RAIL_H - 2.0f && g.vz <= 0) {
                    on_rail = true;
                    g.z = RAIL_H;
                    g.vz = 0;
                    if (!g.grinding) {
                        g.grinding = true;
                        g.combo++;
                        sfx_grind();
                    }
                } else if (g.z < RAIL_H - 2.0f) {
                    /* caught the side of it rather than landing on top */
                    g.speed *= 0.5f;
                    g.combo = 0;
                    sfx_clip();
                }
            }
        } else if (t->kind == T_GAP) {
            const float dx = t->x - g.px, dy = t->y - g.py;
            if (g.z < 3.0f && dx * dx + dy * dy < t->len * t->len) {
                bail();
                return;
            }
        }
    }

    if (g.grinding && !on_rail) {
        g.grinding = false;
        g.vz = 24.0f;   /* pops off the end */
    }
    if (g.grinding) g.bonus += (int)(120.0f * dt) * (g.combo < 5 ? g.combo : 5);

    // Worked out from the distance rather than added to a frame at a time: a fraction of
    // a point per frame is under one, and the cast would take every one of them to zero.
    g.score = (int)(g.dist * 0.5f) + g.bonus;
    restock();
}

// ---------------------------------------------------------------- drawing

// World to screen. Forward is up the screen, so the whole lot is turned into her frame:
// right is across, forward is negative y.
static void to_screen(float wx, float wy, int *sx, int *sy)
{
    const float dx = wx - g.px, dy = wy - g.py;
    const float fx = cosf(g.heading), fy = sinf(g.heading);
    const float rx = -fy, ry = fx;
    *sx = CC + (int)(dx * rx + dy * ry);
    *sy = CC + (int)(-(dx * fx + dy * fy));
}

static void draw_seg(float ax, float ay, float bx, float by, uint8_t col, int thick)
{
    int x0, y0, x1, y1;
    to_screen(ax, ay, &x0, &y0);
    to_screen(bx, by, &x1, &y1);
    T->canvas_line(g.cv, x0, y0, x1, y1, col);
    if (thick > 1) {
        T->canvas_line(g.cv, x0 + 1, y0, x1 + 1, y1, col);
        T->canvas_line(g.cv, x0, y0 + 1, x1, y1 + 1, col);
    }
}

static void draw_lot(void)
{
    T->canvas_clear(g.cv, g.c_road);

    // Lane markings: dashes laid out on the lot itself, so they slide and swing exactly as
    // the skater turns. This is most of the sense of speed, and it costs nothing.
    const float spacing = 46.0f;
    const float base_x = floorf(g.px / spacing) * spacing;
    const float base_y = floorf(g.py / spacing) * spacing;
    for (int i = -4; i <= 4; i++) {
        const float lx = base_x + i * spacing;
        for (int k = -4; k <= 4; k++) {
            const float ly = base_y + k * spacing;
            draw_seg(lx, ly - 9, lx, ly + 9, g.c_line, 1);
        }
    }

    for (int i = 0; i < g.n_things; i++) {
        const Thing *t = &g.things[i];
        int sx, sy;
        float ax, ay, bx, by;
        if (t->kind == T_CRACK) {
            thing_ends(t, &ax, &ay, &bx, &by);
            draw_seg(ax, ay, bx, by, g.c_crack, 1);
        } else if (t->kind == T_CONE) {
            to_screen(t->x, t->y, &sx, &sy);
            T->canvas_fill_circle(g.cv, sx, sy, 4, g.c_cone);
            T->canvas_fill_circle(g.cv, sx, sy - 1, 2, g.c_cone2);
        } else if (t->kind == T_BENCH) {
            thing_ends(t, &ax, &ay, &bx, &by);
            draw_seg(ax, ay, bx, by, g.c_bench, 3);
            draw_seg(ax, ay, bx, by, g.c_bench2, 1);
        } else if (t->kind == T_RAIL) {
            thing_ends(t, &ax, &ay, &bx, &by);
            draw_seg(ax, ay, bx, by, g.c_rail, 3);
            draw_seg(ax, ay, bx, by, g.c_rail_hi, 1);
        } else if (t->kind == T_GAP) {
            to_screen(t->x, t->y, &sx, &sy);
            T->canvas_fill_circle(g.cv, sx, sy, (int)t->len + 1, g.c_gap_lip);
            T->canvas_fill_circle(g.cv, sx, sy, (int)t->len, g.c_gap);
        }
    }
}

static void draw_skater(void)
{
    // She is always at the middle, facing up. Height lifts her off her shadow, which is
    // the only way to see how much air an ollie got.
    const int lift = (int)(g.z * 0.55f);
    const int sx = CC, sy = CC - lift;

    T->canvas_fill_circle(g.cv, CC, CC + 2, 5, g.c_shadow);

    /* the board, leaning the way she is carving */
    const float tilt = g.lean * 0.5f;
    const float c = cosf(tilt), s = sinf(tilt);
    const int bx0 = sx - (int)(8 * s), by0 = sy - (int)(8 * c);
    const int bx1 = sx + (int)(8 * s), by1 = sy + (int)(8 * c);
    T->canvas_line(g.cv, bx0, by0, bx1, by1, g.c_board);
    T->canvas_line(g.cv, bx0 + 1, by0, bx1 + 1, by1, g.c_board);

    T->canvas_fill_circle(g.cv, sx, sy - 2, 4, g.c_skater);
    T->canvas_fill_circle(g.cv, sx, sy - 5, 3, g.c_skater2);
    if (g.grinding) {
        /* sparks, so a grind is obvious without reading the score */
        for (int i = 0; i < 3; i++)
            T->canvas_pixel(g.cv, sx + (int)frand(-7, 7), sy + (int)frand(2, 8), g.c_accent);
    }
}

static void draw_hud(void)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", g.score);
    T->canvas_text_centered(g.cv, CC, 12, buf, g.c_text, 1, true);
    for (int i = 0; i < LIVES; i++)
        T->canvas_fill_circle(g.cv, CC - (LIVES - 1) * 6 + i * 12, 25, 3,
                              i < g.lives ? g.c_skater : g.c_dim);
    if (g.grinding && g.combo > 1) {
        snprintf(buf, sizeof(buf), "GRIND X%d", g.combo < 5 ? g.combo : 5);
        T->canvas_text_centered(g.cv, CC, CC + 52, buf, g.c_accent, 1, true);
    }
    if (g.best) {
        snprintf(buf, sizeof(buf), "BEST %d", g.best);
        T->canvas_text_centered(g.cv, CC, CW - 12, buf, g.c_dim, 1, false);
    }
}

static void banner(const char *top, const char *sub, uint8_t col)
{
    const tat_banner_t b = {
        .top = top, .mid = sub,
        .top_color = col, .mid_color = g.c_text,
        .panel = g.c_panel, .border = col,
        .bars = true,
    };
    T->canvas_banner(g.cv, CC, CC - (sub ? 13 : 8), CW - 54, &b);
}

// ---------------------------------------------------------------- the game

static void sg_begin(const tat_api_t *api)
{
    T = api;
    memset(&g, 0, sizeof(g));
    g.cv = T->canvas_create(SCALE, 0);
    if (!g.cv) {
        T->log("no memory for the lot");
        return;
    }
    g.c_road = T->canvas_color(g.cv, T->rgb(58, 58, 66));
    g.c_line = T->canvas_color(g.cv, T->rgb(94, 94, 104));
    g.c_crack = T->canvas_color(g.cv, T->rgb(44, 44, 52));
    g.c_gap = T->canvas_color(g.cv, T->rgb(10, 10, 14));
    g.c_gap_lip = T->canvas_color(g.cv, T->rgb(34, 34, 42));
    g.c_cone = T->canvas_color(g.cv, T->rgb(246, 124, 36));
    g.c_cone2 = T->canvas_color(g.cv, T->rgb(255, 236, 210));
    g.c_bench = T->canvas_color(g.cv, T->rgb(128, 84, 46));
    g.c_bench2 = T->canvas_color(g.cv, T->rgb(174, 122, 70));
    g.c_rail = T->canvas_color(g.cv, T->rgb(150, 156, 172));
    g.c_rail_hi = T->canvas_color(g.cv, T->rgb(226, 232, 244));
    g.c_skater = T->canvas_color(g.cv, T->rgb(244, 72, 148));
    g.c_skater2 = T->canvas_color(g.cv, T->rgb(255, 214, 120));
    g.c_board = T->canvas_color(g.cv, T->rgb(90, 222, 216));
    g.c_shadow = T->canvas_color(g.cv, T->rgb(38, 38, 46));
    g.c_text = T->canvas_color(g.cv, T->rgb(248, 248, 255));
    g.c_dim = T->canvas_color(g.cv, T->rgb(124, 128, 146));
    g.c_accent = T->canvas_color(g.cv, T->rgb(255, 214, 61));
    g.c_danger = T->canvas_color(g.cv, T->rgb(255, 84, 92));
    g.c_panel = T->canvas_color(g.cv, T->rgb(14, 14, 22));

    g.seed = (uint32_t)T->now_us();
    T->save_get("best", &g.best, 0);
    start_run();
    T->log("ready, best %d", g.best);
}

static void sg_enter(void) { g.dirty = true; }

static void sg_update(float dt)
{
    if (!g.cv) return;
    if (dt > 1.0f / 30) dt = 1.0f / 30;

    const tat_input_t *in = T->input();
    const tat_gestures_t *ges = T->gestures();

    if (T->menu_is_open()) {
        switch (T->menu_update()) {
        case 0: T->menu_toggle_sound(); break;
        case 1:
            start_run();
            T->menu_close();
            break;
        }
        if (!T->menu_is_open()) g.dirty = true;
        return;
    }
    if (ges->swipe_left) {
        T->menu_open();
        return;
    }

    g.phase_t += dt;
    g.dirty = true;
    switch (g.phase) {
    case READY:
        if (ges->tap || (in->clicked & TAT_BTN_B)) {
            // However she is being held right now is "cruising", the same way the racer
            // takes its neutral: nobody holds a watch perfectly level.
            g.pitch_neutral = g.pitch;
            g.phase = SKATING;
            g.phase_t = 0;
            sfx_start();
        }
        break;
    case SKATING: step(in, dt); break;
    case BAIL:
        if (g.phase_t > 1.0f) {
            g.phase = SKATING;
            g.phase_t = 0;
        }
        break;
    case OVER:
        if (g.phase_t > 0.8f && (ges->tap || (in->clicked & TAT_BTN_B))) start_run();
        break;
    }
}

static void sg_draw(void)
{
    if (!g.cv) return;

    if (T->menu_is_open()) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", g.best);
        const tat_menu_row_t rows[] = {
            T->menu_sound_row(),
            {"NEW RUN", "GO", T->ui_color(TAT_UI_ACCENT)},
            {"BEST", buf, T->ui_color(TAT_UI_LABEL)},
        };
        T->menu_draw(rows, 3, "PAUSED");
        return;
    }
    if (!g.dirty) return;
    g.dirty = false;

    draw_lot();
    draw_skater();
    draw_hud();

    if (g.phase == READY) banner("TURN TO STEER", "TIP FORWARD, TAP TO OLLIE", g.c_board);
    else if (g.phase == BAIL) banner("BAILED", NULL, g.c_danger);
    else if (g.phase == OVER) {
        char sub[24];
        snprintf(sub, sizeof(sub), "SCORE %d", g.score);
        banner("RUN OVER", sub, g.c_danger);
    }

    T->canvas_present(g.cv);
}

static void sg_redraw(void) { g.dirty = true; }

static bool sg_keep_awake(void) { return g.phase == SKATING && !T->menu_is_open(); }

static void sg_unload(void)
{
    if (g.cv) T->canvas_destroy(g.cv);
    g.cv = NULL;
}

const tat_game_t tat_game = {
    .magic = TAT_GAME_MAGIC,
    .api_major = TAT_API_MAJOR,
    .api_minor = TAT_API_MINOR,
    .id = "skatergirlz",
    .name = "SKATER GIRLZ",
    .accent_r = 244, .accent_g = 72, .accent_b = 148,
    .assets = NULL,
    .asset_count = 0,
    .begin = sg_begin,
    .enter = sg_enter,
    .update = sg_update,
    .draw = sg_draw,
    .leave = NULL,
    .unload = sg_unload,
    .redraw = sg_redraw,
    .keep_awake = sg_keep_awake,
};
