// STARFALL - a run down a shaft, where the way out is always somewhere round the edge.
//
// The screen is round, so the game is a tunnel seen head on: the middle of the screen is
// far away and the rim is right in front of you. Everything approaches by sweeping outward
// from the centre and off the edge. Barriers come in rings with one gap in each; you turn
// the watch to put your ship in the gap before the ring reaches you, and tap to shoot the
// mines that ride between them.
//
//   turn the watch - move around the ring
//   tap            - fire
//   swipe left     - pause menu
//
// Written against tat_api.h alone: it includes nothing else from the console and calls
// nothing by name. Built as a package with tools/mktat.py.
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "tat/tat_api.h"

static const tat_api_t *T;

// ---------------------------------------------------------------- the shaft

#define SCALE 2
#define CW ((TAT_SCREEN + SCALE - 1) / SCALE)   /* 233 */
#define CC (CW / 2)
#define PI 3.14159265f
#define TAU (2 * PI)

// A pixel's distance from the centre is how far down the shaft it is. Near the rim is
// close enough to hit you; the middle is the far end. Perspective is the usual 1/z, so a
// thing at depth z is drawn at radius K/z and appears to accelerate as it arrives.
#define R_MIN 14.0f     /* the far end: anything smaller is just the vanishing point */
#define R_HIT 96.0f     /* where your ship sits, and where a barrier reaches you */
#define R_MAX 112.0f    /* past this it has gone by */
#define K 900.0f
#define Z_FAR (K / R_MIN)
#define Z_HIT (K / R_HIT)
#define Z_GONE (K / R_MAX)

// Depth is bucketed so the shader can find what is at a pixel without searching: one pass
// over the obstacles fills this table, then every pixel is a single lookup.
#define BANDS 64

#define MAX_RINGS 10
#define MAX_MINES 12
#define MAX_BOLTS 8
#define LIVES 3

#define SHIP_HALF 0.16f      /* how wide the ship is, in radians */
#define TURN_RATE 3.4f       /* radians per second at full tilt */

typedef struct {
    float z;
    float gap_a;      /* the middle of the way through */
    float gap_half;   /* how wide the gap is, in radians */
    bool counted;     /* already scored for getting past */
    bool alive;
} Ring;

typedef struct {
    float z, a;
    bool alive;
} Mine;

typedef struct {
    float z, a;
    bool alive;
} Bolt;

typedef enum { READY, PLAYING, HIT, OVER } Phase;

static struct {
    tat_canvas_t *cv;
    const uint16_t *pol_a, *pol_r;

    /* palette */
    uint8_t c_void, c_wall[6], c_ring, c_ring_hi, c_gap, c_ship, c_ship_hi, c_bolt;
    uint8_t c_mine, c_mine_hi, c_text, c_dim, c_accent, c_danger, c_panel, c_box;

    /* the run */
    Phase phase;
    float phase_t;
    float ship_a;        /* where the ship is around the ring */
    float speed;         /* how fast depth is eaten, in z per second */
    float dist;          /* how far down the shaft, for the score */
    int score, bonus, best;   /* score is distance plus bonus, worked out each frame */
    int lives;
    float spawn_z;       /* the next ring goes in when the last one has come this far */
    float fire_t;
    float shake;

    Ring rings[MAX_RINGS];
    Mine mines[MAX_MINES];
    Bolt bolts[MAX_BOLTS];

    /* what is at each depth band this frame: -1 nothing, 0..n a ring, or MINE_BAND */
    int8_t band[BANDS];
    float band_gap_a[BANDS], band_gap_half[BANDS];

    uint32_t seed;
    bool dirty;
} g;

// ---------------------------------------------------------------- odds and ends

static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// The shaft is generated as it is flown, and nobody replays a seed, so this is just a
// cheap source of noise rather than something that has to be reproducible.
static float frand(float lo, float hi)
{
    g.seed = g.seed * 1664525u + 1013904223u;
    return lo + (hi - lo) * ((g.seed >> 8 & 0xFFFF) / 65535.0f);
}

// Shortest way round from a to b, which is what every angle comparison here wants.
static float angle_diff(float a, float b)
{
    float d = fmodf(a - b + PI, TAU);
    if (d < 0) d += TAU;
    return d - PI;
}

