// STARFALL - on a raider's tail down a canyon of steel, through whatever is in the way.
//
// You are flying a fighter down a trench at speed. There is a raider ahead of you that does
// not want to be caught, and there are blast gates coming down the trench with one opening
// in each. Tilting the watch flies your ship, and your ship has mass: it gathers way, it
// overshoots, it has to be caught and steadied. Your guns point where your ship points,
// which is the middle of the screen - so the same stick that holds the raider in your sight
// has to put you through the gap in the next gate, and those two jobs pull against each
// other. That is the game.
//
//   tilt        - fly. The ship drifts; flying it is catching the drift
//   tap         - fire. With a lock it is a kill and time back; without one it costs time
//   swipe left  - pause menu
//
// It comes in levels. Each is a sortie: so many raiders to bring down before the clock runs
// out, on three shields, and a gate through the wing costs one. Clear it and the next has
// more raiders, quicker ones, a smaller sight, and gates that come oftener with less room.
//
// It is a hybrid of two things. The chase - a ship with inertia steered by tilt, a target
// zone in the middle, a tap that only counts when you are lined up, a countdown - is the
// idea of "X-Wing Pursuit" by The Last Outpost Workshop (MIT), turned inside out so that
// the thing you line up with moves. The gates are what was good in the first STARFALL,
// which was a tunnel seen head on with rings to find the gap in. None of the first game's
// code is here, and none of its art could be: those ships belong to somebody else. These
// are ours.
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
#define CY (CC - 6)        /* the eye is a little above the middle, so more floor shows than sky */
#define PI 3.14159265f

// The trench, in its own units, seen through a lens of FOCAL from wherever the ship is: half
// as wide as TRENCH_W, its floor TRENCH_H below the middle and its walls WALL_TOP above it.
// At RAIDER_Z one unit is one pixel, which is where the raider flies.
#define FOCAL 96.0f
#define TRENCH_W 78.0f
#define TRENCH_H 54.0f
#define WALL_TOP 54.0f
#define SHIP_Z 40.0f
#define RAIDER_Z 96.0f
#define FAR_Z 560.0f
#define STRIPE 28.0f       /* the plating is banded every this many units, which is what shows the speed */
#define FLY_SPEED 175.0f   /* units a second: a gate is in sight for about three seconds */

// Flying. Tilt is a force, not a position.
#define THRUST 250.0f      /* units/s^2 at a full tilt */
#define FULL_TILT 0.40f    /* g of tilt, away from level, that counts as full */
#define DRAG 1.9f          /* 1/s: left alone, the ship loses most of its way in a second */
#define REACH_X 60.0f      /* how far from the middle of the trench the ship can get */
#define REACH_Y 38.0f

#define LEVEL_S 30.0f
#define MISS_COST_S 1.5f
#define LOCK_S 0.20f       /* how long the raider has to stay in the sight before it is a lock */
#define SHIELDS 3
#define MAX_GATES 4
#define MAX_PARTS 48

typedef enum { READY, FLYING, CLEARED, OVER } Phase;
typedef enum { OUT_OF_TIME, SHOT_DOWN } Ending;

typedef struct { float x, y, vx, vy, life; uint8_t col; } Part;
typedef struct { float z, hx, hy, hw, hh; bool alive; } Gate;   /* the opening, in trench units */

static struct {
    tat_canvas_t *cv;

    uint8_t c_space, c_star, c_floor[3][2], c_wall[3][2], c_end, c_rim;
    uint8_t c_gate[3], c_gate_edge;
    uint8_t c_hull, c_hull_dk, c_wing, c_engine, c_engine_hi;       /* ours */
    uint8_t c_raider, c_raider_dk, c_raider_eng, c_raider_eye;      /* theirs */
    uint8_t c_sight, c_lock, c_bolt, c_bolt_hi, c_fire[3];
    uint8_t c_text, c_dim, c_accent, c_danger, c_go, c_panel, c_shield;

    Phase phase;
    float phase_t;
    Ending ending;

    int level, kills, quota;       /* kills this level, of how many */
    int score, best, best_level;
    int shields;
    float clock;

    float x, y, vx, vy;            /* our ship, in trench units from the middle */
    float level_x, level_y;        /* what the tilt reads when the watch is held "level" */
    float rx, ry;                  /* the raider, likewise */
    float weave_t, jink_t, jink_x, jink_y;
    float scroll;
    float lock_t;
    bool locked;
    int invert;

    Gate gates[MAX_GATES];
    float gate_in;                 /* seconds until the next one */

    float bolt_t;
    bool bolt_hit;
    float boom_t, boom_x, boom_y;
    float flash, hurt, shake;
    float gain_t;                  /* "+3S" floating up */
    int gain;

    Part parts[MAX_PARTS];
    uint8_t col_floor[CW + 1], col_left[CW + 1], col_right[CW + 1];
    uint32_t seed;
    float anim_t;
    bool dirty;
} g;

// ---------------------------------------------------------------- odds and ends

static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

static float frand(float lo, float hi)
{
    g.seed = g.seed * 1664525u + 1013904223u;
    return lo + (hi - lo) * ((g.seed >> 8 & 0xFFFF) / 65535.0f);
}

static uint32_t hash(uint32_t a)
{
    a ^= a >> 16;
    a *= 0x7feb352du;
    a ^= a >> 15;
    a *= 0x846ca68bu;
    return a ^ (a >> 16);
}

