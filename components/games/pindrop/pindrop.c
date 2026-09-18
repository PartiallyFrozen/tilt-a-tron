// PIN DROP - a ball falls through a field of pins and you tilt the watch to steer it into
// the one hole.
//
// The board stays put on the screen; gravity follows the real world. Turning the watch
// changes which way the ball falls across the pins, exactly like tilting a board in your
// hands. The rim is live: touch it and the ball is gone. That is what the whole game is
// about - the pins are always trying to throw you at the edge, and a round board means
// there is no safe corner to sit in. Later levels put bombs out on the field too.
//
// Written against tat_api.h alone - it includes nothing else from the console, which is
// what will let it become an installable file.
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

#define BALL_R 7.5f
#define PIN_R 3.0f
#define BOMB_R 6.5f

// How far the ball's centre can get before it is touching the rim. Everything that the
// ball can reach has to live inside this, or a level would kill you for something you
// were never allowed to touch.
#define DEATH_R (BOARD_R - BALL_R)
#define PIN_MAX_R (DEATH_R - PIN_R - BALL_R - 2.0f)
#define BOMB_MAX_R (DEATH_R - BOMB_R - BALL_R - 2.0f)

#define DROP_Y (CCENTRE - 70.5f)   // in at the top, with room before the edge
#define ENTRY_CLEAR_Y 58.0f        // nothing is placed in the lane the ball drops through

#define MAX_PINS 48
#define MAX_BOMBS 6
#define LIVES 3

#define GRAVITY 185.0f      // px/s^2 at one g
#define BOUNCE 0.45f        // how much speed a pin gives back
#define DRAG 0.35f          // 1/s, so a ball left alone settles instead of jittering forever
#define MAX_SPEED 260.0f

typedef struct {
    float x, y;
} Vec;

// A bomb is either parked or walking a circle around the middle of the board. The moving
// ones arrive later, and they are what stops a level from having one memorised route.
typedef struct {
    Vec p;
    float orbit_r, angle, speed;   // speed 0 = parked
} Bomb;

static struct {
    tat_canvas_t *cv;

    /* palette */
    uint8_t c_bg, c_face, c_face_dim, c_rim, c_rim2, c_pin, c_pin_hi, c_pin_sh;
    uint8_t c_hole, c_hole_rim, c_edge, c_edge_hot, c_bomb, c_bomb_hi, c_spark, c_track;
    uint8_t c_ball, c_ball_hi, c_text, c_dim, c_accent, c_go;

    /* level */
    int level, best_level;
    Vec pins[MAX_PINS];
    int pin_count;
    Vec hole;
    float hole_r;
    Bomb bombs[MAX_BOMBS];
    int bomb_count;

    /* ball */
    Vec p, v;
    int bounces;
    float run_t;

