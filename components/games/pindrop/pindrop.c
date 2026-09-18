// PIN DROP - a ball falls through a field of pins and you tilt the watch to steer it
// into the one hole.
//
// The board stays put on the screen; gravity follows the real world. Turning the watch
// changes which way the ball falls across the pins, exactly like tilting a board in your
// hands. Because the board is round the ball never gets stranded at the bottom: roll the
// watch and it sets off again, so a miss costs time rather than the game.
//
// This is the first game written against tat_api.h alone - it includes nothing else from
// the console, which is what will let it become an installable file.
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "tat/tat_api.h"

static const tat_api_t *T;

// ---------------------------------------------------------------- the board

// Everything below is in canvas pixels: a 233 x 233 picture the console doubles onto the
// round screen.
#define CW 233
#define CCENTRE 116.5f
#define BOARD_R 95.0f    // the playfield. The round screen only shows a circle of
                         // radius ~116 about the centre, so this leaves a readable
                         // band top and bottom for the HUD - the canvas corners
                         // are never visible at all.

#define BALL_R 4.5f
#define PIN_R 3.0f
#define MAX_PINS 64

#define GRAVITY 300.0f      // px/s^2 at one g
#define BOUNCE 0.52f        // how much speed a pin gives back
#define RIM_BOUNCE 0.40f
#define DRAG 0.35f          // 1/s, so a ball left alone settles instead of jittering forever
#define MAX_SPEED 260.0f

typedef struct {
    float x, y;
} Vec;

static struct {
    tat_canvas_t *cv;

    /* palette */
    uint8_t c_bg, c_face, c_face_dim, c_rim, c_rim2, c_pin, c_pin_hi, c_pin_sh;
    uint8_t c_hole, c_hole_rim, c_ball, c_ball_hi, c_text, c_dim, c_accent, c_go;

    /* level */
    int level, best_level;
    Vec pins[MAX_PINS];
    int pin_count;
    Vec hole;
    float hole_r;

    /* ball */
    Vec p, v;
    int bounces;
    float run_t;

    /* play */
    enum { READY, PLAYING, SUNK } phase;
    float phase_t;
    Vec grav;          /* low-passed, in g */
    float flat_t;      /* how long the watch has been lying flat */
    int best_bounces;
    bool dirty;
} g;

// ---------------------------------------------------------------- odds and ends

static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// A level's layout comes from its number, so everyone playing level 7 plays the same
// board. Small, fast, and good enough for scattering pins.
static uint32_t seed;
static uint32_t next_rand(void)
{
    seed = seed * 1664525u + 1013904223u;
    return seed >> 8;
}
static float frand(float lo, float hi) { return lo + (hi - lo) * ((next_rand() & 0xFFFF) / 65535.0f); }

static float dist2(Vec a, Vec b)
{
    const float dx = a.x - b.x, dy = a.y - b.y;
    return dx * dx + dy * dy;
}

// ---------------------------------------------------------------- building a level

static int pins_for(int level)
{
    const int n = 14 + level * 3;
    return n > MAX_PINS ? MAX_PINS : n;
}

static float hole_r_for(int level)
{
    // Starts generous and tightens, but never so small the ball cannot fit.
    const float r = 13.0f - level * 0.45f;
    return r < BALL_R + 2.5f ? BALL_R + 2.5f : r;
}

static void build_level(int level)
{
    seed = (uint32_t)level * 2654435761u + 12345u;
    g.pin_count = pins_for(level);
    g.hole_r = hole_r_for(level);

    // The hole goes in the lower half of the board, so the first drop has somewhere to
    // fall towards rather than sitting under the entry point.
    for (int tries = 0;; tries++) {
        const float a = frand(0.25f, 2.90f);            /* mostly the lower half */
        const float r = frand(BOARD_R * 0.35f, BOARD_R - g.hole_r - 6.0f);
        g.hole.x = CCENTRE + cosf(a) * r;
        g.hole.y = CCENTRE + sinf(a) * r;
        if (g.hole.y > CCENTRE - 20.0f || tries > 40) break;
    }

    // Pins scattered with enough room between them for the ball to pass, and kept clear
    // of the hole so a level is always winnable.
    const float min_gap = BALL_R * 2 + PIN_R * 2 + 3.0f;
    int placed = 0;
    for (int tries = 0; placed < g.pin_count && tries < 4000; tries++) {
        const float a = frand(0, 6.2831853f);
        const float r = frand(14.0f, BOARD_R - PIN_R - 7.0f);
        Vec c = {CCENTRE + cosf(a) * r, CCENTRE + sinf(a) * r};

        if (dist2(c, g.hole) < (g.hole_r + PIN_R + BALL_R + 4.0f) * (g.hole_r + PIN_R + BALL_R + 4.0f)) continue;
        /* keep the drop point clear so the ball always gets moving */
        if (c.y < 30.0f && fabsf(c.x - CCENTRE) < 14.0f) continue;

        bool clear = true;
        for (int i = 0; i < placed && clear; i++)
            if (dist2(c, g.pins[i]) < min_gap * min_gap) clear = false;
        if (!clear) continue;

        g.pins[placed++] = c;
    }
    g.pin_count = placed;
}