static void rect(int x, int y, int w, int h, uint8_t c)
{
    if (w > 0 && h > 0) T->canvas_fill_rect(g.cv, x, y, w, h, c);
}

// Where a point in the trench lands on the screen, seen from the ship.
static int jolt(void) { return g.shake > 0 ? (int)(sinf(g.anim_t * 95.0f) * g.shake * 3.0f) : 0; }
static int px_of(float wx, float z) { return CC + (int)floorf((wx - g.x) * FOCAL / z); }
static int py_of(float wy, float z) { return CY + jolt() + (int)floorf((wy - g.y) * FOCAL / z); }

// ---------------------------------------------------------------- sound

static void tone1(float f0, float f1, int ms, int wave, float vol, int delay)
{
    const tat_tone_t t = {f0, f1, (uint16_t)ms, (uint8_t)wave, vol, (uint16_t)delay};
    T->tone(&t);
}
static void sfx_lock(void) { tone1(1320, 0, 45, TAT_SQUARE, 0.3f, 0); }
static void sfx_fire(void)
{
    tone1(1500, 300, 130, TAT_SQUARE, 0.5f, 0);
    tone1(1300, 260, 130, TAT_SQUARE, 0.35f, 30);
}
static void sfx_boom(void)
{
    tone1(220, 40, 420, TAT_NOISE, 0.8f, 0);
    tone1(110, 50, 300, TAT_SQUARE, 0.5f, 40);
}
static void sfx_miss(void) { tone1(240, 120, 120, TAT_SQUARE, 0.4f, 120); }
static void sfx_tick(void) { tone1(880, 0, 30, TAT_TRIANGLE, 0.35f, 0); }
static void sfx_start(void) { tone1(330, 990, 160, TAT_TRIANGLE, 0.6f, 0); }
static void sfx_gate_hit(void)
{
    tone1(160, 50, 300, TAT_NOISE, 0.9f, 0);
    tone1(90, 60, 260, TAT_SQUARE, 0.6f, 0);
}
static void sfx_gate_pass(void) { tone1(500, 260, 90, TAT_NOISE, 0.25f, 0); }
static void sfx_cleared(void)
{
    static const float n[] = {523, 659, 784, 1047};
    for (int i = 0; i < 4; i++) tone1(n[i], 0, i == 3 ? 300 : 110, TAT_TRIANGLE, 0.55f, i * 120);
}
static void sfx_over(void)
{
    tone1(392, 0, 150, TAT_SQUARE, 0.6f, 0);
    tone1(311, 0, 150, TAT_SQUARE, 0.6f, 150);
    tone1(233, 0, 340, TAT_SQUARE, 0.6f, 300);
}

// ---------------------------------------------------------------- how hard it is

// Everything that makes a level harder than the last, in one place.
static int level_quota(int level) { return level + 3 > 10 ? 10 : level + 3; }
static float weave_speed(void) { return 0.85f + 0.10f * (float)(g.level < 12 ? g.level : 12) + 0.03f * (float)g.kills; }
static float weave_reach_x(void) { return clampf(30.0f + 4.0f * (float)g.level, 0, REACH_X); }
static float sight_r(void) { return clampf(22.0f - 1.3f * (float)g.level, 11.0f, 22.0f); }
static float time_back(void) { return g.level < 4 ? 4.0f : g.level < 8 ? 3.0f : 2.0f; }
static float gate_every(void) { return clampf(6.2f - 0.5f * (float)g.level, 2.2f, 6.2f); }
static float gate_half_w(void) { return clampf(36.0f - 2.0f * (float)g.level, 20.0f, 36.0f); }
static float gate_half_h(void) { return clampf(27.0f - 1.2f * (float)g.level, 17.0f, 27.0f); }

// ---------------------------------------------------------------- the run

// It comes in somewhere that is not where you are pointing, so every kill starts a chase.
static void new_raider(void)
{
    const float a = frand(0, 2 * PI), far_out = frand(40.0f, 58.0f);
    g.rx = clampf(g.x + cosf(a) * far_out, -REACH_X, REACH_X);
    g.ry = clampf(g.y + sinf(a) * far_out * 0.7f, -REACH_Y, REACH_Y);
    g.weave_t = frand(0, 20.0f);
    g.jink_t = frand(1.2f, 2.6f);
    g.jink_x = g.jink_y = 0;
    g.lock_t = 0;
    g.locked = false;
}

static void puff(float x, float y, float vx, float vy, float life, uint8_t col)
{
    for (int i = 0; i < MAX_PARTS; i++)
        if (g.parts[i].life <= 0) {
            g.parts[i] = (Part){x, y, vx, vy, life, col};
            return;
        }
}

static void begin_level(int level)
{
    g.level = level;
    g.kills = 0;
    g.quota = level_quota(level);
    g.clock = LEVEL_S;
    g.bolt_t = g.boom_t = g.gain_t = 0;
    for (int i = 0; i < MAX_GATES; i++) g.gates[i].alive = false;
    g.gate_in = 3.5f;   /* a breath to find the raider before the first gate */
    new_raider();
}

static void new_run(void)
{
    g.x = g.y = g.vx = g.vy = 0;
    g.score = 0;
    g.shields = SHIELDS;
    g.flash = g.hurt = g.shake = 0;
    memset(g.parts, 0, sizeof(g.parts));
    begin_level(1);
    g.phase = READY;
    g.phase_t = 0;
    g.dirty = true;
}

