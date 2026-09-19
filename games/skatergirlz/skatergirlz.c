// SKATER GIRLZ - a side-on skate run that never ends.
//
// She rides left to right across a rooftop street. Tip the watch forward to push faster
// and back to slow down, tap to ollie, and land on a rail from above to grind it. Miss a
// gap and the run is over; clip a bin and you lose your speed, which costs you more than
// it sounds like.
//
//   tip forward - push; tip back to slow
//   tap         - ollie; hold for more air
//   swipe left  - pause menu
//
// Side on, not from above. The first version of this was top-down, which was my
// misreading of the handoff: its speed model is named after Canabalt, and its gaps, rails
// and grinds are all side-scroller ideas. A skate run reads as a skate run from the side.
//
// Written against tat_api.h alone. Built as a package with tools/mktat.py.
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "tat/tat_api.h"

static const tat_api_t *T;

#define SCALE 2
#define CW ((TAT_SCREEN + SCALE - 1) / SCALE)   /* 233 */
#define PI 3.14159265f

// Where things sit on the canvas. She rides a third of the way across, so there is room
// to read what is coming without losing sight of what is underneath her. The round screen
// cuts the corners off, so the ground sits high enough that the drop below it still shows.
#define SKATER_X 74
#define GROUND_Y 148          /* canvas y of street level */
#define FALL_LIMIT (-52.0f)   /* below this, down a gap, the run is over */

#define MAX_SLABS 18
#define MAX_RAILS 6
#define LIVES 3

#define MAX_SPEED 108.0f      /* world units a second */
#define MIN_SPEED 26.0f       /* she never quite stops; a stopped skater is not a game */
#define ACCEL 64.0f
#define GRAVITY 420.0f
#define OLLIE_V 132.0f
#define OLLIE_HOLD_V 196.0f
#define RAIL_SNAP 5.0f        /* how close to the top counts as landing on it */

typedef struct {
    float x0, x1;   /* world span */
    float top;      /* height of the surface above street level */
    bool cone;      /* a bin sitting on it, near the right-hand end */
    float cone_x;
} Slab;

typedef struct {
    float x0, x1, top;
    bool used;      /* scored already, so one rail pays once per grind */
} Rail;

typedef enum { READY, SKATING, OVER } Phase;

static struct {
    tat_canvas_t *cv;

    /* palette */
    uint8_t c_sky[4], c_far, c_near, c_street, c_street2, c_kerb, c_void;
    uint8_t c_rail, c_rail_leg, c_cone, c_cone2;
    uint8_t c_skin, c_hair, c_shirt, c_jeans, c_board, c_wheel, c_shadow;
    uint8_t c_text, c_dim, c_accent, c_danger, c_panel;

    /* the run */
    Phase phase;
    float phase_t;
    float x;              /* how far along she is */
    float y, vy;          /* height above street level */
    float speed;
    bool airborne;
    bool grinding;
    float air_t;          /* how long this jump has lasted, for the style points */
    int score, bonus, best;
    int lives;
    int combo;

    float grav_x, grav_y, grav_z;
    float pitch, pitch_neutral;

