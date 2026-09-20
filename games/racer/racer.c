// GRAND PRIX - a pseudo-3D racer you steer by turning the watch like a wheel.
//
// The scene is drawn upright into a small 8-bit picture and then rotated back by however
// far the watch is turned, so the road stays level in the world while the watch does not.
// Tipping the watch forward is the throttle.
//
//   turn the watch - steer
//   tip forward    - faster; tip back to slow (off = flat out)
//   touch          - pause
//   swipe left     - pause menu
//
// Written against tat_api.h alone: it includes nothing else from the console and calls
// nothing by name.
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "tat/tat_api.h"

static const tat_api_t *T;

// ---------------------------------------------------------------- the view

// 256 wide: a little bigger than the 233 px circle, so that when the picture is rotated
// the corners have something to show instead of black wedges.
#define VW 256
#define VH 256
#define VC 128
#define HORIZON 108

// Road model, after the classic pseudo-3D racers.
#define SEG_LEN 200.0f
#define RUMBLE_LEN 3           /* segments per kerb stripe */
#define ROAD_W 2000.0f         /* half-width of the road in world units */
#define CAM_H 1000.0f
#define CAM_DEPTH 0.84f        /* 1 / tan(fov / 2), about a 100 degree view */
#define DRAW_DIST 110
#define MAX_SPEED (SEG_LEN * 60.0f)
#define ACCEL (MAX_SPEED / 5.0f)
#define BRAKE (-MAX_SPEED)
#define OFFROAD_DECEL (-MAX_SPEED / 2.0f)
#define OFFROAD_LIMIT (MAX_SPEED / 4.0f)
#define CENTRIFUGAL 0.18f      /* how hard corners push you wide */
#define PLAYER_Z (CAM_H * CAM_DEPTH)
#define LAPS 3
#define RIVALS 5
#define FULL_LOCK 0.70f        /* radians of wheel for full steering, about 40 degrees */
#define STEER_RATE 3.0f        /* how fast the car crosses the road at full lock: comfortably
                                  more than the sharpest corner pushes back */

// buildTrack lays segments until it passes 1300 and then adds a run-out, and each addRoad
// adds at most 20 + 50 + 20. 1600 leaves room for the longest possible overshoot.
#define MAX_SEGS 1600
// Scenery is one prop every 3 to 8 segments, plus the start straight's stands and gantry.
// At the tightest spacing that is a third of MAX_SEGS; 700 is comfortably past it, and the
// generator stops cleanly rather than running off the end.
#define MAX_PROPS 700

// The car sprite is 32 x 16 in the sheet. Its blue livery colours are swapped for each
// car's own at draw time, which is why the palette indices below have to be exactly these.
#define CAR_W 32
#define CAR_H 16

enum {
    P_BLACK, P_WHITE,
    P_SKY0, P_SKY1, P_SKY2, P_SKY3, P_SKY4, P_SKY5, P_SKY6, P_SKY7,   /* sky top to horizon */
    P_SUN, P_SUN_GLOW, P_CLOUD, P_MOUNTAIN_FAR, P_MOUNTAIN, P_GRASS_A, P_GRASS_B, P_ROAD_A, P_ROAD_B,
    P_KERB_RED, P_KERB_WHITE, P_LANE, P_TIRE, P_TIRE_LIT, P_WING, P_GREY, P_HELMET, P_YELLOW, P_RED, P_GREEN,
    P_PANEL, P_FLAME_A, P_FLAME_B, P_TRUNK, P_LEAF, P_LEAF_DARK, P_SIGN, P_SIGN_STRIPE,
    /* car liveries: body colours, then the matching shade at the same offset */
    P_CAR0, P_CAR1, P_CAR2, P_CAR3, P_CAR4, P_CAR_PLAYER,
    P_CAR0_D, P_CAR1_D, P_CAR2_D, P_CAR3_D, P_CAR4_D, P_CAR_PLAYER_D,
    P_COUNT
};
#define CAR_SHADE (P_CAR0_D - P_CAR0)

// The palette as plain bytes. It used to be a constexpr table of packed colours, but the
// packing is the console's business now and rgb() is a call, so it is built once in begin.
static const uint8_t PALETTE_RGB[P_COUNT][3] = {
    {0, 0, 0}, {255, 255, 255},
    {24, 58, 140}, {36, 78, 162}, {52, 100, 184}, {74, 124, 204}, {102, 150, 220},
    {136, 176, 232}, {172, 202, 242}, {208, 226, 248},
    {255, 244, 190}, {250, 226, 150}, {244, 248, 255}, {104, 124, 160}, {64, 78, 104},
    {38, 140, 52}, {30, 122, 44}, {98, 98, 104}, {88, 88, 94},
    {214, 40, 40}, {240, 240, 240}, {235, 235, 235}, {22, 22, 26}, {62, 62, 70},
    {34, 34, 42}, {112, 116, 126}, {255, 214, 40}, {255, 220, 40}, {255, 50, 50}, {40, 220, 110},
    {14, 16, 24}, {255, 150, 30}, {255, 232, 90}, {92, 60, 30}, {40, 150, 60}, {26, 104, 44},
    {244, 244, 248}, {220, 40, 50},
    {40, 120, 255}, {255, 204, 30}, {40, 190, 90}, {170, 80, 220}, {255, 130, 30}, {225, 30, 40},
    {22, 72, 170}, {190, 146, 14}, {22, 128, 56}, {112, 46, 152}, {186, 86, 12}, {150, 16, 26},
};

typedef struct {
    float curve;
} Segment;

typedef struct {
    float z, offset, speed;
    int lap;
    uint8_t color;
} Car;

typedef struct {
    float x, y, w, scale;
} Projected;