static void drop_ball(void)
{
    // In at the top of the board, nudged a little so it does not fall dead straight.
    g.p.x = CCENTRE + frand(-5.0f, 5.0f);
    g.p.y = 18.0f;
    g.v.x = frand(-8.0f, 8.0f);
    g.v.y = 12.0f;
    g.bounces = 0;
    g.run_t = 0;
}

static void start_level(int level)
{
    g.level = level < 1 ? 1 : level;
    build_level(g.level);
    drop_ball();
    g.phase = READY;
    g.phase_t = 0;
    g.dirty = true;
}

// ---------------------------------------------------------------- sound

static void tick(float speed)
{
    if (T->volume() == 0) return;
    // Pitch follows the knock, so a hard bounce sounds like one.
    const float f = 420.0f + clampf(speed, 0, 200.0f) * 3.4f;
    tat_tone_t t = {f, f * 0.82f, 26, TAT_SQUARE, 0.35f, 0};
    T->tone(&t);
}

static void plunk(void)
{
    tat_tone_t a = {700, 1180, 90, TAT_TRIANGLE, 0.7f, 0};
    tat_tone_t b = {1180, 1500, 120, TAT_TRIANGLE, 0.6f, 80};
    T->tone(&a);
    T->tone(&b);
}

// ---------------------------------------------------------------- physics

static void bounce_off(Vec centre, float radius, float restitution)
{
    float dx = g.p.x - centre.x, dy = g.p.y - centre.y;
    float d = sqrtf(dx * dx + dy * dy);
    if (d < 0.0001f) {
        dx = 0;
        dy = -1;
        d = 1;
    }
    const float nx = dx / d, ny = dy / d;

    /* lift the ball clear so it cannot end up inside the pin */
    g.p.x = centre.x + nx * radius;
    g.p.y = centre.y + ny * radius;

    const float into = g.v.x * nx + g.v.y * ny;
    if (into >= 0) return;   /* already moving away */
    g.v.x -= (1.0f + restitution) * into * nx;
    g.v.y -= (1.0f + restitution) * into * ny;
}

static void step_ball(float dt)
{
    g.v.x += g.grav.x * GRAVITY * dt;
    g.v.y += g.grav.y * GRAVITY * dt;

    const float damp = 1.0f - DRAG * dt;
    g.v.x *= damp;
    g.v.y *= damp;

    const float sp = sqrtf(g.v.x * g.v.x + g.v.y * g.v.y);
    if (sp > MAX_SPEED) {
        g.v.x *= MAX_SPEED / sp;
        g.v.y *= MAX_SPEED / sp;
    }

    // Move in small steps: at speed the ball would otherwise pass straight through a pin
    // between one frame and the next.
    const int steps = (int)(sp * dt / (PIN_R * 0.6f)) + 1;
    const float h = dt / steps;
    for (int k = 0; k < steps; k++) {
        g.p.x += g.v.x * h;
        g.p.y += g.v.y * h;

        for (int i = 0; i < g.pin_count; i++) {
            const float reach = PIN_R + BALL_R;
            if (dist2(g.p, g.pins[i]) < reach * reach) {
                const float before = sqrtf(g.v.x * g.v.x + g.v.y * g.v.y);
                bounce_off(g.pins[i], reach, BOUNCE);
                if (before > 25.0f) {
                    g.bounces++;
                    tick(before);
                }
            }
        }

        /* the rim of the board */
        const float dx = g.p.x - CCENTRE, dy = g.p.y - CCENTRE;
        const float rr = BOARD_R - BALL_R;
        if (dx * dx + dy * dy > rr * rr) {
            Vec c = {CCENTRE, CCENTRE};
            bounce_off(c, rr, RIM_BOUNCE);
        }

        /* down the hole: the ball has to be properly over it, not just clipping the edge */
        const float lip = g.hole_r - BALL_R * 0.55f;
        if (lip > 0 && dist2(g.p, g.hole) < lip * lip) {
            g.phase = SUNK;
            g.phase_t = 0;
            plunk();
            if (g.level >= g.best_level) {
                g.best_level = g.level + 1;
                T->save_set("best", g.best_level);
            }
            if (!g.best_bounces || g.bounces < g.best_bounces) {
                g.best_bounces = g.bounces;
                T->save_set("bounces", g.best_bounces);
            }
            return;
        }
    }
}