static void end_run(Ending how)
{
    g.ending = how;
    g.phase = OVER;
    g.phase_t = 0;
    if (g.score > g.best) {
        g.best = g.score;
        T->save_set("score", g.best);
    }
    if (g.level > g.best_level) {
        g.best_level = g.level;
        T->save_set("level", g.best_level);
    }
    T->log("%s on level %d: %d points, best %d", how == SHOT_DOWN ? "shot down" : "out of time", g.level, g.score, g.best);
    sfx_over();
}

static void raider_screen(float *sx, float *sy)
{
    *sx = (float)px_of(g.rx, RAIDER_Z);
    *sy = (float)py_of(g.ry, RAIDER_Z);
}

// ---------------------------------------------------------------- flying

static void fly(const tat_input_t *in, float dt)
{
    // The tilt, away from however the watch was being held when the run began, is thrust.
    float tx = (in->tilt.ax - g.level_x) / FULL_TILT;
    float ty = (in->tilt.ay - g.level_y) / FULL_TILT;
    if (g.invert) ty = -ty;
#ifdef SF_AUTOPILOT
    // Flying it over Wi-Fi is not possible - a tilt arrives a second late - so a test build
    // flies itself: at the opening of the next gate when one is close, otherwise at the
    // raider, and it fires on a lock. No shipped build defines this.
    {
        float want_x = g.rx, want_y = g.ry;
        for (int i = 0; i < MAX_GATES; i++)
            if (g.gates[i].alive && g.gates[i].z > SHIP_Z && g.gates[i].z < 300.0f) {
                want_x = clampf(g.rx, g.gates[i].hx - g.gates[i].hw + 12, g.gates[i].hx + g.gates[i].hw - 12);
                want_y = clampf(g.ry, g.gates[i].hy - g.gates[i].hh + 10, g.gates[i].hy + g.gates[i].hh - 10);
            }
        tx = ((want_x - g.x) * 3.0f - g.vx * 1.3f) / 60.0f;
        ty = ((want_y - g.y) * 3.0f - g.vy * 1.3f) / 60.0f;
    }
#endif
    const float m = sqrtf(tx * tx + ty * ty);
    if (m > 1) tx /= m, ty /= m;
    if (m < 0.06f) tx = ty = 0;

    g.vx += tx * THRUST * dt;
    g.vy += ty * THRUST * dt;
    const float keep = expf(-DRAG * dt);
    g.vx *= keep;
    g.vy *= keep;
    g.x += g.vx * dt;
    g.y += g.vy * dt;
    // The trench has walls, and leaning on one takes the way off the ship.
    if (g.x < -REACH_X || g.x > REACH_X) {
        g.x = clampf(g.x, -REACH_X, REACH_X);
        g.vx *= -0.3f;
    }
    if (g.y < -REACH_Y || g.y > REACH_Y) {
        g.y = clampf(g.y, -REACH_Y, REACH_Y);
        g.vy *= -0.3f;
    }

    // The raider weaves in a slow figure that never quite repeats, and every so often
    // breaks off it altogether - which is the moment a lock is lost.
    g.weave_t += dt * weave_speed();
    const float reach = weave_reach_x();
    float wx = sinf(g.weave_t) * reach + sinf(g.weave_t * 2.3f + 1.0f) * reach * 0.3f;
    float wy = sinf(g.weave_t * 1.4f + 2.0f) * reach * 0.5f + cosf(g.weave_t * 0.7f) * reach * 0.2f;
    g.jink_t -= dt;
    if (g.jink_t <= 0) {
        g.jink_t = frand(1.6f, 3.4f) / (0.8f + 0.07f * (float)g.level);
        g.jink_x = frand(-24.0f, 24.0f);
        g.jink_y = frand(-16.0f, 16.0f);
    }
    wx = clampf(wx + g.jink_x, -REACH_X, REACH_X);
    wy = clampf(wy + g.jink_y, -REACH_Y, REACH_Y);
    const float ease = 1.0f - expf(-dt * 3.2f);
    g.rx += (wx - g.rx) * ease;
    g.ry += (wy - g.ry) * ease;

    // In the sight, and staying there, is a lock.
    const float dx = g.rx - g.x, dy = g.ry - g.y;   /* at the raider's distance a unit is a pixel */
    const bool in_sight = g.boom_t <= 0 && dx * dx + dy * dy < sight_r() * sight_r();
    if (in_sight) {
        g.lock_t += dt;
        if (!g.locked && g.lock_t >= LOCK_S) {
            g.locked = true;
            sfx_lock();
        }
    } else {
        g.lock_t = 0;
        g.locked = false;
    }
}