// Things beside the track. offset is in road half-widths (beyond +-1 is off the road).
typedef enum { PROP_TREE, PROP_SIGN, PROP_GANTRY, PROP_STAND, PROP_BUSH } PropKind;
typedef struct {
    int seg;
    float offset;
    uint8_t kind;
} Prop;

typedef enum { READY, COUNTDOWN, RACING, FINISHED } Phase;

static struct {
    tat_canvas_t *cv;
    uint8_t *view;   /* the canvas pixels, written directly */
    tat_sheet_t *car_sheet, *trees, *sign, *stand, *bush;
    int car_sheet_w;   /* the sheet's own stride, needed by the car blitter */

    // The track: 6 KB of segments, 8 KB of scenery and a 6 KB index. As plain members
    // these landed in internal RAM, which is the scarce kind - it took the free internal
    // heap from 48 KB to 23 KB and Wi-Fi stopped answering. alloc() hands out the large,
    // plentiful memory, and it is what a loaded package would have to use anyway.
    Segment *segs;
    int n_segs;
    Prop *props;                /* sorted by segment */
    int n_props;
    int *prop_first;            /* first prop index for each segment, n_props if none */
    float track_len;
    Car rivals[RIVALS];
    float pos, px, speed;       /* distance along the track, lateral (-1..1 on road), speed */
    int lap;
    float lap_t, best_lap, last_lap, race_t;
    int place;
    Phase phase;
    float phase_t;
    int countdown;
    float sky_off;
    float engine_t, kerb_t;
    int wins;

    /* steering */
    float grav_x, grav_y;       /* smoothed gravity in screen axes */
    float roll;                 /* how far the picture is rotated, radians */
    float steer;                /* -1..1 */
    bool mirror;
    // Tipping the watch forward/back is the throttle. "Level" is however it was held when
    // the race started; 0 = off (always flat out), 1..3 = low/med/high sensitivity.
    float grav_z, pitch, pitch_neutral, throttle;
    int pitch_sens;

    /* rendering */
    Projected proj_near[DRAW_DIST], proj_far[DRAW_DIST];
    bool proj_ok[DRAW_DIST];
    int64_t fps_t0;
    int fps_frames;

    /* menu */
    bool menu_swallow;
} g;

// ---------------------------------------------------------------- odds and ends

static float frand(void) { return T->random() / 4294967296.0f; }
static float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
static float minf(float a, float b) { return a < b ? a : b; }
static float maxf(float a, float b) { return a > b ? a : b; }
static int mini(int a, int b) { return a < b ? a : b; }
static int maxi(int a, int b) { return a > b ? a : b; }
static float ease_in(float a, float b, float t) { return a + (b - a) * t * t; }
static float ease_in_out(float a, float b, float t) { return a + (b - a) * ((-cosf(t * 3.14159265f) / 2) + 0.5f); }

// ---------------------------------------------------------------- sound

static void tone1(float f0, float f1, uint16_t ms, uint8_t wave, float vol, uint16_t delay)
{
    const tat_tone_t t = {f0, f1, ms, wave, vol, delay};
    T->tone(&t);
}

static void sfx_beep(bool go) { tone1(go ? 1040.0f : 520.0f, 0, go ? 420 : 160, TAT_SQUARE, 0.7f, 0); }
static void sfx_bump(void) { tone1(170, 70, 110, TAT_NOISE, 0.7f, 0); }
static void sfx_kerb(void) { tone1(120, 90, 30, TAT_NOISE, 0.22f, 0); }

static void sfx_lap(void)
{
    tone1(784, 0, 80, TAT_SQUARE, 0.6f, 0);
    tone1(1047, 0, 140, TAT_SQUARE, 0.6f, 80);
}

static void sfx_finish(void)
{
    tone1(523, 0, 100, TAT_SQUARE, 0.7f, 0);
    tone1(659, 0, 100, TAT_SQUARE, 0.7f, 100);
    tone1(784, 0, 100, TAT_SQUARE, 0.7f, 200);
    tone1(1047, 0, 300, TAT_SQUARE, 0.8f, 300);
}

static void sfx_engine(float speed01)
{
    // Climb through five "gears": the pitch rises, drops at each shift, and rises again.
    const float gg = speed01 * 5.0f;
    const int gear = mini(4, (int)gg);
    const float f = 70.0f + gear * 16.0f + (gg - gear) * 120.0f;
    tone1(f, f * 1.03f, 85, TAT_TRIANGLE, 0.16f + 0.10f * speed01, 0);
}

// ---------------------------------------------------------------- the track

static void add_segment(float curve)
{
    if (g.n_segs >= MAX_SEGS) return;
    g.segs[g.n_segs++].curve = curve;
}

static void add_road(int enter, int hold, int leave, float curve)
{
    for (int i = 0; i < enter; i++) add_segment(ease_in(0, curve, (float)i / enter));
    for (int i = 0; i < hold; i++) add_segment(curve);
    for (int i = 0; i < leave; i++) add_segment(ease_in_out(curve, 0, (float)i / leave));
}

static void add_prop(int seg, float offset, PropKind kind)
{
    if (g.n_props >= MAX_PROPS) return;
    g.props[g.n_props].seg = seg;
    g.props[g.n_props].offset = offset;
    g.props[g.n_props].kind = (uint8_t)kind;
    g.n_props++;
}