// ---------------------------------------------------------------- the game

static void pd_begin(const tat_api_t *api)
{
    T = api;
    memset(&g, 0, sizeof(g));
    g.cv = T->canvas_create(2, 0);
    if (!g.cv) {
        T->log("no memory for the board");
        return;
    }

    g.c_bg = T->canvas_color(g.cv, T->rgb(8, 10, 16));
    g.c_face = T->canvas_color(g.cv, T->rgb(238, 228, 200));
    g.c_face_dim = T->canvas_color(g.cv, T->rgb(214, 202, 172));
    g.c_rim = T->canvas_color(g.cv, T->rgb(228, 62, 96));
    g.c_rim2 = T->canvas_color(g.cv, T->rgb(60, 168, 226));
    g.c_pin = T->canvas_color(g.cv, T->rgb(150, 152, 160));
    g.c_pin_hi = T->canvas_color(g.cv, T->rgb(246, 248, 252));
    g.c_pin_sh = T->canvas_color(g.cv, T->rgb(92, 94, 104));
    g.c_hole = T->canvas_color(g.cv, T->rgb(14, 16, 24));
    g.c_hole_rim = T->canvas_color(g.cv, T->rgb(255, 205, 60));
    g.c_ball = T->canvas_color(g.cv, T->rgb(232, 46, 74));
    g.c_ball_hi = T->canvas_color(g.cv, T->rgb(255, 168, 180));
    g.c_text = T->canvas_color(g.cv, T->rgb(250, 250, 255));
    g.c_dim = T->canvas_color(g.cv, T->rgb(140, 146, 168));
    g.c_accent = T->canvas_color(g.cv, T->rgb(255, 205, 60));
    g.c_go = T->canvas_color(g.cv, T->rgb(60, 210, 120));

    g.best_level = 1;
    T->save_get("best", &g.best_level, 0);
    T->save_get("bounces", &g.best_bounces, 0);
    start_level(g.best_level);
}

static void pd_enter(void)
{
    g.dirty = true;
    g.flat_t = 0;
}

static void pd_update(float dt)
{
    const tat_input_t *in = T->input();
    const tat_gestures_t *ges = T->gestures();

    if (T->menu_is_open()) {
        switch (T->menu_update()) {
        case 0:   /* LEVEL: step through the ones reached */
            start_level(g.level >= g.best_level ? 1 : g.level + 1);
            T->menu_invalidate();
            break;
        case 1: T->menu_toggle_sound(); break;
        case 2:   /* RETRY */
            start_level(g.level);
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

    // Gravity, smoothed so a shaky hand does not rattle the ball. Lying flat there is no
    // "down" to read, so hold the last one and say so.
    const float k = 1.0f - expf(-dt / 0.09f);
    g.grav.x += (in->tilt.ax - g.grav.x) * k;
    g.grav.y += (in->tilt.ay - g.grav.y) * k;
    const float upright = sqrtf(g.grav.x * g.grav.x + g.grav.y * g.grav.y);
    g.flat_t = upright < 0.18f ? g.flat_t + dt : 0;

    g.phase_t += dt;
    switch (g.phase) {
    case READY:
        if (ges->tap || (in->clicked & TAT_BTN_B) || g.phase_t > 2.5f) {
            g.phase = PLAYING;
            g.dirty = true;
        }
        break;

    case PLAYING:
        g.run_t += dt;
        step_ball(dt);
        g.dirty = true;
        if (in->clicked & TAT_BTN_B) {   /* PWR re-drops a ball that has gone sulky */
            drop_ball();
        }
        break;

    case SUNK:
        if (g.phase_t > 1.4f || ges->tap || (in->clicked & TAT_BTN_B)) start_level(g.level + 1);
        break;
    }
}

// ---------------------------------------------------------------- drawing

static void draw_board(void)
{
    const int c = (int)CCENTRE;
    T->canvas_clear(g.cv, g.c_bg);

    /* the face, with a two-tone rim so the board reads as a real object */
    T->canvas_fill_circle(g.cv, c, c, (int)BOARD_R + 6, g.c_rim);
    for (int a = 0; a < 360; a += 30) {
        const float r0 = (a * 3.14159265f) / 180.0f;
        const int x = c + (int)(cosf(r0) * (BOARD_R + 3));
        const int y = c + (int)(sinf(r0) * (BOARD_R + 3));
        T->canvas_fill_circle(g.cv, x, y, 4, g.c_rim2);
    }
    T->canvas_fill_circle(g.cv, c, c, (int)BOARD_R, g.c_face);
    T->canvas_fill_circle(g.cv, c, c, (int)BOARD_R - 2, g.c_face_dim);
    T->canvas_fill_circle(g.cv, c, c, (int)BOARD_R - 4, g.c_face);

    /* the hole: a dark well with a lit lip, so it reads as somewhere to fall into */
    T->canvas_fill_circle(g.cv, (int)g.hole.x, (int)g.hole.y, (int)g.hole_r + 1, g.c_hole_rim);
    T->canvas_fill_circle(g.cv, (int)g.hole.x, (int)g.hole.y, (int)g.hole_r, g.c_hole);

    for (int i = 0; i < g.pin_count; i++) {
        const int x = (int)g.pins[i].x, y = (int)g.pins[i].y;
        T->canvas_fill_circle(g.cv, x, y + 1, (int)PIN_R, g.c_pin_sh);
        T->canvas_fill_circle(g.cv, x, y, (int)PIN_R, g.c_pin);
        T->canvas_pixel(g.cv, x - 1, y - 1, g.c_pin_hi);
    }
}

static void draw_hud(void)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "LVL %d", g.level);
    T->canvas_text_centered(g.cv, CW / 2, 11, buf, g.c_text, 1, true);

    const char *plural = g.bounces == 1 ? "" : "S";
    if (g.best_bounces) snprintf(buf, sizeof(buf), "%d PING%s   BEST %d", g.bounces, plural, g.best_bounces);
    else snprintf(buf, sizeof(buf), "%d PING%s", g.bounces, plural);
    T->canvas_text_centered(g.cv, CW / 2, CW - 12, buf, g.c_dim, 1, false);
}