// Gates come down the trench at the speed the plating does. Each has one opening, put
// somewhere the ship can get to in the time there is, and the moment one reaches the ship
// either the ship is in the opening or it is not.
static void run_gates(float dt)
{
    g.gate_in -= dt;
    if (g.gate_in <= 0) {
        g.gate_in = gate_every() * frand(0.8f, 1.25f);
        for (int i = 0; i < MAX_GATES; i++) {
            Gate *gt = &g.gates[i];
            if (gt->alive) continue;
            gt->alive = true;
            gt->z = FAR_Z;
            gt->hw = gate_half_w();
            gt->hh = gate_half_h();
            gt->hx = frand(-(TRENCH_W - gt->hw - 6), TRENCH_W - gt->hw - 6);
            gt->hy = frand(-(WALL_TOP - gt->hh - 6), TRENCH_H - gt->hh - 6);
            break;
        }
    }
    for (int i = 0; i < MAX_GATES; i++) {
        Gate *gt = &g.gates[i];
        if (!gt->alive) continue;
        const float before = gt->z;
        gt->z -= FLY_SPEED * dt;
        if (before > SHIP_Z && gt->z <= SHIP_Z) {
            // A little kinder than it looks: the wingtips may brush.
            const bool through = fabsf(g.x - gt->hx) < gt->hw - 5.0f && fabsf(g.y - gt->hy) < gt->hh - 4.0f;
            if (through) {
                g.score += 10 * g.level;
                sfx_gate_pass();
            } else {
                g.shields--;
                g.hurt = 1.0f;
                g.shake = 1.0f;
                g.vx *= 0.3f;
                g.vy *= 0.3f;
                sfx_gate_hit();
                for (int k = 0; k < 18; k++)
                    puff(CC + frand(-30, 30), CW - 50 + frand(-16, 10), frand(-120, 120), frand(-140, 10), frand(0.3f, 0.7f),
                         g.c_fire[k % 3]);
                T->log("hit a gate on level %d, %d shields left", g.level, g.shields);
                if (g.shields <= 0) {
                    end_run(SHOT_DOWN);
                    return;
                }
            }
        }
        if (gt->z < SHIP_Z - 14.0f) gt->alive = false;
    }
}

static void fire(void)
{
    if (g.bolt_t > 0 || g.boom_t > 0) return;
    g.bolt_t = 0.16f;
    g.bolt_hit = g.locked;
    sfx_fire();
    if (g.locked) {
        raider_screen(&g.boom_x, &g.boom_y);
        g.boom_t = 0.7f;
        g.flash = 1.0f;
        g.kills++;
        g.score += 100 * g.level;
        g.gain = (int)time_back();
        g.clock += (float)g.gain;
        if (g.clock > LEVEL_S) g.clock = LEVEL_S;   /* a full clock is as good as it gets */
        g.gain_t = 1.0f;
        g.locked = false;
        g.lock_t = 0;
        sfx_boom();
        for (int i = 0; i < 26; i++) {
            const float a = frand(0, 2 * PI), v = frand(20.0f, 130.0f);
            puff(g.boom_x, g.boom_y, cosf(a) * v, sinf(a) * v, frand(0.3f, 0.8f), g.c_fire[i % 3]);
        }
        T->log("level %d kill %d of %d, %.1f s left", g.level, g.kills, g.quota, (double)g.clock);
    } else {
        // A shot into nothing. It costs time, or the game would be tapping as fast as you can.
        g.clock -= MISS_COST_S;
        g.gain = -(int)(MISS_COST_S + 0.5f);
        g.gain_t = 1.0f;
        sfx_miss();
    }
}

// ---------------------------------------------------------------- drawing

// The trench is worked out a pixel at a time, which is cheaper than it sounds. From how far
// a pixel is below the middle you know how far away that bit of floor is; from how far it is
// to the side you know the same for the wall on that side. Both come from tables built once
// a frame, so a pixel costs a couple of multiplies and a lookup. The ship moving sideways
// does not move the vanishing point - it brings one wall nearer and sends the other away,
// which is what makes drifting toward a wall feel like drifting toward a wall. The plating
// is banded along its length, and the bands sliding past are what make it feel fast.
static uint8_t plating(uint8_t shade[3][2], float z)
{
    if (z > FAR_Z) return g.c_end;
    return shade[z < 130 ? 0 : z < 270 ? 1 : 2][((int)((z + g.scroll) / STRIPE)) & 1];
}

static void draw_trench(void)
{
    const float floor_h = TRENCH_H - g.y, top_h = WALL_TOP + g.y;
    const float left_w = TRENCH_W + g.x, right_w = TRENCH_W - g.x;
    for (int d = 1; d <= CW; d++) {
        g.col_floor[d] = plating(g.c_floor, floor_h * FOCAL / (float)d);
        g.col_left[d] = plating(g.c_wall, left_w * FOCAL / (float)d);
        g.col_right[d] = plating(g.c_wall, right_w * FOCAL / (float)d);
    }

    const int cy = CY + jolt();
    uint8_t *px = T->canvas_pixels(g.cv);
    for (int y = 0; y < CW; y++) {
        const float dy = (float)(y - cy);
        uint8_t *row = px + y * CW;
        for (int x = 0; x < CW; x++) {
            const int sdx = x - CC;
            const int dx = sdx < 0 ? -sdx : sdx;
            const float wall_w = sdx < 0 ? left_w : right_w;
            uint8_t c;
            if (dy > 0 && (float)dx * floor_h < dy * wall_w) {
                const int i = (int)dy;
                c = g.col_floor[i > CW ? CW : i];
            } else if (dx > 0 && dy * wall_w <= floor_h * (float)dx && -dy * wall_w <= top_h * (float)dx) {
                c = (sdx < 0 ? g.col_left : g.col_right)[dx > CW ? CW : dx];
                if (-dy * wall_w > (top_h - 3.5f) * (float)dx) c = g.c_rim;   /* the lit top edge */
            } else {
                c = g.c_space;
            }
            row[x] = c;
        }
    }
    // Stars, over whatever is left of the sky.
    for (int i = 0; i < 46; i++) {
        const uint32_t h = hash((uint32_t)i + 9);
        const int x = (int)(h % CW) - (int)(g.x * 0.1f), y = (int)(h / 251 % 100) - (int)(g.y * 0.1f);
        if (x < 0 || y < 0 || x >= CW || y >= CW) continue;
        if (px[y * CW + x] == g.c_space) px[y * CW + x] = (h >> 20 & 3) ? g.c_star : g.c_text;
    }
}