static int band_of(float z)
{
    const float r = K / z;
    if (r < R_MIN || r > R_MAX) return -1;
    const int b = (int)((r - R_MIN) / (R_MAX - R_MIN) * BANDS);
    return b < 0 ? 0 : (b >= BANDS ? BANDS - 1 : b);
}

// ---------------------------------------------------------------- sound

static void tone1(float f0, float f1, uint16_t ms, uint8_t wave, float vol, uint16_t delay)
{
    const tat_tone_t t = {f0, f1, ms, wave, vol, delay};
    T->tone(&t);
}

static void sfx_fire(void) { tone1(900, 360, 40, TAT_SQUARE, 0.28f, 0); }
static void sfx_pop(void)
{
    tone1(420, 90, 70, TAT_NOISE, 0.55f, 0);
    tone1(700, 1200, 60, TAT_TRIANGLE, 0.4f, 30);
}
static void sfx_through(void) { tone1(620, 880, 45, TAT_TRIANGLE, 0.3f, 0); }
static void sfx_hit(void)
{
    tone1(300, 70, 220, TAT_NOISE, 0.75f, 0);
    tone1(180, 60, 260, TAT_SQUARE, 0.55f, 60);
}
static void sfx_start(void) { tone1(420, 900, 120, TAT_TRIANGLE, 0.6f, 0); }
static void sfx_over(void)
{
    tone1(392, 0, 160, TAT_SQUARE, 0.6f, 0);
    tone1(311, 0, 160, TAT_SQUARE, 0.6f, 160);
    tone1(233, 0, 340, TAT_SQUARE, 0.6f, 320);
}

// ---------------------------------------------------------------- the run

static void add_ring(void)
{
    for (int i = 0; i < MAX_RINGS; i++) {
        if (g.rings[i].alive) continue;
        Ring *r = &g.rings[i];
        r->z = Z_FAR;
        r->gap_a = frand(0, TAU);
        // The gap narrows as you get further in, but never past what the ship can fit
        // through with a little room to spare.
        const float tight = clampf(g.dist / 4000.0f, 0, 1);
        r->gap_half = SHIP_HALF + 0.62f - 0.34f * tight;
        r->counted = false;
        r->alive = true;
        return;
    }
}

static void add_mine(float near_a)
{
    for (int i = 0; i < MAX_MINES; i++) {
        if (g.mines[i].alive) continue;
        g.mines[i].z = Z_FAR;
        // Mines sit off to one side of the gap, so the way through is rarely free.
        g.mines[i].a = near_a + frand(0.8f, TAU - 0.8f);
        g.mines[i].alive = true;
        return;
    }
}

static void start_run(void)
{
    memset(g.rings, 0, sizeof(g.rings));
    memset(g.mines, 0, sizeof(g.mines));
    memset(g.bolts, 0, sizeof(g.bolts));
    g.ship_a = -PI / 2;   /* the top of the screen */
    g.speed = 26.0f;
    g.dist = 0;
    g.score = 0;
    g.bonus = 0;
    g.lives = LIVES;
    g.spawn_z = Z_FAR;
    g.fire_t = 0;
    g.shake = 0;
    g.phase = READY;
    g.phase_t = 0;
    g.dirty = true;
}