// A fresh circuit every race: straights and corners of random length and sharpness.
static void build_track(void)
{
    g.n_segs = 0;
    add_road(10, 40, 10, 0);   /* start/finish straight */
    // Leave room for the longest addRoad plus the run-out, so the track always closes
    // properly instead of being cut off wherever the array happened to fill.
    while (g.n_segs < 1300 && g.n_segs < MAX_SEGS - 190) {
        const float r = frand();
        if (r < 0.25f) {
            add_road(8, 20 + (int)(frand() * 50), 8, 0);
        } else {
            const float sharp = (r < 0.55f ? 2.0f : r < 0.85f ? 3.5f : 5.0f) * (frand() < 0.5f ? -1 : 1);
            add_road(20 + (int)(frand() * 20), 20 + (int)(frand() * 50), 20 + (int)(frand() * 20), sharp);
            if (frand() < 0.3f) add_road(15, 15 + (int)(frand() * 20), 15, -sharp * 0.8f);   /* chicane */
        }
    }
    add_road(10, 30, 10, 0);
    g.track_len = g.n_segs * SEG_LEN;

    // Scenery: trees scattered along both sides, arrow boards on the outside of corners.
    g.n_props = 0;
    const int n = g.n_segs;
    add_prop(4, 0, PROP_GANTRY);
    for (int i = 6; i < 44; i += 6) {
        add_prop(i, -2.1f, PROP_STAND);
        add_prop(i, 2.1f, PROP_STAND);
    }
    for (int i = 48; i < n; i += 3 + (int)(frand() * 5)) {
        const float side = frand() < 0.5f ? -1.0f : 1.0f;
        if (fabsf(g.segs[i].curve) > 2.5f && frand() < 0.5f)
            add_prop(i, (g.segs[i].curve > 0 ? -1.0f : 1.0f) * 1.45f, PROP_SIGN);
        else if (frand() < 0.3f)
            add_prop(i, side * (1.3f + frand() * 0.8f), PROP_BUSH);
        else
            add_prop(i, side * (1.5f + frand() * 1.6f), PROP_TREE);
    }

    for (int i = 0; i <= n; i++) g.prop_first[i] = g.n_props;
    for (int k = g.n_props - 1; k >= 0; k--) g.prop_first[g.props[k].seg] = k;
    for (int i = n - 1; i >= 0; i--)
        if (g.prop_first[i] == g.n_props || g.props[g.prop_first[i]].seg != i)
            g.prop_first[i] = mini(g.prop_first[i], g.prop_first[i + 1]);
}

static void save_settings(void)
{
    T->save_set("mirror", g.mirror ? 1 : 0);
    T->save_set("pitch", g.pitch_sens);
    T->save_set("wins", g.wins);
}

static void new_race(void)
{
    build_track();
    g.pos = 0;
    g.px = 0;
    g.speed = 0;
    g.lap = 1;
    g.lap_t = g.best_lap = g.last_lap = g.race_t = 0;
    static const uint8_t colors[RIVALS] = {P_CAR0, P_CAR1, P_CAR2, P_CAR3, P_CAR4};
    for (int i = 0; i < RIVALS; i++) {
        /* a grid ahead of you, fastest at the front */
        g.rivals[i].z = (i + 1) * 5.0f * SEG_LEN + PLAYER_Z;
        g.rivals[i].offset = (i % 2) ? 0.45f : -0.45f;
        g.rivals[i].speed = MAX_SPEED * (0.74f + 0.045f * i);
        g.rivals[i].lap = 1;
        g.rivals[i].color = colors[i];
    }
    g.place = RIVALS + 1;
    g.phase = READY;
    g.phase_t = 0;
}

// ---------------------------------------------------------------- update

static void update_steering(const tat_input_t *in, float dt)
{
    // Low-pass gravity to take out hand tremor, then read the wheel angle from it.
    const float gx = in->tilt.ax * (g.mirror ? -1 : 1), gy = in->tilt.ay;
    const float k = 1.0f - expf(-dt / 0.12f);
    g.grav_x += (gx - g.grav_x) * k;
    g.grav_y += (gy - g.grav_y) * k;
    g.grav_z += (in->tilt.az - g.grav_z) * k;
    /* forward/back lean: 0 with the screen facing you, positive as the top tips away */
    g.pitch = atan2f(g.grav_z, sqrtf(g.grav_x * g.grav_x + g.grav_y * g.grav_y));
    if (g.grav_x * g.grav_x + g.grav_y * g.grav_y > 0.09f) {
        /* lying flat there is no "down" to read, so hold the last angle */
        g.roll = atan2f(g.grav_x, g.grav_y);
    }
    float s = g.roll / FULL_LOCK;
    const float dead = 0.06f;
    s = fabsf(s) < dead ? 0 : (s - copysignf(dead, s)) / (1 - dead);
    s = clampf(s, -1, 1);
    /* gentle around the middle for small corrections, full authority at the ends */
    s = 0.65f * s + 0.35f * s * fabsf(s);
    g.steer += (s - g.steer) * (1.0f - expf(-dt / 0.08f));
}

static float total_distance(float z, int lp) { return (lp - 1) * g.track_len + z; }