static void banner(const char *top, const char *sub, uint8_t colour)
{
    const int h = sub ? 30 : 20;
    const int y = (int)CCENTRE - h / 2;
    T->canvas_fill_rect(g.cv, 26, y, CW - 52, h, g.c_hole);
    T->canvas_fill_rect(g.cv, 26, y, CW - 52, 2, colour);
    T->canvas_fill_rect(g.cv, 26, y + h - 2, CW - 52, 2, colour);
    T->canvas_text_centered(g.cv, CW / 2, y + 9, top, colour, 1, true);
    if (sub) T->canvas_text_centered(g.cv, CW / 2, y + 21, sub, g.c_text, 1, false);
}

static void pd_draw(void)
{
    if (!g.cv) return;

    if (T->menu_is_open()) {
        char lvl[16], bnc[16];
        snprintf(lvl, sizeof(lvl), "%d / %d", g.level, g.best_level);
        snprintf(bnc, sizeof(bnc), "%d", g.best_bounces);
        const tat_menu_row_t rows[] = {
            {"LEVEL", lvl, 0},
            T->menu_sound_row(),
            {"RETRY", "GO", T->ui_color(TAT_UI_ACCENT)},
            {"FEWEST PINGS", g.best_bounces ? bnc : "-", T->ui_color(TAT_UI_LABEL)},
        };
        T->menu_draw(rows, 4, "PAUSED");
        return;
    }

    if (!g.dirty) return;
    g.dirty = false;

    draw_board();

    /* the ball, with a highlight so it looks round rather than flat */
    T->canvas_fill_circle(g.cv, (int)g.p.x, (int)g.p.y, (int)BALL_R, g.c_ball);
    T->canvas_fill_circle(g.cv, (int)(g.p.x - 1.2f), (int)(g.p.y - 1.4f), 1, g.c_ball_hi);

    draw_hud();

    if (g.phase == READY) banner("TILT TO STEER", "TAP TO DROP", g.c_accent);
    else if (g.phase == SUNK) {
        char sub[24];
        snprintf(sub, sizeof(sub), "%d PING%s", g.bounces, g.bounces == 1 ? "" : "S");
        banner("IN!", sub, g.c_go);
    } else if (g.flat_t > 1.2f) {
        T->canvas_text_centered(g.cv, CW / 2, (int)CCENTRE + 70, "TURN THE WATCH", g.c_accent, 1, true);
    }

    T->canvas_present(g.cv);
}

static void pd_redraw(void) { g.dirty = true; }

static bool pd_keep_awake(void) { return g.phase == PLAYING && !T->menu_is_open(); }

const tat_game_t tat_game = {
    .magic = TAT_GAME_MAGIC,
    .api_major = TAT_API_MAJOR,
    .api_minor = TAT_API_MINOR,
    .id = "pindrop",
    .name = "PIN DROP",
    .accent = 0xE0C4,   /* the ball's red, in the panel's packing */
    .assets = NULL,
    .asset_count = 0,
    .begin = pd_begin,
    .enter = pd_enter,
    .update = pd_update,
    .draw = pd_draw,
    .leave = NULL,
    .unload = NULL,
    .keep_awake = pd_keep_awake,
    .redraw = pd_redraw,
};