static void lose_life(void)
{
    g.lives--;
    g.shake = 1.0f;
    sfx_hit();
    if (g.lives > 0) {
        g.phase = HIT;
        g.phase_t = 0;
        // Clear what is close enough to hit again the instant play resumes.
        for (int i = 0; i < MAX_RINGS; i++)
            if (g.rings[i].alive && g.rings[i].z < Z_HIT * 1.8f) g.rings[i].alive = false;
        for (int i = 0; i < MAX_MINES; i++)
            if (g.mines[i].alive && g.mines[i].z < Z_HIT * 1.8f) g.mines[i].alive = false;
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

static void fire(void)
{
    for (int i = 0; i < MAX_BOLTS; i++) {
        if (g.bolts[i].alive) continue;
        g.bolts[i].z = Z_HIT;
        g.bolts[i].a = g.ship_a;
        g.bolts[i].alive = true;
        sfx_fire();
        return;
    }
}

static void step(const tat_input_t *in, float dt)
{
    // Turning the watch turns the ship. Gravity in the plane of the screen tells us which
    // way is down; the ship goes round to meet it, which makes "hold the gap at the top"
    // the natural way to play.
    const float lean = in->tilt.ax;
    g.ship_a += clampf(lean * 2.4f, -1.0f, 1.0f) * TURN_RATE * dt;
    if (g.ship_a > PI) g.ship_a -= TAU;
    if (g.ship_a < -PI) g.ship_a += TAU;

    g.speed += 1.6f * dt;             /* it never stops getting faster */
    if (g.speed > 90.0f) g.speed = 90.0f;
    g.dist += g.speed * dt;
    g.shake = g.shake > 0 ? g.shake - dt * 3.0f : 0;

    g.fire_t -= dt;
    if (in->touch.pressed && g.fire_t <= 0) {
        fire();
        g.fire_t = 0.16f;
    }

    /* rings come at you */
    for (int i = 0; i < MAX_RINGS; i++) {
        Ring *r = &g.rings[i];
        if (!r->alive) continue;
        r->z -= g.speed * dt;
        if (r->z <= Z_GONE) {
            r->alive = false;
            continue;
        }
        if (!r->counted && r->z <= Z_HIT) {
            r->counted = true;
            // Through the gap, or into the wall. The gap is the only way past.
            if (fabsf(angle_diff(g.ship_a, r->gap_a)) < r->gap_half - SHIP_HALF * 0.5f) {
                g.bonus += 10;
                sfx_through();
            } else {
                lose_life();
                return;
            }
        }
    }

    for (int i = 0; i < MAX_MINES; i++) {
        Mine *m = &g.mines[i];
        if (!m->alive) continue;
        m->z -= g.speed * dt;
        if (m->z <= Z_GONE) {
            m->alive = false;
            continue;
        }
        if (m->z <= Z_HIT && m->z > Z_HIT * 0.86f &&
            fabsf(angle_diff(g.ship_a, m->a)) < SHIP_HALF + 0.10f) {
            m->alive = false;
            lose_life();
            return;
        }
    }

    /* bolts go away from you, down the shaft */
    for (int i = 0; i < MAX_BOLTS; i++) {
        Bolt *b = &g.bolts[i];
        if (!b->alive) continue;
        b->z += (g.speed + 120.0f) * dt;
        if (b->z >= Z_FAR) {
            b->alive = false;
            continue;
        }
        for (int j = 0; j < MAX_MINES; j++) {
            Mine *m = &g.mines[j];
            if (!m->alive) continue;
            if (fabsf(m->z - b->z) < 3.0f && fabsf(angle_diff(m->a, b->a)) < 0.22f) {
                m->alive = false;
                b->alive = false;
                g.bonus += 25;
                sfx_pop();
                break;
            }
        }
    }

    // Keep the shaft stocked. "How far has the newest ring come" has to be measured from
    // the FURTHEST one, not the nearest: with the nearest, an empty shaft reads as "the
    // newest ring has travelled nothing" and the first one is never laid at all.
    float newest = -1.0f;
    for (int i = 0; i < MAX_RINGS; i++)
        if (g.rings[i].alive && g.rings[i].z > newest) newest = g.rings[i].z;
    const float spacing = 46.0f + 26.0f * clampf(1.0f - g.dist / 3000.0f, 0, 1);
    if (newest < 0 || Z_FAR - newest > spacing) {
        add_ring();
        // A mine rides with about every other ring, off to the side of the way through.
        if (frand(0, 1) < 0.55f) {
            float a = 0;
            for (int i = 0; i < MAX_RINGS; i++)
                if (g.rings[i].alive && g.rings[i].z > Z_FAR - 2.0f) a = g.rings[i].gap_a;
            add_mine(a);
        }
    }

    // Score is distance plus what you earned, worked out fresh each frame. Adding a
    // fraction per frame does not work: at sixty frames a second the amount is well under
    // one, the cast takes it to zero, and the score never moves at all.
    g.score = (int)(g.dist * 0.30f) + g.bonus;
}

// ---------------------------------------------------------------- drawing

// One pass fills in what sits at each depth band, so the shader below can ask a single
// question per pixel instead of walking every obstacle.
#define BAND_NONE (-1)
#define BAND_RING (-2)

static void build_bands(void)
{
    for (int i = 0; i < BANDS; i++) g.band[i] = BAND_NONE;
    for (int i = 0; i < MAX_RINGS; i++) {
        const Ring *r = &g.rings[i];
        if (!r->alive) continue;
        const int b = band_of(r->z);
        if (b < 0) continue;
        // A ring is drawn two bands thick, so it reads as a wall rather than a hairline.
        for (int k = b; k < b + 2 && k < BANDS; k++) {
            g.band[k] = BAND_RING;
            g.band_gap_a[k] = r->gap_a;
            g.band_gap_half[k] = r->gap_half;
        }
    }
}

static void draw_shaft(void)
{
    uint8_t *px = T->canvas_pixels(g.cv);
    if (!px || !g.pol_a || !g.pol_r) return;

    const float inv = BANDS / (R_MAX - R_MIN);
    for (int y = 0; y < CW; y++) {
        for (int x = 0; x < CW; x++) {
            const int i = (SCALE * y) * TAT_SCREEN + SCALE * x;
            const float r = g.pol_r[i] / 16.0f / SCALE;
            uint8_t c;
            if (r < R_MIN) {
                c = g.c_void;   /* the far end, where everything comes from */
            } else if (r > R_MAX) {
                c = g.c_void;
            } else {
                // Walls get darker with distance, which is most of what sells the depth.
                const int shade = (int)((r - R_MIN) / (R_MAX - R_MIN) * 6);
                c = g.c_wall[shade < 0 ? 0 : (shade > 5 ? 5 : shade)];
                const int b = (int)((r - R_MIN) * inv);
                if (b >= 0 && b < BANDS && g.band[b] == BAND_RING) {
                    const float a = (g.pol_a[i] / 65536.0f) * TAU - PI;
                    const float d = fabsf(angle_diff(a, g.band_gap_a[b]));
                    c = d < g.band_gap_half[b] ? g.c_gap : g.c_ring;
                }
            }
            px[y * CW + x] = c;
        }
    }
}

// A thing at depth z and angle a, drawn as a blob whose size follows the perspective.
static void draw_at(float z, float a, uint8_t col, uint8_t hi, float size)
{
    const float r = K / z;
    if (r < R_MIN || r > R_MAX + 6) return;
    const int rad = (int)(size * r / 40.0f) + 1;
    const int x = CC + (int)(cosf(a) * r);
    const int y = CC + (int)(sinf(a) * r);
    T->canvas_fill_circle(g.cv, x, y, rad, col);
    if (rad > 2) T->canvas_fill_circle(g.cv, x - rad / 3, y - rad / 3, rad / 3, hi);
}

static void draw_ship(void)
{
    const float wobble = g.shake > 0 ? sinf(g.phase_t * 60.0f) * g.shake * 3.0f : 0;
    const float r = R_HIT + wobble;
    const int x = CC + (int)(cosf(g.ship_a) * r);
    const int y = CC + (int)(sinf(g.ship_a) * r);
    // A dart pointing at the middle of the screen, which is where it is going.
    const float nx = -cosf(g.ship_a), ny = -sinf(g.ship_a);
    const float sx = -ny, sy = nx;
    const int tipx = x + (int)(nx * 9), tipy = y + (int)(ny * 9);
    const int lx = x + (int)(sx * 5), ly = y + (int)(sy * 5);
    const int rx = x - (int)(sx * 5), ry = y - (int)(sy * 5);
    T->canvas_line(g.cv, tipx, tipy, lx, ly, g.c_ship);
    T->canvas_line(g.cv, tipx, tipy, rx, ry, g.c_ship);
    T->canvas_line(g.cv, lx, ly, rx, ry, g.c_ship);
    T->canvas_fill_circle(g.cv, x, y, 2, g.c_ship_hi);
}

static void draw_hud(void)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", g.score);
    T->canvas_text_centered(g.cv, CC, 12, buf, g.c_text, 1, true);
    for (int i = 0; i < LIVES; i++)
        T->canvas_fill_circle(g.cv, CC - (LIVES - 1) * 6 + i * 12, 25, 3,
                              i < g.lives ? g.c_ship : g.c_box);
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

static void sf_begin(const tat_api_t *api)
{
    T = api;
    memset(&g, 0, sizeof(g));
    g.cv = T->canvas_create(SCALE, 0);
    if (!g.cv) {
        T->log("no memory for the shaft");
        return;
    }
    g.pol_a = T->polar_angles();
    g.pol_r = T->polar_radii();
    if (!g.pol_a || !g.pol_r) T->log("no polar tables; the shaft cannot be drawn");

    g.c_void = T->canvas_color(g.cv, T->rgb(4, 5, 10));
    static const uint8_t WALL[6][3] = {
        {12, 16, 34}, {18, 24, 50}, {26, 34, 68}, {34, 46, 88}, {44, 60, 110}, {56, 76, 134},
    };
    for (int i = 0; i < 6; i++) g.c_wall[i] = T->canvas_color(g.cv, T->rgb(WALL[i][0], WALL[i][1], WALL[i][2]));
    g.c_ring = T->canvas_color(g.cv, T->rgb(236, 72, 110));
    g.c_ring_hi = T->canvas_color(g.cv, T->rgb(255, 150, 176));
    g.c_gap = T->canvas_color(g.cv, T->rgb(20, 40, 74));
    g.c_ship = T->canvas_color(g.cv, T->rgb(120, 240, 255));
    g.c_ship_hi = T->canvas_color(g.cv, T->rgb(255, 255, 255));
    g.c_bolt = T->canvas_color(g.cv, T->rgb(255, 240, 120));
    g.c_mine = T->canvas_color(g.cv, T->rgb(250, 168, 40));
    g.c_mine_hi = T->canvas_color(g.cv, T->rgb(255, 232, 150));
    g.c_text = T->canvas_color(g.cv, T->rgb(246, 248, 255));
    g.c_dim = T->canvas_color(g.cv, T->rgb(130, 140, 170));
    g.c_accent = T->canvas_color(g.cv, T->rgb(255, 214, 61));
    g.c_danger = T->canvas_color(g.cv, T->rgb(255, 80, 90));
    g.c_panel = T->canvas_color(g.cv, T->rgb(10, 12, 22));
    g.c_box = T->canvas_color(g.cv, T->rgb(70, 76, 96));

    g.seed = (uint32_t)T->now_us();
    T->save_get("best", &g.best, 0);
    start_run();
    T->log("ready, best %d", g.best);
}

static void sf_enter(void) { g.dirty = true; }

static void sf_update(float dt)
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
            g.phase = PLAYING;
            g.phase_t = 0;
            sfx_start();
        }
        break;
    case PLAYING: step(in, dt); break;
    case HIT:
        if (g.phase_t > 0.9f) {
            g.phase = PLAYING;
            g.phase_t = 0;
        }
        break;
    case OVER:
        if (g.phase_t > 0.8f && (ges->tap || (in->clicked & TAT_BTN_B))) start_run();
        break;
    }
}