// A gate: the whole cross-section of the trench filled in, except the opening. Far away it
// is only an outline, so that it can be seen coming without hiding what is beyond it.
static void draw_gate(const Gate *gt)
{
    const float z = gt->z < 8 ? 8 : gt->z;
    const int l = px_of(-TRENCH_W, z), r = px_of(TRENCH_W, z), t = py_of(-WALL_TOP, z), b = py_of(TRENCH_H, z);
    const int hl = px_of(gt->hx - gt->hw, z), hr = px_of(gt->hx + gt->hw, z);
    const int ht = py_of(gt->hy - gt->hh, z), hb = py_of(gt->hy + gt->hh, z);
    if (z < 330) {
        const uint8_t c = g.c_gate[z < 120 ? 0 : z < 220 ? 1 : 2];
        rect(l, t, r - l, ht - t, c);
        rect(l, hb, r - l, b - hb, c);
        rect(l, ht, hl - l, hb - ht, c);
        rect(hr, ht, r - hr, hb - ht, c);
        // Ribs across it and a heavy frame round the opening, so that it reads as a thing
        // that was built and can be hit, and its size tells how close it is.
        const uint8_t rib = g.c_gate[z < 120 ? 1 : 2];
        const int step = (int)(14.0f * FOCAL / z);
        if (step > 3)
            for (int y = t + step; y < b; y += step) {
                if (y < ht || y > hb) rect(l, y, r - l, 1, rib);
                else {
                    rect(l, y, hl - l, 1, rib);
                    rect(hr, y, r - hr, 1, rib);
                }
            }
        const int fr = (int)(4.0f * FOCAL / z) + 1;
        rect(hl - fr, ht - fr, hr - hl + 2 * fr, fr, rib);
        rect(hl - fr, hb, hr - hl + 2 * fr, fr, rib);
        rect(hl - fr, ht, fr, hb - ht, rib);
        rect(hr, ht, fr, hb - ht, rib);
    } else {
        rect(l, t, r - l, 1, g.c_gate[2]);
        rect(l, b, r - l, 1, g.c_gate[2]);
        rect(l, t, 1, b - t, g.c_gate[2]);
        rect(r, t, 1, b - t, g.c_gate[2]);
    }
    // The opening is edged in light, and brighter as it closes: that is what to steer at.
    const uint8_t e = z < 260 ? g.c_gate_edge : g.c_sight;
    rect(hl, ht, hr - hl, 1, e);
    rect(hl, hb, hr - hl + 1, 1, e);
    rect(hl, ht, 1, hb - ht, e);
    rect(hr, ht, 1, hb - ht, e);
}

// The raider, seen from behind: a narrow body, wings swept down and back, a tail fin, two
// engines burning red. Drawn from lines so that it can roll as it weaves, and drawn large
// and pale against the dark of the trench, because a target nobody can find is not a target.
static void draw_raider(float fx, float fy)
{
    const int x = (int)fx, y = (int)fy;
    const float roll = clampf((g.jink_x + sinf(g.weave_t) * 20.0f) / 60.0f, -0.6f, 0.6f);
    const int tip = (int)(roll * 9.0f);
    for (int k = 0; k < 3; k++) {   /* wings, three lines thick */
        T->canvas_line(g.cv, x - 3, y + k, x - 20, y + 7 + k + tip, g.c_raider);
        T->canvas_line(g.cv, x + 3, y + k, x + 20, y + 7 + k - tip, g.c_raider);
    }
    rect(x - 21, y + 6 + tip, 2, 7, g.c_raider_dk);      /* winglets, turned down */
    rect(x + 19, y + 6 - tip, 2, 7, g.c_raider_dk);
    rect(x - 4, y - 4, 8, 10, g.c_raider);
    rect(x - 3, y - 6, 6, 2, g.c_raider);
    rect(x - 1, y - 13, 2, 8, g.c_raider_dk);            /* tail fin */
    rect(x - 2, y - 3, 4, 3, g.c_raider_eye);
    const bool flick = ((int)(g.anim_t * 30.0f) & 1) != 0;
    rect(x - 7, y + 2, 3, 3, flick ? g.c_raider_eng : g.c_fire[1]);
    rect(x + 4, y + 2, 3, 3, flick ? g.c_fire[1] : g.c_raider_eng);
    rect(x - 6, y + 5, 1, 2, g.c_fire[0]);
    rect(x + 5, y + 5, 1, 2, g.c_fire[0]);
}