    Slab slabs[MAX_SLABS];
    int n_slabs;
    Rail rails[MAX_RAILS];
    int n_rails;
    float built_to;       /* world x that the street has been laid up to */

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

/* world x to canvas x: she stays put and the street moves past her */
static int sx_of(float wx) { return SKATER_X + (int)(wx - g.x); }
/* height above street level to canvas y */
static int sy_of(float h) { return GROUND_Y - (int)h; }

// ---------------------------------------------------------------- sound

static void tone1(float f0, float f1, uint16_t ms, uint8_t wave, float vol, uint16_t delay)
{
    const tat_tone_t t = {f0, f1, ms, wave, vol, delay};
    T->tone(&t);
}

static void sfx_ollie(void) { tone1(260, 640, 70, TAT_TRIANGLE, 0.45f, 0); }
static void sfx_land(void) { tone1(150, 90, 55, TAT_NOISE, 0.4f, 0); }
static void sfx_grind(void) { tone1(140, 200, 150, TAT_NOISE, 0.32f, 0); }
static void sfx_clip(void) { tone1(210, 80, 120, TAT_NOISE, 0.6f, 0); }
static void sfx_start(void) { tone1(440, 880, 110, TAT_TRIANGLE, 0.6f, 0); }

static void sfx_fall(void)
{
    tone1(700, 90, 420, TAT_TRIANGLE, 0.6f, 0);
    tone1(200, 60, 240, TAT_NOISE, 0.5f, 220);
}

static void sfx_over(void)
{
    tone1(392, 0, 150, TAT_SQUARE, 0.6f, 0);
    tone1(330, 0, 150, TAT_SQUARE, 0.6f, 150);
    tone1(247, 0, 320, TAT_SQUARE, 0.6f, 300);
}

// ---------------------------------------------------------------- the street

// Lay another slab of street, sometimes after a gap, sometimes at a different height, and
// put a rail or a bin on it now and then. Everything is generated just ahead of what can
// be seen, so the run never repeats and never has to be stored.
static void extend(void)
{
    while (g.built_to < g.x + 320.0f) {
        if (g.n_slabs >= MAX_SLABS) {
            // Drop the oldest, which is long behind her by now.
            memmove(&g.slabs[0], &g.slabs[1], sizeof(Slab) * (MAX_SLABS - 1));
            g.n_slabs--;
        }
        const float r = frand(0, 1);
        // A gap you have to ollie. Kept inside what a held ollie clears at cruising speed,
        // which is about seventy units: anything wider is a death sentence, not a jump.
        float gap = 0;
        if (r < 0.30f && g.built_to > 260.0f) gap = frand(26.0f, 54.0f);

        const float len = frand(70.0f, 150.0f);
        float top = 0;
        if (frand(0, 1) < 0.26f) top = frand(12.0f, 26.0f);   /* a raised block */

        Slab *s = &g.slabs[g.n_slabs++];
        s->x0 = g.built_to + gap;
        s->x1 = s->x0 + len;
        s->top = top;
        s->cone = false;
        s->cone_x = 0;
        // A bin sits on flat street only, never on a block you have just had to hop onto.
        if (top == 0 && len > 95.0f && frand(0, 1) < 0.42f) {
            s->cone = true;
            s->cone_x = s->x0 + frand(40.0f, len - 20.0f);
        }
        g.built_to = s->x1;

        /* a rail floating over the middle of a long flat slab */
        if (top == 0 && len > 110.0f && frand(0, 1) < 0.55f) {
            if (g.n_rails >= MAX_RAILS) {
                memmove(&g.rails[0], &g.rails[1], sizeof(Rail) * (MAX_RAILS - 1));
                g.n_rails--;
            }
            Rail *rl = &g.rails[g.n_rails++];
            rl->x0 = s->x0 + frand(24.0f, 44.0f);
            rl->x1 = rl->x0 + frand(46.0f, 84.0f);
            if (rl->x1 > s->x1 - 14.0f) rl->x1 = s->x1 - 14.0f;
            rl->top = frand(17.0f, 25.0f);
            rl->used = false;
        }
    }
}

/* the height of the street under `wx`, or a miss if she is over a gap */
static bool ground_at(float wx, float *top)
{
    for (int i = 0; i < g.n_slabs; i++)
        if (wx >= g.slabs[i].x0 && wx <= g.slabs[i].x1) {
            *top = g.slabs[i].top;
            return true;
        }
    return false;
}

static Rail *rail_at(float wx)
{
    for (int i = 0; i < g.n_rails; i++)
        if (wx >= g.rails[i].x0 && wx <= g.rails[i].x1) return &g.rails[i];
    return NULL;
}

static void start_run(void)
{
    g.x = 0;
    g.y = 0;
    g.vy = 0;
    g.speed = MIN_SPEED;
    g.airborne = false;
    g.grinding = false;
    g.air_t = 0;
    g.score = g.bonus = 0;
    g.combo = 0;
    g.lives = LIVES;
    g.n_slabs = 0;
    g.n_rails = 0;
    g.built_to = -120.0f;
    // A long clear run-up: the first thing she meets should not be a gap.
    Slab *s = &g.slabs[g.n_slabs++];
    s->x0 = -120.0f;
    s->x1 = 260.0f;
    s->top = 0;
    s->cone = false;
    g.built_to = s->x1;
    extend();
    g.phase = READY;
    g.phase_t = 0;
    g.dirty = true;
}

static void wipe_out(bool fell)
{
    g.lives--;
    g.combo = 0;
    g.grinding = false;
    if (fell) sfx_fall();
    if (g.lives > 0) {
        // Put her back on the last solid ground rather than starting the street again:
        // losing a life should cost the run its flow, not its progress.
        float top = 0;
        float probe = g.x;
        for (int i = 0; i < 400 && !ground_at(probe, &top); i++) probe -= 4.0f;
        g.x = probe - 30.0f;
        ground_at(g.x, &top);
        g.y = top;
        g.vy = 0;
        g.airborne = false;
        g.speed = MIN_SPEED;
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
    // Tipping the watch away from you is the push. Gravity gives the angle, so there is
    // nothing to drift, and whatever angle she was being held at when the run started is
    // taken as cruising.
    const float k = 1.0f - expf(-dt / 0.12f);
    g.grav_x += (in->tilt.ax - g.grav_x) * k;
    g.grav_y += (in->tilt.ay - g.grav_y) * k;
    g.grav_z += (in->tilt.az - g.grav_z) * k;
    g.pitch = atan2f(g.grav_z, sqrtf(g.grav_x * g.grav_x + g.grav_y * g.grav_y));

    const float want = clampf(0.30f + (g.pitch - g.pitch_neutral) * 1.9f, 0.0f, 1.0f);
    const float target = MIN_SPEED + want * (MAX_SPEED - MIN_SPEED);
    g.speed += clampf(target - g.speed, -ACCEL * 1.4f * dt, ACCEL * dt);
    g.speed = clampf(g.speed, MIN_SPEED, MAX_SPEED);

    /* the ollie */
    if (in->touch.pressed && !g.airborne) {
        g.vy = OLLIE_V;
        g.airborne = true;
        g.grinding = false;
        g.air_t = 0;
        sfx_ollie();
    }
    // Keeping your finger down through the first part of the jump gets you higher, which
    // is how a wide gap or a high rail becomes reachable.
    if (in->touch.down && g.vy > 0 && g.vy < OLLIE_HOLD_V) g.vy += 620.0f * dt;

    g.x += g.speed * dt;

    float top = 0;
    const bool over_ground = ground_at(g.x, &top);
    Rail *rl = rail_at(g.x);

    if (g.airborne || g.grinding) {
        const float prev_y = g.y;
        if (!g.grinding) {
            g.vy -= GRAVITY * dt;
            g.y += g.vy * dt;
            g.air_t += dt;
        }

        /* coming down onto a rail from above is a grind */
        if (rl && g.vy <= 0 && prev_y >= rl->top - RAIL_SNAP && g.y <= rl->top + RAIL_SNAP) {
            g.y = rl->top;
            g.vy = 0;
            g.airborne = false;
            if (!g.grinding) {
                g.grinding = true;
                g.combo++;
                if (!rl->used) {
                    rl->used = true;
                    g.bonus += 60 * (g.combo < 5 ? g.combo : 5);
                }
                sfx_grind();
            }
        } else if (g.grinding && !rl) {
            /* run off the end of it */
            g.grinding = false;
            g.airborne = true;
            g.vy = 20.0f;
        } else if (!g.grinding && over_ground && g.vy <= 0 && g.y <= top) {
            g.y = top;
            g.vy = 0;
            g.airborne = false;
            // Style for a long one. A hop over a crack is not worth the same as clearing
            // a whole gap, and the air time is what tells them apart.
            if (g.air_t > 0.35f) g.bonus += (int)(g.air_t * 60.0f);
            sfx_land();
        } else if (!over_ground && g.y < FALL_LIMIT) {
            wipe_out(true);
            return;
        }
        if (g.grinding) g.bonus += (int)(90.0f * dt) * (g.combo < 5 ? g.combo : 5);
    } else {
        /* on the deck: follow the street, and walk off the edge of it */
        if (!over_ground) {
            g.airborne = true;
            g.vy = 0;
        } else if (g.y < top) {
            // Ran into the side of a raised block rather than landing on it: that costs
            // speed, the same as a bin, and she scrambles up.
            g.speed *= 0.5f;
            g.combo = 0;
            g.y = top;
            sfx_clip();
        } else {
            g.y = top;
        }
    }

    /* bins */
    for (int i = 0; i < g.n_slabs; i++) {
        Slab *s = &g.slabs[i];
        if (!s->cone) continue;
        if (fabsf(g.x - s->cone_x) < 5.0f && g.y < s->top + 12.0f) {
            s->cone = false;   /* knocked over; it does not catch you twice */
            g.speed *= 0.45f;
            g.combo = 0;
            sfx_clip();
        }
    }

    extend();

    // Worked out from the distance each frame rather than added to a frame at a time: a
    // fraction of a point sixty times a second casts to zero every time.
    g.score = (int)(g.x * 0.35f) + g.bonus;
}

// ---------------------------------------------------------------- drawing

static void draw_world(void)
{
    /* sky, four bands, lightest at the horizon */
    for (int i = 0; i < 4; i++)
        T->canvas_fill_rect(g.cv, 0, i * (GROUND_Y / 4), CW, GROUND_Y / 4 + 1, g.c_sky[i]);

    // Two rows of rooftops behind her, the far one drifting slower. It is what makes the
    // speed readable when the street underneath is plain.
    for (int layer = 0; layer < 2; layer++) {
        const float par = layer == 0 ? 0.18f : 0.42f;
        const uint8_t col = layer == 0 ? g.c_far : g.c_near;
        const float span = layer == 0 ? 54.0f : 38.0f;
        const float off = g.x * par;
        const float base = floorf(off / span) * span;
        for (int i = -1; i < CW / (int)span + 3; i++) {
            const float bx = base + i * span;
            const int sx = (int)(bx - off);
            /* a height that depends only on which building it is, so they stay put */
            const int idx = (int)(bx / span);
            const int h = 26 + ((idx * 37) % 40) + layer * 14;
            T->canvas_fill_rect(g.cv, sx, GROUND_Y - h, (int)span - 5, h, col);
        }
    }

    /* the drop below the street, seen through the gaps */
    T->canvas_fill_rect(g.cv, 0, GROUND_Y, CW, CW - GROUND_Y, g.c_void);

    for (int i = 0; i < g.n_slabs; i++) {
        const Slab *s = &g.slabs[i];
        const int x0 = sx_of(s->x0), x1 = sx_of(s->x1);
        if (x1 < -8 || x0 > CW + 8) continue;
        const int y = sy_of(s->top);
        T->canvas_fill_rect(g.cv, x0, y, x1 - x0, CW - y, g.c_street);
        T->canvas_fill_rect(g.cv, x0, y, x1 - x0, 2, g.c_kerb);          /* the lip */
        for (int d = x0 + 6; d < x1 - 4; d += 16)                        /* paving joints */
            T->canvas_fill_rect(g.cv, d, y + 5, 1, 6, g.c_street2);

        if (s->cone) {
            const int cx = sx_of(s->cone_x), cy = sy_of(s->top);
            T->canvas_fill_rect(g.cv, cx - 4, cy - 11, 8, 11, g.c_cone);
            T->canvas_fill_rect(g.cv, cx - 5, cy - 13, 10, 3, g.c_cone2);
        }
    }

    for (int i = 0; i < g.n_rails; i++) {
        const Rail *r = &g.rails[i];
        const int x0 = sx_of(r->x0), x1 = sx_of(r->x1);
        if (x1 < -8 || x0 > CW + 8) continue;
        const int y = sy_of(r->top);
        T->canvas_fill_rect(g.cv, x0, y, x1 - x0, 2, g.c_rail);
        T->canvas_fill_rect(g.cv, x0 + 1, y + 2, 2, GROUND_Y - y - 2, g.c_rail_leg);
        T->canvas_fill_rect(g.cv, x1 - 3, y + 2, 2, GROUND_Y - y - 2, g.c_rail_leg);
    }
}

static void draw_skater(void)
{
    const int x = SKATER_X;
    const int y = sy_of(g.y);   /* the ground under her feet */

    /* board */
    T->canvas_fill_rect(g.cv, x - 8, y - 3, 16, 2, g.c_board);
    T->canvas_fill_rect(g.cv, x - 6, y - 1, 2, 2, g.c_wheel);
    T->canvas_fill_rect(g.cv, x + 4, y - 1, 2, 2, g.c_wheel);

    // Crouched when she is in the air, upright when she is rolling: the whole read of an
    // ollie is the shape changing, not the height alone.
    const int crouch = g.airborne ? 3 : 0;
    const int hip = y - 5 - crouch;
    T->canvas_fill_rect(g.cv, x - 3, hip - 7, 6, 7, g.c_jeans);    /* legs */
    T->canvas_fill_rect(g.cv, x - 4, hip - 14, 8, 7, g.c_shirt);   /* body */
    T->canvas_fill_rect(g.cv, x - 3, hip - 19, 6, 5, g.c_skin);    /* head */
    T->canvas_fill_rect(g.cv, x - 5, hip - 20, 4, 7, g.c_hair);    /* hair, streaming back */
    if (g.airborne) T->canvas_fill_rect(g.cv, x + 3, hip - 13, 5, 2, g.c_skin);   /* arm out */

    if (g.grinding)
        for (int i = 0; i < 3; i++)
            T->canvas_pixel(g.cv, x - 8 + (int)frand(0, 5), y + (int)frand(0, 4), g.c_accent);
}

static void draw_hud(void)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", g.score);
    T->canvas_text_centered(g.cv, CW / 2, 14, buf, g.c_text, 1, true);
    for (int i = 0; i < LIVES; i++)
        T->canvas_fill_circle(g.cv, CW / 2 - (LIVES - 1) * 6 + i * 12, 27, 3,
                              i < g.lives ? g.c_shirt : g.c_dim);
    if (g.grinding && g.combo > 1) {
        snprintf(buf, sizeof(buf), "GRIND X%d", g.combo < 5 ? g.combo : 5);
        T->canvas_text_centered(g.cv, CW / 2, 44, buf, g.c_accent, 1, true);
    }
    if (g.best) {
        snprintf(buf, sizeof(buf), "BEST %d", g.best);
        T->canvas_text_centered(g.cv, CW / 2, CW - 14, buf, g.c_dim, 1, false);
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
    T->canvas_banner(g.cv, CW / 2, CW / 2 - (sub ? 13 : 8), CW - 54, &b);
}

// ---------------------------------------------------------------- the game

static void sg_begin(const tat_api_t *api)
{
    T = api;
    memset(&g, 0, sizeof(g));
    g.cv = T->canvas_create(SCALE, 0);
    if (!g.cv) {
        T->log("no memory for the street");
        return;
    }
    static const uint8_t SKY[4][3] = {{44, 32, 74}, {86, 52, 106}, {162, 84, 116}, {242, 146, 110}};
    for (int i = 0; i < 4; i++) g.c_sky[i] = T->canvas_color(g.cv, T->rgb(SKY[i][0], SKY[i][1], SKY[i][2]));
    g.c_far = T->canvas_color(g.cv, T->rgb(76, 58, 100));
    g.c_near = T->canvas_color(g.cv, T->rgb(52, 40, 74));
    g.c_street = T->canvas_color(g.cv, T->rgb(62, 62, 72));
    g.c_street2 = T->canvas_color(g.cv, T->rgb(48, 48, 58));
    g.c_kerb = T->canvas_color(g.cv, T->rgb(150, 150, 164));
    g.c_void = T->canvas_color(g.cv, T->rgb(14, 12, 22));
    g.c_rail = T->canvas_color(g.cv, T->rgb(226, 232, 244));
    g.c_rail_leg = T->canvas_color(g.cv, T->rgb(128, 134, 150));
    g.c_cone = T->canvas_color(g.cv, T->rgb(86, 140, 96));
    g.c_cone2 = T->canvas_color(g.cv, T->rgb(140, 190, 148));
    g.c_skin = T->canvas_color(g.cv, T->rgb(246, 200, 164));
    g.c_hair = T->canvas_color(g.cv, T->rgb(255, 206, 70));
    g.c_shirt = T->canvas_color(g.cv, T->rgb(244, 72, 148));
    g.c_jeans = T->canvas_color(g.cv, T->rgb(66, 104, 190));
    g.c_board = T->canvas_color(g.cv, T->rgb(90, 222, 216));
    g.c_wheel = T->canvas_color(g.cv, T->rgb(250, 250, 255));
    g.c_shadow = T->canvas_color(g.cv, T->rgb(38, 36, 48));
    g.c_text = T->canvas_color(g.cv, T->rgb(248, 248, 255));
    g.c_dim = T->canvas_color(g.cv, T->rgb(124, 122, 146));
    g.c_accent = T->canvas_color(g.cv, T->rgb(255, 214, 61));
    g.c_danger = T->canvas_color(g.cv, T->rgb(255, 84, 92));
    g.c_panel = T->canvas_color(g.cv, T->rgb(14, 12, 22));

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
            /* however she is being held now is cruising; nobody holds a watch level */
            g.pitch_neutral = g.pitch;
            g.phase = SKATING;
            g.phase_t = 0;
            sfx_start();
        }
        break;
    case SKATING: step(in, dt); break;
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

    draw_world();
    draw_skater();
    draw_hud();

    if (g.phase == READY) banner("TIP FORWARD TO PUSH", "TAP TO OLLIE", g.c_board);
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