    /* play */
    enum { READY, PLAYING, SUNK, LOST, OVER } phase;
    int lives;
    float phase_t;
    Vec death_p;            /* where it went, for the little burst */
    const char *death_msg;
    Vec grav;               /* low-passed, in g */
    float flat_t;           /* how long the watch has been lying flat */
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

static float len_from_centre(Vec a)
{
    const float dx = a.x - CCENTRE, dy = a.y - CCENTRE;
    return sqrtf(dx * dx + dy * dy);
}

// ---------------------------------------------------------------- building a level

static int pins_for(int level)
{
    const int n = 9 + level * 2;
    return n > MAX_PINS ? MAX_PINS : n;
}

// None for the first couple of levels: learn the board and the edge first, then start
// having to go around things.
static int bombs_for(int level)
{
    if (level < 3) return 0;
    const int n = (level - 1) / 2;
    return n > MAX_BOMBS ? MAX_BOMBS : n;
}

static float hole_r_for(int level)
{
    // Starts generous and tightens, but never so small the ball cannot fit.
    const float r = 14.0f - level * 0.45f;
    return r < BALL_R + 3.0f ? BALL_R + 3.0f : r;
}

// Is this spot clear of the lane the ball drops through?
static bool off_the_entry(Vec c) { return !(c.y < ENTRY_CLEAR_Y && fabsf(c.x - CCENTRE) < 20.0f); }

static void build_level(int level)
{
    seed = (uint32_t)level * 2654435761u + 12345u;
    g.pin_count = 0;
    g.bomb_count = 0;
    g.hole_r = hole_r_for(level);

    // The hole goes in the lower half of the board, so the first drop has somewhere to
    // fall towards rather than sitting under the entry point.
    for (int tries = 0;; tries++) {
        const float a = frand(0.25f, 2.90f);            /* mostly the lower half */
        const float r = frand(BOARD_R * 0.35f, DEATH_R - g.hole_r - 4.0f);
        g.hole.x = CCENTRE + cosf(a) * r;
        g.hole.y = CCENTRE + sinf(a) * r;
        if (g.hole.y > CCENTRE - 20.0f || tries > 40) break;
    }
    // Bombs. Parked ones are placed clear of the hole and of each other; from level 5 some
    // of them walk a circle instead. Level 5 rather than later because a board whose edge
    // kills is already hard to survive: the escalation has to arrive while people are
    // still getting there.
    //
    // An orbit is allowed to cross the hole. The first version forbade it, on the grounds
    // that a blocked hole is an unwinnable level - but the board is only 190 across, and
    // the forbidden band around the hole swallowed the whole range of usable radii, so
    // every moving bomb was rejected and the level quietly came out one bomb short. It is
    // not unwinnable anyway: the bomb is moving, so it clears the hole in about a second
    // and going in behind it is the puzzle.
    const int want = bombs_for(level);
    const bool moving_allowed = level >= 5;
    for (int tries = 0; g.bomb_count < want && tries < 800; tries++) {
        // Last resort: a parked bomb always fits somewhere, so a level is never short.
        const bool moving = moving_allowed && (g.bomb_count % 2) == 1 && tries < 500;
        Bomb b = {{0, 0}, 0, 0, 0};

        if (moving) {
            b.orbit_r = frand(26.0f, 70.0f);
            b.angle = frand(0, 6.2831853f);
            b.speed = frand(0.35f, 0.75f) * (frand(0, 1) < 0.5f ? -1.0f : 1.0f);
            b.p.x = CCENTRE + cosf(b.angle) * b.orbit_r;
            b.p.y = CCENTRE + sinf(b.angle) * b.orbit_r;
        } else {
            const float a = frand(0, 6.2831853f);
            const float r = frand(BOARD_R * 0.28f, BOMB_MAX_R);
            b.p.x = CCENTRE + cosf(a) * r;
            b.p.y = CCENTRE + sinf(a) * r;
            const float apart = g.hole_r + BOMB_R + 16.0f;
            if (dist2(b.p, g.hole) < apart * apart) continue;
            if (!off_the_entry(b.p)) continue;
        }

        bool clear = true;
        for (int i = 0; i < g.bomb_count && clear; i++) {
            const float apart = BOMB_R * 2 + BALL_R * 2 + 6.0f;
            if (dist2(b.p, g.bombs[i].p) < apart * apart) clear = false;
        }
        if (!clear) continue;
        g.bombs[g.bomb_count++] = b;
    }

    // Pins scattered with enough room between them for the ball to pass, kept clear of the
    // hole so a level is always winnable, and clear of the bombs and their orbits so the
    // ball is never knocked into one it could not see coming.
    const float min_gap = BALL_R * 2 + PIN_R * 2 + 3.0f;
    for (int tries = 0; g.pin_count < pins_for(level) && tries < 4000; tries++) {
        const float a = frand(0, 6.2831853f);
        const float r = frand(14.0f, PIN_MAX_R);
        Vec c = {CCENTRE + cosf(a) * r, CCENTRE + sinf(a) * r};

        const float clearance = g.hole_r + PIN_R + BALL_R + 4.0f;
        if (dist2(c, g.hole) < clearance * clearance) continue;
        if (!off_the_entry(c)) continue;

        bool clear = true;
        for (int i = 0; i < g.bomb_count && clear; i++) {
            const Bomb *b = &g.bombs[i];
            if (b->speed != 0) {
                if (fabsf(r - b->orbit_r) < PIN_R + BOMB_R + 3.0f) clear = false;
            } else {
                const float apart = PIN_R + BOMB_R + BALL_R + 4.0f;
                if (dist2(c, b->p) < apart * apart) clear = false;
            }
        }
        for (int i = 0; i < g.pin_count && clear; i++)
            if (dist2(c, g.pins[i]) < min_gap * min_gap) clear = false;
        if (!clear) continue;

        g.pins[g.pin_count++] = c;
    }
}

static void start_level(int level);

static void drop_ball(void)
{
    // In at the top of the board, nudged a little so it does not fall dead straight.
    g.p.x = CCENTRE + frand(-5.0f, 5.0f);
    g.p.y = DROP_Y;
    g.v.x = frand(-8.0f, 8.0f);
    g.v.y = 12.0f;
    g.bounces = 0;
    g.run_t = 0;

    // Wind any orbiting bomb round to somewhere else first. Letting a ball drop straight
    // onto one is a death nobody could have avoided, and this is cheaper than forbidding
    // every orbit that passes near the entry - which is most of them.
    const float clear_r = BOMB_R + BALL_R + 20.0f;
    for (int i = 0; i < g.bomb_count; i++) {
        Bomb *b = &g.bombs[i];
        if (b->speed == 0) continue;
        for (int turn = 0; turn < 10 && dist2(b->p, g.p) < clear_r * clear_r; turn++) {
            b->angle += 0.9f;
            b->p.x = CCENTRE + cosf(b->angle) * b->orbit_r;
            b->p.y = CCENTRE + sinf(b->angle) * b->orbit_r;
        }
    }
}

static void start_run(int level)
{
    g.lives = LIVES;
    start_level(level);
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

static void zap(void)   /* the rim */
{
    tat_tone_t a = {900, 180, 130, TAT_SQUARE, 0.65f, 0};
    tat_tone_t b = {220, 70, 200, TAT_NOISE, 0.5f, 90};
    T->tone(&a);
    T->tone(&b);
}

static void boom(void)  /* a bomb */
{
    tat_tone_t a = {260, 40, 300, TAT_NOISE, 0.75f, 0};
    tat_tone_t b = {160, 50, 260, TAT_TRIANGLE, 0.55f, 40};
    T->tone(&a);
    T->tone(&b);
}

static void plunk(void)
{
    tat_tone_t a = {700, 1180, 90, TAT_TRIANGLE, 0.7f, 0};
    tat_tone_t b = {1180, 1500, 120, TAT_TRIANGLE, 0.6f, 80};
    T->tone(&a);
    T->tone(&b);
}

// ---------------------------------------------------------------- physics

// Off the outside of something round - a pin. The surface pushes the ball away from the
// centre, so that is the way the normal points.
static void bounce_off_pin(Vec centre, float radius, float restitution)
{
    float dx = g.p.x - centre.x, dy = g.p.y - centre.y;
    float d = sqrtf(dx * dx + dy * dy);
    if (d < 0.0001f) {
        dx = 0;
        dy = -1;
        d = 1;
    }
    const float nx = dx / d, ny = dy / d;
    g.p.x = centre.x + nx * radius;   /* never leave the ball inside the pin */
    g.p.y = centre.y + ny * radius;

    const float into = g.v.x * nx + g.v.y * ny;
    if (into >= 0) return;   /* already heading away */
    g.v.x -= (1.0f + restitution) * into * nx;
    g.v.y -= (1.0f + restitution) * into * ny;
}

static void lose_ball(const char *why)
{
    g.death_p = g.p;
    g.death_msg = why;
    g.lives--;
    g.phase = g.lives > 0 ? LOST : OVER;
    g.phase_t = 0;
    g.dirty = true;
}

static void move_bombs(float dt)
{
    for (int i = 0; i < g.bomb_count; i++) {
        Bomb *b = &g.bombs[i];
        if (b->speed == 0) continue;
        b->angle += b->speed * dt;
        b->p.x = CCENTRE + cosf(b->angle) * b->orbit_r;
        b->p.y = CCENTRE + sinf(b->angle) * b->orbit_r;
    }
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

    // Move in small steps: at speed the ball would otherwise pass straight through a pin -
    // or clean across the rim without ever being outside it - between one frame and the next.
    const int steps = (int)(sp * dt / (PIN_R * 0.6f)) + 1;
    const float h = dt / steps;
    for (int k = 0; k < steps; k++) {
        g.p.x += g.v.x * h;
        g.p.y += g.v.y * h;

        /* the rim is live */
        const float dx = g.p.x - CCENTRE, dy = g.p.y - CCENTRE;
        if (dx * dx + dy * dy > DEATH_R * DEATH_R) {
            zap();
            lose_ball("HIT THE EDGE");
            return;
        }

        for (int i = 0; i < g.bomb_count; i++) {
            const float reach = BOMB_R + BALL_R;
            if (dist2(g.p, g.bombs[i].p) < reach * reach) {
                boom();
                lose_ball("HIT A BOMB");
                return;
            }
        }

        for (int i = 0; i < g.pin_count; i++) {
            const float reach = PIN_R + BALL_R;
            if (dist2(g.p, g.pins[i]) < reach * reach) {
                const float before = sqrtf(g.v.x * g.v.x + g.v.y * g.v.y);
                bounce_off_pin(g.pins[i], reach, BOUNCE);
                if (before > 25.0f) {
                    g.bounces++;
                    tick(before);
                }
            }
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
    g.c_edge = T->canvas_color(g.cv, T->rgb(226, 74, 64));
    g.c_edge_hot = T->canvas_color(g.cv, T->rgb(255, 190, 80));
    g.c_pin = T->canvas_color(g.cv, T->rgb(150, 152, 160));
    g.c_pin_hi = T->canvas_color(g.cv, T->rgb(246, 248, 252));
    g.c_pin_sh = T->canvas_color(g.cv, T->rgb(92, 94, 104));
    g.c_hole = T->canvas_color(g.cv, T->rgb(14, 16, 24));
    g.c_hole_rim = T->canvas_color(g.cv, T->rgb(255, 205, 60));
    g.c_bomb = T->canvas_color(g.cv, T->rgb(32, 34, 44));
    g.c_bomb_hi = T->canvas_color(g.cv, T->rgb(96, 100, 116));
    g.c_spark = T->canvas_color(g.cv, T->rgb(255, 140, 40));
    g.c_track = T->canvas_color(g.cv, T->rgb(186, 172, 140));
    g.c_ball = T->canvas_color(g.cv, T->rgb(232, 46, 74));
    g.c_ball_hi = T->canvas_color(g.cv, T->rgb(255, 168, 180));
    g.c_text = T->canvas_color(g.cv, T->rgb(250, 250, 255));
    g.c_dim = T->canvas_color(g.cv, T->rgb(140, 146, 168));
    g.c_accent = T->canvas_color(g.cv, T->rgb(255, 205, 60));
    g.c_go = T->canvas_color(g.cv, T->rgb(60, 210, 120));

    g.best_level = 1;
    T->save_get("best", &g.best_level, 0);
    T->save_get("bounces", &g.best_bounces, 0);
    start_run(g.best_level);
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
            start_run(g.level >= g.best_level ? 1 : g.level + 1);
            T->menu_invalidate();
            break;
        case 1: T->menu_toggle_sound(); break;
        case 2:   /* RETRY */
            start_run(g.level);
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
        // The bombs keep moving while you look the board over, so what you plan for is
        // what you get.
        move_bombs(dt);
        g.dirty = true;
        if (ges->tap || (in->clicked & TAT_BTN_B) || g.phase_t > 2.5f) g.phase = PLAYING;
        break;

    case PLAYING:
        g.run_t += dt;
        move_bombs(dt);
        step_ball(dt);
        g.dirty = true;
        if (g.phase == PLAYING && (in->clicked & TAT_BTN_B)) drop_ball();   /* re-drop a sulky ball */
        break;

    case SUNK:
        if (g.phase_t > 1.4f || ges->tap || (in->clicked & TAT_BTN_B)) start_level(g.level + 1);
        break;

    case LOST:
        g.dirty = true;   /* the burst is animating */
        if (g.phase_t > 1.2f || ges->tap || (in->clicked & TAT_BTN_B)) {
            drop_ball();        /* same board, one ball fewer */
            g.phase = READY;
            g.phase_t = 0;
        }
        break;

    case OVER:
        g.dirty = true;
        if (g.phase_t > 0.8f && (ges->tap || (in->clicked & TAT_BTN_B))) start_run(1);
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

    // The live line, drawn where it actually bites: the ball dies when its CENTRE crosses
    // this, so this is the ring to show rather than the pretty outer bezel. It lights up
    // as the ball closes in, which is the only warning the player gets.
    const float near = DEATH_R - len_from_centre(g.p);
    const bool hot = g.phase == PLAYING && near < 16.0f;
    T->canvas_fill_circle(g.cv, c, c, (int)DEATH_R + 3, hot ? g.c_edge_hot : g.c_edge);
    T->canvas_fill_circle(g.cv, c, c, (int)DEATH_R, g.c_face);
    T->canvas_fill_circle(g.cv, c, c, (int)DEATH_R - 2, g.c_face_dim);
    T->canvas_fill_circle(g.cv, c, c, (int)DEATH_R - 4, g.c_face);

    /* the hole: a dark well with a lit lip, so it reads as somewhere to fall into */
    T->canvas_fill_circle(g.cv, (int)g.hole.x, (int)g.hole.y, (int)g.hole_r + 1, g.c_hole_rim);
    T->canvas_fill_circle(g.cv, (int)g.hole.x, (int)g.hole.y, (int)g.hole_r, g.c_hole);

    for (int i = 0; i < g.pin_count; i++) {
        const int x = (int)g.pins[i].x, y = (int)g.pins[i].y;
        T->canvas_fill_circle(g.cv, x, y + 1, (int)PIN_R, g.c_pin_sh);
        T->canvas_fill_circle(g.cv, x, y, (int)PIN_R, g.c_pin);
        T->canvas_pixel(g.cv, x - 1, y - 1, g.c_pin_hi);
    }

    /* bombs last, so they sit on top of everything they could be confused with */
    for (int i = 0; i < g.bomb_count; i++) {
        const Bomb *b = &g.bombs[i];
        const int x = (int)b->p.x, y = (int)b->p.y;
        // The track it walks. Worth drawing clearly rather than faintly: a bomb that
        // sweeps a line you cannot see is just an ambush, and the whole point is timing
        // your run through the gap behind it.
        if (b->speed != 0) {
            for (int a = 0; a < 360; a += 9) {
                const float r0 = (a * 3.14159265f) / 180.0f;
                const int tx = c + (int)(cosf(r0) * b->orbit_r);
                const int ty = c + (int)(sinf(r0) * b->orbit_r);
                T->canvas_pixel(g.cv, tx, ty, g.c_track);
                T->canvas_pixel(g.cv, tx + 1, ty, g.c_track);
            }
        }
        T->canvas_fill_circle(g.cv, x, y, (int)BOMB_R, g.c_bomb);
        T->canvas_fill_circle(g.cv, x - 2, y - 2, 2, g.c_bomb_hi);
        T->canvas_fill_circle(g.cv, x + 2, y - (int)BOMB_R, 2, g.c_spark);   /* the fuse */
    }
}

static void draw_hud(void)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "LVL %d", g.level);
    T->canvas_text_centered(g.cv, CW / 2, 11, buf, g.c_text, 1, true);

    /* balls left, as balls */
    for (int i = 0; i < LIVES; i++) {
        const int x = CW / 2 - (LIVES - 1) * 6 + i * 12;
        T->canvas_fill_circle(g.cv, x, 24, 4, i < g.lives ? g.c_ball : g.c_face_dim);
    }

    const char *plural = g.bounces == 1 ? "" : "S";
    if (g.best_bounces) snprintf(buf, sizeof(buf), "%d PING%s   BEST %d", g.bounces, plural, g.best_bounces);
    else snprintf(buf, sizeof(buf), "%d PING%s", g.bounces, plural);
    T->canvas_text_centered(g.cv, CW / 2, CW - 12, buf, g.c_dim, 1, false);
}

static void banner(const char *top, const char *sub, uint8_t colour)
{
    const tat_banner_t b = {
        .top = top, .mid = sub,
        .top_color = colour, .mid_color = g.c_text,
        .panel = g.c_hole, .border = colour,
        .bars = true,
    };
    T->canvas_banner(g.cv, CW / 2, (int)CCENTRE - (sub ? 13 : 8), CW - 52, &b);
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

    if (g.phase == LOST || g.phase == OVER) {
        // A ring going out from where it went, so you can see what caught you.
        const int r = 6 + (int)(g.phase_t * 90.0f);
        if (r < 40) {
            T->canvas_fill_circle(g.cv, (int)g.death_p.x, (int)g.death_p.y, r, g.c_edge_hot);
            T->canvas_fill_circle(g.cv, (int)g.death_p.x, (int)g.death_p.y, r - 3, g.c_face);
        }
    } else {
        /* the ball, with a highlight so it looks round rather than flat */
        T->canvas_fill_circle(g.cv, (int)g.p.x, (int)g.p.y, (int)BALL_R, g.c_ball);
        T->canvas_fill_circle(g.cv, (int)(g.p.x - 1.2f), (int)(g.p.y - 1.4f), 1, g.c_ball_hi);
    }

    draw_hud();

    if (g.phase == READY) banner("KEEP OFF THE EDGE", "TILT TO STEER", g.c_accent);
    else if (g.phase == SUNK) {
        char sub[24];
        snprintf(sub, sizeof(sub), "%d PING%s", g.bounces, g.bounces == 1 ? "" : "S");
        banner("IN!", sub, g.c_go);
    } else if (g.phase == LOST) {
        char sub[24];
        snprintf(sub, sizeof(sub), "%d BALL%s LEFT", g.lives, g.lives == 1 ? "" : "S");
        banner(g.death_msg, sub, g.c_edge_hot);
    } else if (g.phase == OVER) {
        char sub[24];
        snprintf(sub, sizeof(sub), "REACHED LEVEL %d", g.level);
        banner("OUT OF BALLS", sub, g.c_ball);
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
    .accent_r = 228, .accent_g = 62, .accent_b = 96,   /* the ball's red */
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