// Ours, from behind and below the eye: twin booms, a wide wing that banks with the drift,
// two engines. The bank is the instrument - it tells you which way you are still sliding.
static void draw_ship(void)
{
    const int x = CC + (int)(g.vx * 0.10f), y = CW - 46 + (int)(g.vy * 0.06f) + jolt();
    const int bank = (int)clampf(g.vx * 0.09f, -8.0f, 8.0f);
    const bool hurt = g.hurt > 0 && ((int)(g.anim_t * 24.0f) & 1);
    const uint8_t hull = hurt ? g.c_danger : g.c_hull, wing = hurt ? g.c_danger : g.c_wing;
    for (int k = 0; k < 3; k++) {
        T->canvas_line(g.cv, x - 4, y + k, x - 30, y + 6 + k + bank, wing);
        T->canvas_line(g.cv, x + 4, y + k, x + 30, y + 6 + k - bank, wing);
    }
    rect(x - 30, y + 3 + bank, 2, 7, g.c_hull_dk);
    rect(x + 28, y + 3 - bank, 2, 7, g.c_hull_dk);
    rect(x - 5, y - 6, 10, 13, hull);
    rect(x - 3, y - 9, 6, 3, hull);
    rect(x - 2, y - 5, 4, 4, g.c_hull_dk);             /* the canopy */
    rect(x - 11, y - 1, 4, 10, g.c_hull_dk);           /* booms */
    rect(x + 7, y - 1, 4, 10, g.c_hull_dk);
    const int burn = 3 + ((int)(g.anim_t * 40.0f) & 1) + (int)(sqrtf(g.vx * g.vx + g.vy * g.vy) * 0.03f);
    rect(x - 10, y + 9, 2, burn, g.c_engine_hi);
    rect(x + 8, y + 9, 2, burn, g.c_engine_hi);
    rect(x - 11, y + 9, 4, 2, g.c_engine);
    rect(x + 7, y + 9, 4, 2, g.c_engine);
}

static void draw_sight(void)
{
    const int r = (int)sight_r(), cy = CY + jolt();
    const uint8_t c = g.locked ? g.c_lock : g.c_sight;
    // A ring of dashes, so that what is inside it can still be seen.
    for (int a = 0; a < 24; a++) {
        if (!g.locked && (a & 1)) continue;
        const float t = a * (2 * PI / 24) + (g.locked ? g.anim_t * 4.0f : 0);
        T->canvas_pixel(g.cv, CC + (int)(cosf(t) * r), cy + (int)(sinf(t) * r), c);
        T->canvas_pixel(g.cv, CC + (int)(cosf(t) * (r + 1)), cy + (int)(sinf(t) * (r + 1)), c);
    }
    rect(CC - r - 6, cy, 4, 1, c);
    rect(CC + r + 3, cy, 4, 1, c);
    rect(CC, cy - r - 6, 1, 4, c);
    rect(CC, cy + r + 3, 1, 4, c);
    rect(CC, cy, 1, 1, c);
    if (g.locked) T->canvas_text_centered(g.cv, CC, cy + r + 14, "LOCK", g.c_lock, 1, true);

    // Which way the raider has gone, when it is a long way out: a mark on the edge of the
    // sight, so that a chase always has a direction.
    const float dx = g.rx - g.x, dy = g.ry - g.y, d = sqrtf(dx * dx + dy * dy);
    if (d > 52.0f && g.boom_t <= 0) rect(CC + (int)(dx / d * 38.0f) - 1, cy + (int)(dy / d * 38.0f) - 1, 3, 3, g.c_danger);
}

static void draw_hud(void)
{
    char buf[24];
    snprintf(buf, sizeof(buf), "%d", g.score);
    T->canvas_text_centered(g.cv, CC, 17, buf, g.c_text, 2, true);

    // The clock, as a bar that empties; red when it is nearly gone.
    const int bw = 70, bx = CC - bw / 2, by = 30;
    const bool low = g.clock < 6.0f;
    rect(bx - 1, by - 1, bw + 2, 5, g.c_panel);
    rect(bx, by, (int)(bw * clampf(g.clock / LEVEL_S, 0, 1)), 3, low ? g.c_danger : g.c_go);

    // The sortie: which level, how many of how many, and what is left of the shields.
    snprintf(buf, sizeof(buf), "L%d  %d/%d", g.level, g.kills, g.quota);
    T->canvas_text(g.cv, CC - 6 - T->text_width(buf, 1, true), 39, buf, g.c_dim, 1, true);
    for (int i = 0; i < SHIELDS; i++)
        rect(CC + 8 + i * 9, 40, 6, 5, i < g.shields ? g.c_shield : g.c_panel);

    if (g.gain_t > 0) {
        snprintf(buf, sizeof(buf), g.gain > 0 ? "+%dS" : "%dS", g.gain);
        T->canvas_text_centered(g.cv, CC + 50, 31 - (int)((1.0f - g.gain_t) * 10.0f), buf, g.gain > 0 ? g.c_go : g.c_danger,
                                1, true);
    }
}

static void banner(const char *top, const char *mid, const char *bottom, uint8_t col)
{
    const tat_banner_t b = {
        .top = top, .mid = mid, .bottom = bottom,
        .top_color = col, .mid_color = g.c_text, .bottom_color = g.c_dim,
        .panel = g.c_panel, .border = col,
        .bars = true,
    };
    T->canvas_banner(g.cv, CC, 70, CW - 44, &b);
}

// ---------------------------------------------------------------- the game

static uint8_t col(uint8_t r, uint8_t gg, uint8_t b) { return T->canvas_color(g.cv, T->rgb(r, gg, b)); }