static void update_race(const tat_input_t *in, float dt)
{
    (void)in;
    const float sp = g.speed / MAX_SPEED;
    const int seg_i = (int)(fmodf(g.pos + PLAYER_Z, g.track_len) / SEG_LEN) % g.n_segs;

    /* steering bites harder the faster you go; corners push you wide */
    g.px += g.steer * sp * dt * STEER_RATE;
    g.px -= g.segs[seg_i].curve * sp * sp * CENTRIFUGAL * dt;

    /* tip forward to go faster, back to slow right down; with it off, flat out */
    if (g.pitch_sens == 0) {
        g.throttle = 1.0f;
    } else {
        static const float kRange[4] = {0, 0.60f, 0.40f, 0.26f};   /* radians from level to full/stop */
        g.throttle = clampf(0.70f + (g.pitch - g.pitch_neutral) / kRange[g.pitch_sens] * 0.62f, 0.08f, 1.0f);
    }
    const float target = MAX_SPEED * g.throttle;
    if (g.speed < target) g.speed = minf(target, g.speed + ACCEL * dt);
    else g.speed = maxf(target, g.speed + BRAKE * 0.55f * dt);
    const bool offroad = fabsf(g.px) > 1.0f;
    if (offroad && g.speed > OFFROAD_LIMIT) g.speed += OFFROAD_DECEL * dt;
    g.px = clampf(g.px, -2.2f, 2.2f);
    g.speed = clampf(g.speed, 0, MAX_SPEED);

    /* kerbs buzz through the speaker */
    g.kerb_t -= dt;
    if (fabsf(g.px) > 0.85f && g.speed > MAX_SPEED * 0.15f && g.kerb_t <= 0) {
        sfx_kerb();
        g.kerb_t = 0.07f;
    }

    /* rivals hold their line and drift a little */
    for (int i = 0; i < RIVALS; i++) {
        Car *c = &g.rivals[i];
        c->z += c->speed * dt;
        if (c->z >= g.track_len) {
            c->z -= g.track_len;
            c->lap++;
        }
        c->offset += sinf(g.race_t * 0.7f + c->color) * 0.05f * dt;
        c->offset = clampf(c->offset, -0.75f, 0.75f);
    }

    /* running into the back of someone costs you speed */
    const float my_z = fmodf(g.pos + PLAYER_Z, g.track_len);
    for (int i = 0; i < RIVALS; i++) {
        const Car *c = &g.rivals[i];
        float gap = c->z - my_z;
        if (gap < -g.track_len / 2) gap += g.track_len;
        /* cars are 0.31 road half-widths wide, so that is how close centres get before touching */
        if (gap > 0 && gap < SEG_LEN * 0.8f && g.speed > c->speed && fabsf(g.px - c->offset) < 0.29f) {
            g.speed = c->speed * 0.75f;
            sfx_bump();
        }
    }

    g.pos += g.speed * dt;
    g.lap_t += dt;
    g.race_t += dt;
    g.sky_off += g.segs[seg_i].curve * sp * dt * 28.0f;
    if (g.pos >= g.track_len) {
        g.pos -= g.track_len;
        g.last_lap = g.lap_t;
        if (g.best_lap == 0 || g.lap_t < g.best_lap) g.best_lap = g.lap_t;
        g.lap_t = 0;
        g.lap++;
        if (g.lap > LAPS) {
            g.lap = LAPS;
            g.phase = FINISHED;
            g.phase_t = 0;
            if (g.place == 1) {
                g.wins++;
                save_settings();
            }
            sfx_finish();
            return;
        }
        sfx_lap();
    }

    /* where am I in the field? */
    const float mine = total_distance(g.pos + PLAYER_Z, g.lap);
    g.place = 1;
    for (int i = 0; i < RIVALS; i++)
        if (total_distance(g.rivals[i].z, g.rivals[i].lap) > mine) g.place++;

    g.engine_t -= dt;
    if (g.engine_t <= 0) {
        sfx_engine(sp);
        g.engine_t = 0.075f;
    }
}

// ---------------------------------------------------------------- the scene, upright

static void hspan(int y, int x0, int x1, uint8_t c)
{
    if (y < 0 || y >= VH) return;
    x0 = maxi(x0, 0);
    x1 = mini(x1, VW);
    if (x1 > x0) memset(g.view + y * VW + x0, c, x1 - x0);
}

static void rect(int x, int y, int w, int h, uint8_t c)
{
    for (int j = 0; j < h; j++) hspan(y + j, x, x + w, c);
}

// The HUD used to go through a private copy of the engine's font renderer, reaching into
// the font table directly. The canvas draws the same 5x7 font from the same origin, so the
// copy is gone; `bold` at scale 2 and up is what the old one's fattening amounted to.
static void text(int x, int y, const char *s, uint8_t c, int scale)
{
    T->canvas_text(g.cv, x, y, s, c, scale, scale > 1);
}

static void text_centered(int cx, int y, const char *s, uint8_t c, int scale)
{
    /* canvas_text_centered centres vertically too; the old one took the top edge */
    T->canvas_text_centered(g.cv, cx, y + (7 * scale) / 2, s, c, scale, scale > 1);
}

static void draw_background(void)
{
    /* sky: eight bands, deep blue overhead fading to haze at the horizon */
    for (int y = 0; y < HORIZON; y++) memset(g.view + y * VW, P_SKY0 + mini(7, y * 8 / HORIZON), VW);

    /* a low sun and a few clouds that drift with the corners, slower than the hills */
    const int sun_x = VC + 46 - (int)(g.sky_off * 0.25f) % VW;
    for (int dy = -11; dy <= 11; dy++) {
        const int half = (int)sqrtf((float)(11 * 11 - dy * dy));
        hspan(HORIZON - 34 + dy, sun_x - half - 2, sun_x + half + 2, P_SUN_GLOW);
    }
    for (int dy = -8; dy <= 8; dy++) {
        const int half = (int)sqrtf((float)(8 * 8 - dy * dy));
        hspan(HORIZON - 34 + dy, sun_x - half, sun_x + half, P_SUN);
    }
    for (int k = 0; k < 4; k++) {
        const int cx = ((k * 83 + 20 - (int)(g.sky_off * 0.4f)) % (VW + 60) + VW + 60) % (VW + 60) - 30;
        const int cy = 38 + (k * 13) % 26;
        hspan(cy, cx - 12, cx + 12, P_CLOUD);
        hspan(cy - 1, cx - 8, cx + 9, P_CLOUD);
        hspan(cy - 2, cx - 3, cx + 5, P_CLOUD);
        hspan(cy + 1, cx - 9, cx + 8, P_CLOUD);
    }

    /* two ranges of hills: the far one paler and slower, for depth */
    for (int x = 0; x < VW; x++) {
        const float tf = x + g.sky_off * 0.55f, tn = x + g.sky_off;
        const int hf = (int)(19 + 8 * sinf(tf * 0.027f) + 4 * sinf(tf * 0.071f + 0.7f));
        const int hn = (int)(10 + 6 * sinf(tn * 0.043f + 2.1f) + 3 * sinf(tn * 0.117f + 1.3f));
        for (int y = HORIZON - hf; y < HORIZON - hn; y++) g.view[y * VW + x] = P_MOUNTAIN_FAR;
        for (int y = HORIZON - hn; y < HORIZON; y++) g.view[y * VW + x] = P_MOUNTAIN;
    }
    for (int y = HORIZON; y < VH; y++) memset(g.view + y * VW, P_GRASS_B, VW);
}