static void sf_draw(void)
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

    build_bands();
    draw_shaft();

    for (int i = 0; i < MAX_MINES; i++)
        if (g.mines[i].alive) draw_at(g.mines[i].z, g.mines[i].a, g.c_mine, g.c_mine_hi, 5.0f);
    for (int i = 0; i < MAX_BOLTS; i++)
        if (g.bolts[i].alive) draw_at(g.bolts[i].z, g.bolts[i].a, g.c_bolt, g.c_bolt, 2.2f);
    if (g.phase != OVER) draw_ship();
    draw_hud();

    if (g.phase == READY) banner("TURN TO STEER", "TAP TO FIRE", g.c_ship);
    else if (g.phase == HIT) banner("HIT!", NULL, g.c_danger);
    else if (g.phase == OVER) {
        char sub[24];
        snprintf(sub, sizeof(sub), "SCORE %d", g.score);
        banner("LOST THE SHIP", sub, g.c_danger);
    }

    T->canvas_present(g.cv);
}

static void sf_redraw(void) { g.dirty = true; }

static bool sf_keep_awake(void) { return g.phase == PLAYING && !T->menu_is_open(); }

static void sf_unload(void)
{
    if (g.cv) T->canvas_destroy(g.cv);
    g.cv = NULL;
}

const tat_game_t tat_game = {
    .magic = TAT_GAME_MAGIC,
    .api_major = TAT_API_MAJOR,
    .api_minor = TAT_API_MINOR,
    .id = "starfall",
    .name = "STARFALL",
    .accent_r = 120, .accent_g = 240, .accent_b = 255,
    .assets = NULL,
    .asset_count = 0,
    .begin = sf_begin,
    .enter = sf_enter,
    .update = sf_update,
    .draw = sf_draw,
    .leave = NULL,
    .unload = sf_unload,
    .redraw = sf_redraw,
    .keep_awake = sf_keep_awake,
};