static void sf_begin(const tat_api_t *api)
{
    T = api;
    memset(&g, 0, sizeof(g));
    g.cv = T->canvas_create(SCALE, 0);
    if (!g.cv) {
        T->log("no memory for the trench");
        return;
    }
    g.c_space = col(4, 5, 14);
    g.c_star = col(150, 160, 200);
    // Plating in two tones a band, fading with distance: steel blue floor, greyer walls.
    static const uint8_t FLOOR[3][2][3] = {{{58, 72, 110}, {44, 56, 90}}, {{40, 50, 80}, {31, 40, 66}}, {{26, 33, 54}, {20, 26, 44}}};
    static const uint8_t WALL[3][2][3] = {{{74, 80, 104}, {56, 62, 84}}, {{50, 55, 76}, {39, 44, 62}}, {{31, 35, 52}, {24, 28, 42}}};
    for (int d = 0; d < 3; d++)
        for (int b = 0; b < 2; b++) {
            g.c_floor[d][b] = col(FLOOR[d][b][0], FLOOR[d][b][1], FLOOR[d][b][2]);
            g.c_wall[d][b] = col(WALL[d][b][0], WALL[d][b][1], WALL[d][b][2]);
        }
    g.c_end = col(12, 15, 28);
    g.c_rim = col(150, 170, 215);
    g.c_gate[0] = col(150, 58, 40);
    g.c_gate[1] = col(104, 42, 34);
    g.c_gate[2] = col(70, 32, 30);
    g.c_gate_edge = col(255, 200, 90);
    g.c_hull = col(226, 232, 244);
    g.c_hull_dk = col(120, 132, 160);
    g.c_wing = col(190, 200, 222);
    g.c_engine = col(60, 190, 255);
    g.c_engine_hi = col(200, 245, 255);
    g.c_raider = col(196, 204, 170);
    g.c_raider_dk = col(124, 134, 106);
    g.c_raider_eng = col(255, 70, 60);
    g.c_raider_eye = col(255, 190, 60);
    g.c_sight = col(90, 230, 140);
    g.c_lock = col(255, 70, 80);
    g.c_bolt = col(120, 255, 150);
    g.c_bolt_hi = col(230, 255, 235);
    g.c_fire[0] = col(255, 236, 140);
    g.c_fire[1] = col(255, 150, 50);
    g.c_fire[2] = col(220, 70, 40);
    g.c_text = col(246, 248, 255);
    g.c_dim = col(150, 160, 196);
    g.c_accent = col(120, 240, 255);
    g.c_danger = col(255, 84, 92);
    g.c_go = col(60, 220, 120);
    g.c_panel = col(8, 10, 20);
    g.c_shield = col(90, 200, 255);

    g.seed = (uint32_t)T->now_us() | 1u;
    T->save_get("score", &g.best, 0);   /* the old "best" was a different game's score */
    T->save_get("level", &g.best_level, 0);
    T->save_get("invert", &g.invert, 2);
    new_run();
    T->log("ready, best %d, level %d", g.best, g.best_level);
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
            g.invert = !g.invert;
            T->save_set("invert", g.invert);
            T->menu_invalidate();
            break;
        case 2:
            new_run();
            T->menu_close();
            break;
        }
        if (!T->menu_is_open()) {
            g.level_x = in->tilt.ax;   /* your grip may have changed */
            g.level_y = in->tilt.ay;
            g.dirty = true;
        }
        return;
    }
    if (ges->swipe_left) {
        T->menu_open();
        return;
    }

    g.phase_t += dt;
    g.anim_t += dt;
    g.dirty = true;
    g.scroll += FLY_SPEED * dt;
    if (g.scroll > STRIPE * 4000.0f) g.scroll -= STRIPE * 4000.0f;
    if (g.flash > 0) g.flash -= dt * 4.0f;
    if (g.hurt > 0) g.hurt -= dt * 1.4f;
    if (g.shake > 0) g.shake -= dt * 2.4f;
    if (g.gain_t > 0) g.gain_t -= dt;
    if (g.bolt_t > 0) g.bolt_t -= dt;
    for (int i = 0; i < MAX_PARTS; i++) {
        Part *p = &g.parts[i];
        if (p->life <= 0) continue;
        p->life -= dt;
        p->x += p->vx * dt;
        p->y += p->vy * dt;
        p->vx *= 1.0f - dt;
        p->vy *= 1.0f - dt;
    }

    switch (g.phase) {
    case READY:
        if (ges->tap) {
            g.level_x = in->tilt.ax;   /* however it is being held now is level */
            g.level_y = in->tilt.ay;
            g.phase = FLYING;
            g.phase_t = 0;
            sfx_start();
        }
        break;

    case FLYING:
        fly(in, dt);
        if (in->touch.pressed) fire();
#ifdef SF_AUTOPILOT
        if (g.locked) fire();
#endif
        run_gates(dt);
        if (g.phase != FLYING) break;   /* the last shield went */
        if (g.boom_t > 0 && (g.boom_t -= dt) <= 0) {
            if (g.kills >= g.quota) {
                // The sortie is flown. What is left on the clock is worth something.
                g.score += (int)g.clock * 10 * g.level;
                g.phase = CLEARED;
                g.phase_t = 0;
                for (int i = 0; i < MAX_GATES; i++) g.gates[i].alive = false;
                sfx_cleared();
                T->log("level %d cleared with %.0f s left, score %d", g.level, (double)g.clock, g.score);
                break;
            }
            new_raider();
        }
        g.clock -= dt;
        if (g.clock < 5.0f && (int)(g.clock + dt) != (int)g.clock) sfx_tick();   /* the last five seconds tick */
        if (g.clock <= 0) {
            g.clock = 0;
            end_run(OUT_OF_TIME);
        }
        break;

    case CLEARED:
        // Still flying, nothing to shoot and nothing in the way, while the next is announced.
        fly(in, dt);
        g.locked = false;
        if (g.phase_t > 2.4f) {
            if (g.shields < SHIELDS) g.shields++;   /* a level flown is a shield back */
            begin_level(g.level + 1);
            g.phase = FLYING;
            g.phase_t = 0;
            sfx_start();
        }
        break;

    case OVER:
        if (g.phase_t > 0.8f && ges->tap) new_run();
        break;
    }
}