static Projected project(float world_x, float rel_z, float cam_x)
{
    Projected p;
    p.scale = CAM_DEPTH / rel_z;
    p.x = VC + p.scale * (world_x - cam_x) * VC;
    p.y = HORIZON + p.scale * CAM_H * VC;
    p.w = p.scale * ROAD_W * VC;
    return p;
}

static void draw_segment(const Projected *n, const Projected *f, int index, int max_y)
{
    const int y_far = maxi((int)f->y, HORIZON), y_near = mini((int)n->y, max_y);
    if (y_near <= y_far) return;
    const bool alt = (index / RUMBLE_LEN) % 2;
    const uint8_t grass = alt ? P_GRASS_A : P_GRASS_B, road = alt ? P_ROAD_A : P_ROAD_B;
    const uint8_t kerb = alt ? P_KERB_RED : P_KERB_WHITE;
    const float span = maxf(1.0f, n->y - f->y);
    for (int y = y_far; y < y_near; y++) {
        const float t = (y - f->y) / span;
        const float x = f->x + (n->x - f->x) * t, w = f->w + (n->w - f->w) * t;
        const float kerb_w = w * 0.12f;
        memset(g.view + y * VW, grass, VW);
        hspan(y, (int)(x - w - kerb_w), (int)(x - w), kerb);
        hspan(y, (int)(x + w), (int)(x + w + kerb_w), kerb);
        hspan(y, (int)(x - w), (int)(x + w), index < 3 ? (uint8_t)P_WHITE : road);   /* start/finish line */
        if (alt && index >= 3) {
            const float lane_w = maxf(1.0f, w * 0.03f);
            hspan(y, (int)(x - w / 3 - lane_w), (int)(x - w / 3 + lane_w), P_LANE);
            hspan(y, (int)(x + w / 3 - lane_w), (int)(x + w / 3 + lane_w), P_LANE);
        }
    }
}

// The car, scaled to `w` pixels wide with its wheels on `bottom`. This is why the API
// hands out a sheet's raw pixels: one blue car in the sheet becomes six liveries by
// substituting two palette entries as it blits, and the body shears sideways with the
// lean while the tyres stay planted. No amount of sprite placement does either.
static void draw_car(float cx, float bottom, float w, uint8_t body, float lean, bool flame)
{
    if (w < 5) {   /* a dot in the distance */
        rect((int)cx - 1, (int)bottom - 2, 3, 2, body);
        return;
    }
    const uint8_t *sheet = g.car_sheet ? T->sheet_pixels(g.car_sheet) : NULL;
    if (!sheet) return;
    const int dw = (int)w, dh = maxi(3, (int)(w * CAR_H / CAR_W));
    const int x0 = (int)(cx - w / 2), y0 = (int)bottom - dh;
    const uint8_t shade = (uint8_t)(body + CAR_SHADE);
    const uint8_t fire = (T->random() & 1) ? P_FLAME_A : P_FLAME_B;
    for (int dy = 0; dy < dh; dy++) {
        const int y = y0 + dy;
        if (y < 0 || y >= VH) continue;
        const uint8_t *row = sheet + (dy * CAR_H / dh) * g.car_sheet_w;
        /* the body leans into the corner a little; the tyres stay planted */
        const int shift = (int)(lean * w * 0.05f * (1.0f - (float)dy / dh));
        uint8_t *out = g.view + y * VW;
        for (int dx = 0; dx < dw; dx++) {
            const int x = x0 + dx;
            if (x < 0 || x >= VW) continue;
            uint8_t c = row[dx * CAR_W / dw];
            if (!c) continue;
            if (c == P_CAR0) c = body;
            else if (c == P_CAR0_D) c = shade;
            else if (c == P_FLAME_A) {
                if (!flame) continue;
                c = fire;
            }
            const bool planted = c == P_TIRE || c == P_TIRE_LIT;
            const int xs = planted ? x : x + shift;
            if (xs >= 0 && xs < VW) out[xs] = c;
        }
    }
}