static void sf_draw(void)
{
    if (!g.cv) return;

    if (T->menu_is_open()) {
        char best[24];
        snprintf(best, sizeof(best), "%d  L%d", g.best, g.best_level);
        const tat_menu_row_t rows[] = {
            T->menu_sound_row(),
            {"UP / DOWN", g.invert ? "INVERTED" : "NORMAL", 0},
            {"NEW RUN", "GO", T->ui_color(TAT_UI_ACCENT)},
            {"BEST", best, T->ui_color(TAT_UI_LABEL)},
        };
        T->menu_draw(rows, 4, "PAUSED");
        return;
    }
    if (!g.dirty) return;
    g.dirty = false;

    draw_trench();

    // Far to near, with the raider taking its place among the gates: one that is beyond it
    // is drawn behind it, and one that has passed it is in front.
    const bool raider_up = g.boom_t <= 0 && (g.phase == FLYING || g.phase == READY);
    float ex, ey;
    raider_screen(&ex, &ey);
    bool raider_drawn = !raider_up;
    for (int pass = 0; pass < MAX_GATES; pass++) {
        int far = -1;
        for (int i = 0; i < MAX_GATES; i++)   /* the farthest not yet drawn: there are only four */
            if (g.gates[i].alive && g.gates[i].z > 0 && (far < 0 || g.gates[i].z > g.gates[far].z)) far = i;
        if (far < 0) break;
        if (!raider_drawn && g.gates[far].z < RAIDER_Z) {
            draw_raider(ex, ey);
            raider_drawn = true;
        }
        draw_gate(&g.gates[far]);
        g.gates[far].z = -g.gates[far].z;   /* marked as drawn, and put right below */
    }
    for (int i = 0; i < MAX_GATES; i++)
        if (g.gates[i].z < 0) g.gates[i].z = -g.gates[i].z;
    if (!raider_drawn) draw_raider(ex, ey);

    // The shot: two bolts from the wingtips that meet at the sight.
    if (g.bolt_t > 0) {
        const float t = 1.0f - g.bolt_t / 0.16f;
        const int tx = g.bolt_hit ? (int)g.boom_x : CC, ty = g.bolt_hit ? (int)g.boom_y : CY;
        for (int side = -1; side <= 1; side += 2) {
            const int x0 = CC + side * 30, y0 = CW - 40;
            const float t1 = clampf(t + 0.35f, 0, 1);
            const int xa = x0 + (int)((tx - x0) * t), ya = y0 + (int)((ty - y0) * t);
            const int xb = x0 + (int)((tx - x0) * t1), yb = y0 + (int)((ty - y0) * t1);
            T->canvas_line(g.cv, xa, ya, xb, yb, g.c_bolt);
            T->canvas_line(g.cv, xa + 1, ya, xb + 1, yb, g.c_bolt_hi);
        }
    }
    if (g.boom_t > 0) {
        const float t = 1.0f - g.boom_t / 0.7f;
        const int r = 4 + (int)(t * 26.0f);
        if (t < 0.6f) T->canvas_fill_circle(g.cv, (int)g.boom_x, (int)g.boom_y, r, g.c_fire[t < 0.2f ? 0 : t < 0.4f ? 1 : 2]);
        if (t < 0.35f) T->canvas_fill_circle(g.cv, (int)g.boom_x, (int)g.boom_y, r / 2, g.c_bolt_hi);
    }
    for (int i = 0; i < MAX_PARTS; i++) {
        const Part *p = &g.parts[i];
        if (p->life > 0) rect((int)p->x, (int)p->y, p->life > 0.3f ? 2 : 1, p->life > 0.3f ? 2 : 1, p->col);
    }

    draw_ship();
    if (g.phase == FLYING) draw_sight();
    draw_hud();

    char top[28], mid[32], bottom[32];
    if (g.phase == READY) {
        banner("SIGHT IT AND TAP", "FLY THROUGH THE GATES", "TILT TO FLY - TAP TO START", g.c_accent);
    } else if (g.phase == CLEARED) {
        snprintf(top, sizeof(top), "LEVEL %d CLEAR", g.level);
        snprintf(mid, sizeof(mid), "NEXT: %d RAIDERS", level_quota(g.level + 1));
        banner(top, mid, g.shields < SHIELDS ? "+1 SHIELD" : NULL, g.c_go);
    } else if (g.phase == OVER) {
        snprintf(top, sizeof(top), "%s", g.ending == SHOT_DOWN ? "SHOT DOWN" : "OUT OF TIME");
        snprintf(mid, sizeof(mid), "LEVEL %d   %d POINTS", g.level, g.score);
        snprintf(bottom, sizeof(bottom), "BEST %d   TAP TO FLY AGAIN", g.best);
        banner(top, mid, bottom, g.c_danger);
    }
    T->canvas_present(g.cv);
}

static void sf_redraw(void) { g.dirty = true; }

static bool sf_keep_awake(void) { return (g.phase == FLYING || g.phase == CLEARED) && !T->menu_is_open(); }

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