// A sprite frame scaled to `h` pixels tall (width follows), feet on `bottom`. Also its own
// blitter rather than canvas_sprite_scaled, because scenery is scaled by an arbitrary
// world height rather than a factor, and it draws into the oversized view.
static void blit_scaled(tat_sheet_t *s, int frame, float cx, float bottom, float h, bool flip)
{
    if (!s || h < 2) return;
    const uint8_t *px = T->sheet_pixels(s);
    if (!px) return;
    int sw, sh_h, fw, fh;
    T->sheet_info(s, &sw, &sh_h, &fw, &fh);
    if (fw <= 0 || fh <= 0) return;
    const int cols = sw / fw;
    const int dh = (int)h, dw = maxi(1, (int)(h * fw / fh));
    const int x0 = (int)(cx - dw / 2.0f), y0 = (int)bottom - dh;
    if (y0 >= VH || y0 + dh <= 0 || x0 >= VW || x0 + dw <= 0) return;
    const int fx = (frame % cols) * fw, fy = (frame / cols) * fh;
    for (int dy = 0; dy < dh; dy++) {
        const int y = y0 + dy;
        if (y < 0 || y >= VH) continue;
        const uint8_t *row = px + (fy + dy * fh / dh) * sw + fx;
        uint8_t *out = g.view + y * VW;
        for (int dx = 0; dx < dw; dx++) {
            const int x = x0 + dx;
            if (x < 0 || x >= VW) continue;
            int sx = dx * fw / dw;
            if (flip) sx = fw - 1 - sx;
            const uint8_t c = row[sx];
            if (c) out[x] = c;
        }
    }
}

static void draw_tree(float cx, float bottom, float scale, int kind)
{
    blit_scaled(g.trees, kind & 1, cx, bottom, scale * 2600.0f * VC, false);
}

static void draw_bush(float cx, float bottom, float scale)
{
    blit_scaled(g.bush, 0, cx, bottom, scale * 700.0f * VC, false);
}

// The start/finish gantry: two towers and a beam across the road with the lights on it.
static void draw_gantry(float cx, float bottom, float scale)
{
    const float road = scale * ROAD_W * VC;
    const int h = (int)(scale * 2600.0f * VC);
    if (h < 6) return;
    const int post = maxi(1, (int)(road * 0.07f)), beam = maxi(2, h / 5);
    const int xl = (int)(cx - road * 1.22f), xr = (int)(cx + road * 1.22f), top = (int)bottom - h;
    rect(xl, top, post, h, P_GREY);
    rect(xr - post, top, post, h, P_GREY);
    rect(xl, top, xr - xl, beam, P_WING);
    /* checkered strip along the beam, and the start lights hanging under it */
    const int sq = maxi(1, beam / 3);
    for (int x = xl + post; x < xr - post; x += sq)
        rect(x, top + 1, sq, sq, ((x - xl) / sq) % 2 ? (uint8_t)P_WHITE : (uint8_t)P_BLACK);
    const int lamp = maxi(1, beam / 3);
    for (int k = -2; k <= 2; k++)
        rect((int)cx + k * lamp * 2 - lamp / 2, top + beam, lamp, lamp,
             g.phase == COUNTDOWN ? (uint8_t)P_RED : (uint8_t)P_GREEN);
}

static void draw_stand(float cx, float bottom, float scale)
{
    blit_scaled(g.stand, 0, cx, bottom, scale * 1500.0f * VC, false);
}

static void draw_sign(float cx, float bottom, float scale, bool points_right)
{
    blit_scaled(g.sign, 0, cx, bottom, scale * 1500.0f * VC, !points_right);
}

static void draw_road(void)
{
    const int n_segs = g.n_segs;
    const int base = (int)(g.pos / SEG_LEN) % n_segs;
    const float base_pct = fmodf(g.pos, SEG_LEN) / SEG_LEN;
    const float cam_x = g.px * ROAD_W;
    float x = 0, dx = -g.segs[base].curve * base_pct;
    int max_y = VH;

    for (int n = 0; n < DRAW_DIST; n++) {
        const int i = (base + n) % n_segs;
        const float z_near = n * SEG_LEN - base_pct * SEG_LEN, z_far = z_near + SEG_LEN;
        g.proj_ok[n] = z_near > CAM_DEPTH * 40;
        if (g.proj_ok[n]) {
            g.proj_near[n] = project(-x, z_near, cam_x);
            g.proj_far[n] = project(-x - dx, z_far, cam_x);
        }
        x += dx;
        dx += g.segs[i].curve;
        if (!g.proj_ok[n] || g.proj_far[n].y >= max_y) continue;
        draw_segment(&g.proj_near[n], &g.proj_far[n], i, max_y);
        max_y = mini(max_y, (int)g.proj_far[n].y);
    }

    /* scenery and rivals, far to near so closer things cover farther ones */
    for (int n = DRAW_DIST - 1; n >= 1; n--) {
        if (!g.proj_ok[n]) continue;
        const int seg_i = (base + n) % n_segs;
        for (int k = g.prop_first[seg_i]; k < g.n_props && g.props[k].seg == seg_i; k++) {
            const Projected *a = &g.proj_near[n];
            const float sx = a->x + a->scale * g.props[k].offset * ROAD_W * VC;
            switch (g.props[k].kind) {
            case PROP_TREE: draw_tree(sx, a->y, a->scale, seg_i + k); break;
            case PROP_BUSH: draw_bush(sx, a->y, a->scale); break;
            case PROP_SIGN: draw_sign(sx, a->y, a->scale, g.segs[seg_i].curve > 0); break;
            case PROP_GANTRY: draw_gantry(a->x, a->y, a->scale); break;
            case PROP_STAND: draw_stand(sx, a->y, a->scale); break;
            }
        }
        const float seg_z0 = fmodf(((int)(g.pos / SEG_LEN) + n) * SEG_LEN, g.track_len);
        for (int ri = 0; ri < RIVALS; ri++) {
            const Car *c = &g.rivals[ri];
            float rel = c->z - seg_z0;
            if (rel < 0 && rel > -SEG_LEN * 0.001f) rel = 0;
            if (rel < 0 || rel >= SEG_LEN) continue;
            const float t = rel / SEG_LEN;
            const Projected *a = &g.proj_near[n], *b = &g.proj_far[n];
            const float scale = a->scale + (b->scale - a->scale) * t;
            const float sx = a->x + (b->x - a->x) * t + scale * c->offset * ROAD_W * VC;
            const float sy = a->y + (b->y - a->y) * t;
            draw_car(sx, sy, scale * 620.0f * VC, c->color, 0, true);
        }
    }
}

static void draw_hud(void)
{
    char buf[24];
    snprintf(buf, sizeof(buf), "%d", (int)(g.speed / MAX_SPEED * 320));
    text_centered(VC, 20, buf, P_WHITE, 2);
    text_centered(VC, 37, "KM/H", P_WHITE, 1);
    snprintf(buf, sizeof(buf), "P%d", g.place);
    text(48, 56, buf, P_YELLOW, 2);
    snprintf(buf, sizeof(buf), "L%d/%d", g.lap, LAPS);
    text(VW - 48 - (int)strlen(buf) * 12, 56, buf, P_WHITE, 2);
    snprintf(buf, sizeof(buf), "%.1f", g.lap_t);
    text_centered(VC, 50, buf, P_WHITE, 1);

    /* steering indicator: a little mark showing how far you have turned */
    const int bar = (int)(g.steer * 28);
    if (g.pitch_sens) {   /* throttle gauge beside the speed */
        const int h = (int)(g.throttle * 22);
        rect(VC + 34, 18, 4, 24, P_PANEL);
        rect(VC + 34, 18 + 24 - h - 1, 4, h + 1, g.throttle > 0.95f ? (uint8_t)P_RED : (uint8_t)P_GREEN);
    }
    rect(VC - 30, 234, 60, 3, P_PANEL);
    rect(VC + mini(0, bar), 234, (bar < 0 ? -bar : bar) + 1, 3,
         fabsf(g.steer) > 0.98f ? (uint8_t)P_RED : (uint8_t)P_GREEN);
}

static void panel(int y, int h) { rect(38, y, VW - 76, h, P_PANEL); }

static void draw_overlay(void)
{
    char buf[32];
    switch (g.phase) {
    case READY:
        panel(112, 62);
        text_centered(VC, 118, "TAP TO RACE", P_WHITE, 2);
        text_centered(VC, 140, "TURN THE WATCH TO STEER", P_YELLOW, 1);
        text_centered(VC, 152, g.pitch_sens ? "TIP FORWARD = FASTER" : "SPEED IS AUTOMATIC", P_YELLOW, 1);
        text_centered(VC, 164, "TOUCH TO PAUSE", P_WHITE, 1);
        break;
    case COUNTDOWN:
        snprintf(buf, sizeof(buf), "%d", g.countdown);
        panel(108, 50);
        text_centered(VC, 116, buf, P_RED, 5);
        break;
    case RACING:
        if (g.race_t < 1.0f) {
            panel(108, 50);
            text_centered(VC, 116, "GO!", P_GREEN, 5);
        }
        break;
    case FINISHED:
        panel(100, 84);
        snprintf(buf, sizeof(buf), "FINISHED P%d", g.place);
        text_centered(VC, 106, buf, g.place == 1 ? (uint8_t)P_GREEN : (uint8_t)P_WHITE, 2);
        snprintf(buf, sizeof(buf), "BEST LAP %.1f", g.best_lap);
        text_centered(VC, 130, buf, P_YELLOW, 1);
        snprintf(buf, sizeof(buf), "RACES WON %d", g.wins);
        text_centered(VC, 144, buf, P_WHITE, 1);
        text_centered(VC, 164, "TAP TO RACE AGAIN", P_WHITE, 1);
        break;
    }
}

// ---------------------------------------------------------------- the game

static bool load_sheet(tat_sheet_t **out, const char *name, int fw, int fh)
{
    size_t len = 0;
    const void *png = T->asset(name, &len);
    if (!png) {
        T->log("missing asset %s", name);
        return false;
    }
    *out = T->sheet_load(g.cv, png, len, fw, fh);
    return *out != NULL;
}

static void rc_begin(const tat_api_t *api)
{
    T = api;
    memset(&g, 0, sizeof(g));

    g.cv = T->canvas_create(2, VW);
    if (!g.cv) {
        T->log("no memory for the view");
        return;
    }
    g.view = T->canvas_pixels(g.cv);

    g.segs = T->alloc(sizeof(Segment) * MAX_SEGS);
    g.props = T->alloc(sizeof(Prop) * MAX_PROPS);
    g.prop_first = T->alloc(sizeof(int) * (MAX_SEGS + 1));
    if (!g.segs || !g.props || !g.prop_first) {
        T->log("no memory for the track");
        g.view = NULL;   /* update and draw both bail on this */
        return;
    }

    // The palette goes in before any sheet is loaded, so the sheets' colours land on these
    // exact entries. draw_car compares pixels against P_CAR0 and P_FLAME_A by number.
    tat_color_t pal[P_COUNT];
    for (int i = 0; i < P_COUNT; i++)
        pal[i] = T->rgb(PALETTE_RGB[i][0], PALETTE_RGB[i][1], PALETTE_RGB[i][2]);
    T->canvas_set_palette(g.cv, pal, P_COUNT);

    bool ok = true;
    ok &= load_sheet(&g.car_sheet, "car.png", CAR_W, CAR_H);
    ok &= load_sheet(&g.trees, "trees.png", 24, 32);
    ok &= load_sheet(&g.sign, "sign.png", 24, 18);
    ok &= load_sheet(&g.stand, "stand.png", 48, 30);
    ok &= load_sheet(&g.bush, "bush.png", 16, 10);
    if (!ok) T->log("some sprites failed to load");
    if (g.car_sheet) T->sheet_info(g.car_sheet, &g.car_sheet_w, NULL, NULL, NULL);

    g.grav_y = 1;
    g.throttle = 1;
    g.pitch_sens = 2;
    int mirror = 0;
    T->save_get("mirror", &mirror, 2);
    g.mirror = mirror != 0;
    T->save_get("pitch", &g.pitch_sens, 4);
    T->save_get("wins", &g.wins, 0);
    new_race();
}

static void rc_enter(void)
{
    g.fps_t0 = T->now_us();
    g.fps_frames = 0;
    if (g.phase == RACING) T->menu_open();   /* came back from the home screen mid-race */
}

static void rc_update(float dt)
{
    if (!g.view) return;
    if (dt > 1.0f / 20) dt = 1.0f / 20;

    const tat_input_t *in = T->input();
    const tat_gestures_t *ges = T->gestures();
    g.phase_t += dt;
    update_steering(in, dt);

    if (T->menu_is_open()) {
        // The touch that paused the game is still down when the menu appears; its release
        // must not count as a tap on whatever row is under the finger.
        if (g.menu_swallow) {
            if (in->touch.released || !in->touch.down) g.menu_swallow = false;
            return;
        }
        switch (T->menu_update()) {
        case 0:
            g.mirror = !g.mirror;
            save_settings();
            T->menu_invalidate();
            break;
        case 1:
            g.pitch_sens = (g.pitch_sens + 1) % 4;
            g.pitch_neutral = g.pitch;
            save_settings();
            T->menu_invalidate();
            break;
        case 2: T->menu_toggle_sound(); break;
        case 3:
            new_race();   /* fresh circuit, back on the grid */
            T->menu_close();
            break;
        }
        if (!T->menu_is_open()) g.pitch_neutral = g.pitch;   /* your grip may have changed */
        return;
    }
    // Touching the screen mid-race pauses: you are holding a steering wheel, not looking
    // for a button. Swipe left works everywhere.
    if (ges->swipe_left || (in->touch.pressed && (g.phase == RACING || g.phase == COUNTDOWN))) {
        T->menu_open();
        g.menu_swallow = in->touch.down;
        return;
    }

    switch (g.phase) {
    case READY:
        if (ges->tap) {
            g.pitch_neutral = g.pitch;   /* however you are holding it now is "cruising" */
            g.phase = COUNTDOWN;
            g.phase_t = 0;
            g.countdown = 3;
            sfx_beep(false);
        }
        break;
    case COUNTDOWN:
        if (g.phase_t >= 1.0f) {
            g.phase_t = 0;
            g.countdown--;
            sfx_beep(g.countdown == 0);
            if (g.countdown == 0) g.phase = RACING;
        }
        break;
    case RACING: update_race(in, dt); break;
    case FINISHED:
        g.speed = maxf(0.0f, g.speed - MAX_SPEED * 0.4f * dt);
        g.pos = fmodf(g.pos + g.speed * dt, g.track_len);
        if (ges->tap && g.phase_t > 1.0f) new_race();
        break;
    }
}

static void rc_draw(void)
{
    if (!g.view) return;

    if (T->menu_is_open()) {
        static const char *const kSens[4] = {"OFF", "LOW", "MED", "HIGH"};
        const tat_menu_row_t rows[] = {
            {"STEERING", g.mirror ? "MIRROR" : "NORMAL", 0},
            {"TILT SPEED", kSens[g.pitch_sens], g.pitch_sens ? 0 : T->ui_color(TAT_UI_DIM)},
            T->menu_sound_row(),
            {"RESTART GAME", "GO", T->ui_color(TAT_UI_ACCENT)},
        };
        T->menu_draw(rows, 4, "PAUSED");
        return;
    }

    draw_background();
    draw_road();
    /* your car: leans into the turn, and shudders a touch at speed */
    const float wobble = (g.speed > MAX_SPEED * 0.6f) ? ((T->random() & 1) ? 0.5f : -0.5f) : 0;
    draw_car(VC + g.steer * 5, 224 + wobble, 70, P_CAR_PLAYER, g.steer,
             g.throttle > 0.55f && g.speed > MAX_SPEED * 0.1f);
    draw_hud();
    draw_overlay();

    /* rotate the upright scene by the roll of the watch onto the round screen */
    T->canvas_present_rotated(g.cv, g.roll);

    g.fps_frames++;
    const int64_t now = T->now_us();
    if (now - g.fps_t0 > 5000000) {
        T->log("%.1f fps, throttle %.2f", g.fps_frames * 1e6f / (float)(now - g.fps_t0), g.throttle);
        g.fps_frames = 0;
        g.fps_t0 = now;
    }
}

static bool rc_keep_awake(void)
{
    return !T->menu_is_open() && (g.phase == RACING || g.phase == COUNTDOWN);
}

static void rc_unload(void)
{
    T->free(g.segs);
    T->free(g.props);
    T->free(g.prop_first);
    if (g.cv) T->canvas_destroy(g.cv);
    g.segs = NULL;
    g.props = NULL;
    g.prop_first = NULL;
    g.cv = NULL;
    g.view = NULL;
}

const tat_game_t tat_game = {
    .magic = TAT_GAME_MAGIC,
    .api_major = TAT_API_MAJOR,
    .api_minor = TAT_API_MINOR,
    .id = "racer",
    .name = "GRAND PRIX",
    .accent_r = 255, .accent_g = 70, .accent_b = 70,
    .assets = tat_assets,
    .asset_count = 5,
    .begin = rc_begin,
    .enter = rc_enter,
    .update = rc_update,
    .draw = rc_draw,
    .leave = NULL,
    .unload = rc_unload,
    .redraw = NULL,   /* the scene is redrawn from scratch every frame */
    .keep_awake = rc_keep_awake,
};
