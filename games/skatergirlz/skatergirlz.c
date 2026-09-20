// SKATER GIRLZ - a rooftop skate run at sunset that ends the first time you get it wrong.
//
// She skates left to right across the roofs of a city. Turn the watch to the right, like a
// wheel, to push; turn it left to brake; tap to ollie and hold for more air. Speed is the
// whole game: the faster she goes the faster the metres come, and the wider the gaps
// between the roofs get. Miss a gap and she is gone. Clip the side of a building and she is gone. Hit a bin
// and she stumbles and loses a third of her speed - which hurts, unless slowing down was
// what you wanted.
//
//   turn right  - push, the further the harder (turn left to brake)
//   tap         - ollie; keep your finger down for more air, and more at speed
//   tap again   - in the air: a trick. Land it clean and it is Aura; land mid-flip and it is not
//   land on a rail from above to grind it; tap to pop off
//   swipe left  - pause menu
//
// The mechanics are Canabalt's, and deliberately: a run that only ever speeds up, jumps
// whose height is how long you hold, obstacles that cost speed rather than lives, roofs
// laid out from how fast you are going so that every gap can be made, and one life. They
// are taken from reading the MIT-licensed source of the HaxeFlixel port
// (github.com/ninjamuffin99/canabalt-hf, (c) Finji); no code or art from it is here. What
// this adds is the tilt - in Canabalt you cannot choose your speed, and here you can - the
// rails, and the tricks.
//
// Tricks are the second thing to do with a jump. Letting go of the ollie early and tapping
// again flips the board - a kickflip, a kickflick, a kickflip 360 or a dolphin flip, whichever
// comes; it takes a moment, and she has to be back on it before the roof arrives. Each trick in the same air is worth more than the last, a trick landed onto a
// rail is worth double, and none of it counts until she lands clean - so the height a gap
// does not need is the height there is to spend, and the score is metres plus Aura.
//
// An earlier version had three lives, put her back on the roof when she fell, let her
// scramble up walls, and made hitting a bin a silent multiplication. Every one of those
// took the consequence out of a mistake, and a runner with no consequences is a screensaver.
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

// She rides a quarter of the way across: at speed, what is coming matters more than what
// has been. The camera follows the roofline so that the roof she is on sits at ROOF_LINE.
#define SKATER_X 58
#define ROOF_LINE 156

// Speeds are canvas pixels a second; the screen is 233 wide.
#define V_START 70.0f
#define V_MIN 52.0f
#define V_MAX 270.0f
#define BRAKE 130.0f          /* what turning hard left takes off, per second */

#define GRAVITY 620.0f
#define JUMP_V 158.0f
#define FALL_MAX 170.0f
#define JUMP_KICK_S 0.08f     /* the first instant of a jump is softer, so a tap is a hop */
#define COYOTE_S 0.07f        /* how long after rolling off an edge a jump still counts */
#define LIP 5.0f              /* feet this far below a roof's edge still catch it */
#define HARD_LANDING_S 0.45f  /* this long at terminal velocity and the landing is a stumble:
                                 a real drop, not an ordinary ollie coming back down */

#define MAX_ROOFS 8
#define MAX_BINS 3
#define MAX_PARTS 40
#define MAX_FLYERS 4

typedef struct {
    float x0, x1, y;          /* world span, and the height of the roof (y grows downward) */
    uint32_t look;            /* what decides its windows, colour and clutter */
    int n_bins;
    float bin_x[MAX_BINS];
    bool bin_up[MAX_BINS];    /* still standing */
    bool rail;
    float rail_x0, rail_x1, rail_y;
} Roof;

typedef struct { float x, y, vx, vy, life; uint8_t col; } Part;
typedef struct { float x, y, vx, vy, spin; bool alive; } Flyer;   /* a bin that has been hit */

typedef enum { READY, SKATING, BAILING, OVER } Phase;
typedef enum { DIED_GAP, DIED_WALL } Death;

static struct {
    tat_canvas_t *cv;

    uint8_t c_sky[6], c_sun, c_far, c_far_lit, c_near, c_near_lit;
    uint8_t c_wall[2], c_wall_edge, c_roof, c_lip, c_glass, c_lit, c_clutter;
    uint8_t c_rail, c_rail_leg, c_bin, c_bin_lid;
    uint8_t c_skin, c_hair, c_shirt, c_jeans, c_board, c_wheel, c_shoe;
    uint8_t c_text, c_dim, c_accent, c_danger, c_go, c_panel, c_dust, c_spark, c_streak;
    uint8_t c_aura, c_aura_hi, c_grip;

    Phase phase;
    float phase_t;
    Death death;

    float x, y, vy, v;        /* y is her feet */
    float cam_y;
    bool grounded, grinding;
    float jump_t;             /* >= 0 while a held jump is still rising, else -1 */
    float coyote;
    float fall_t;             /* time spent at terminal velocity */
    float stumble_t;
    float push_t;             /* where she is in the pushing stride */
    float shake;
    float anim_t;
    float grind_tick;
    int grind_bonus;          /* metres of credit from rails */
    float trick_t;            /* > 0 while the board is mid-flip */
    int trick_kind;           /* which of TRICKS it is */
    int tricks;               /* how many this air */
    int pending;              /* Aura earned this air, not hers until she lands it */
    int aura;                 /* banked */
    float glow_t;             /* just banked some: she shines for a moment */
    char pop[20];             /* a word or two that floats up beside her */
    float pop_t;
    uint8_t pop_col;
    int best;
    int sens;                 /* tilt sensitivity, 0..2 */

    float grav_x, grav_y, grav_z;
    float roll, push;         /* push: -1 full brake .. +1 full push */

    Roof roofs[MAX_ROOFS];
    int n_roofs;
    Part parts[MAX_PARTS];
    Flyer flyers[MAX_FLYERS];

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

static uint32_t hash(uint32_t a, uint32_t b)
{
    uint32_t h = a * 2654435761u ^ (b + 0x9E3779B9u + (a << 6) + (a >> 2));
    h ^= h >> 15;
    h *= 2246822519u;
    return h ^ (h >> 13);
}

static int sx_of(float wx) { return SKATER_X + (int)floorf(wx - g.x); }
static int sy_of(float wy)
{
    const int jolt = g.shake > 0 ? (int)(sinf(g.anim_t * 90.0f) * g.shake * 2.5f) : 0;
    return (int)floorf(wy - g.cam_y) + jolt;
}

static int metres(void) { return (int)(g.x / 10.0f) + g.grind_bonus; }
static int score(void) { return metres() + g.aura; }

static void pop(const char *text, uint8_t colour)
{
    snprintf(g.pop, sizeof(g.pop), "%s", text);
    g.pop_t = 0.9f;
    g.pop_col = colour;
}

// ---------------------------------------------------------------- sound

static void tone1(float f0, float f1, int ms, int wave, float vol, int delay)
{
    const tat_tone_t t = {f0, f1, (uint16_t)ms, (uint8_t)wave, vol, (uint16_t)delay};
    T->tone(&t);
}
static void sfx_push(void) { tone1(95, 70, 45, TAT_NOISE, 0.22f, 0); }
static void sfx_ollie(void) { tone1(240, 620, 70, TAT_TRIANGLE, 0.45f, 0); }
static void sfx_land(void) { tone1(150, 90, 55, TAT_NOISE, 0.4f, 0); }
static void sfx_trick(int n) { tone1(520.0f + 130.0f * n, 900.0f + 160.0f * n, 90, TAT_SQUARE, 0.35f, 0); }
static void sfx_aura(void)
{
    tone1(784, 0, 60, TAT_TRIANGLE, 0.5f, 0);
    tone1(1047, 0, 60, TAT_TRIANGLE, 0.5f, 60);
    tone1(1568, 0, 130, TAT_TRIANGLE, 0.5f, 120);
}
static void sfx_sketchy(void) { tone1(300, 120, 140, TAT_SQUARE, 0.45f, 0); }
static void sfx_grind(void) { tone1(900, 700, 60, TAT_NOISE, 0.25f, 0); }
static void sfx_start(void) { tone1(440, 880, 110, TAT_TRIANGLE, 0.6f, 0); }
static void sfx_bin(void)
{
    tone1(180, 60, 160, TAT_NOISE, 0.7f, 0);
    tone1(120, 70, 120, TAT_SQUARE, 0.5f, 0);
}
static void sfx_wall(void)
{
    tone1(90, 40, 260, TAT_NOISE, 0.8f, 0);
    tone1(70, 45, 300, TAT_SQUARE, 0.6f, 0);
}
static void sfx_fall(void) { tone1(760, 90, 520, TAT_TRIANGLE, 0.6f, 0); }
static void sfx_over(void)
{
    tone1(392, 0, 150, TAT_SQUARE, 0.6f, 0);
    tone1(330, 0, 150, TAT_SQUARE, 0.6f, 150);
    tone1(247, 0, 320, TAT_SQUARE, 0.6f, 300);
}

// ---------------------------------------------------------------- bits that fly about

static void puff(float x, float y, float vx, float vy, float life, uint8_t col)
{
    for (int i = 0; i < MAX_PARTS; i++)
        if (g.parts[i].life <= 0) {
            g.parts[i] = (Part){x, y, vx, vy, life, col};
            return;
        }
}

static void dust(int n)
{
    for (int i = 0; i < n; i++)
        puff(g.x + frand(-6, 4), g.y - 1, frand(-50, 10) - g.v * 0.2f, frand(-40, -5), frand(0.2f, 0.45f), g.c_dust);
}

// ---------------------------------------------------------------- the jump

// How long a held jump keeps rising. It grows with speed, which is the rule that makes the
// game hang together: the gaps grow with speed too, and this is what lets her clear them.
static float jump_limit(float v) { return clampf(0.11f + (v / V_MAX) * 0.27f, 0.11f, 0.36f); }

// How high a full jump gets at this speed: the held climb, then what is left of the throw.
static float jump_rise(float v) { return JUMP_V * jump_limit(v) + JUMP_V * JUMP_V / (2 * GRAVITY); }

// ---------------------------------------------------------------- the roofs

// The next roof is laid out from how fast she is going right now, so that whatever speed
// the player has chosen, the gap in front of them can be made: between 40% and 100% of
// about half a second's travel, against a jump that at any speed lasts longer than that.
// The wider the gap, the less the far roof is allowed to be above this one.
static void add_roof(void)
{
    if (g.n_roofs >= MAX_ROOFS) {
        memmove(&g.roofs[0], &g.roofs[1], sizeof(Roof) * (MAX_ROOFS - 1));
        g.n_roofs--;
    }
    const Roof *last = &g.roofs[g.n_roofs - 1];
    Roof *r = &g.roofs[g.n_roofs++];
    memset(r, 0, sizeof(*r));

    const float reach = g.v * 0.5625f;
    const float how_wide = frand(0, 1);
    float gap = reach * (0.4f + 0.6f * how_wide);
    if (gap < 20) gap = 20;

    const float up = jump_rise(g.v) * 0.5f * (1.0f - how_wide);
    float dy = frand(-up, 38.0f);
    if (fabsf(dy) < 5) dy = dy < 0 ? -6 : 8;   /* a step, not a crack */

    float min_w = CW - gap;
    if (min_w < 110) min_w = 110;
    r->x0 = last->x1 + gap;
    r->x1 = r->x0 + frand(min_w, min_w * 2.6f);
    r->y = last->y + dy;
    r->look = hash(g.seed, (uint32_t)r->x0);

    const float w = r->x1 - r->x0;
    // Bins stand clear of where she lands and of where she has to take off.
    const int want = w < 170 ? 0 : (int)frand(0, 1.0f + w / 150.0f);
    for (int i = 0; i < want && r->n_bins < MAX_BINS; i++) {
        const float bx = r->x0 + frand(70.0f, w - 60.0f);
        bool clear = true;
        for (int k = 0; k < r->n_bins; k++) clear = clear && fabsf(r->bin_x[k] - bx) > 46.0f;
        if (!clear) continue;
        r->bin_x[r->n_bins] = bx;
        r->bin_up[r->n_bins++] = true;
    }
    if (w > 200 && frand(0, 1) < 0.55f) {
        r->rail = true;
        r->rail_x0 = r->x0 + frand(60.0f, w - 150.0f);
        r->rail_x1 = r->rail_x0 + frand(60.0f, 110.0f);
        r->rail_y = r->y - frand(15.0f, 22.0f);
    }
}

static void extend(void)
{
    while (g.roofs[g.n_roofs - 1].x1 < g.x + CW + 80.0f) add_roof();
}

static Roof *roof_at(float wx)
{
    for (int i = 0; i < g.n_roofs; i++)
        if (wx >= g.roofs[i].x0 && wx <= g.roofs[i].x1) return &g.roofs[i];
    return NULL;
}

static void start_run(void)
{
    const int best = g.best, sens = g.sens;
    tat_canvas_t *cv = g.cv;
    // Everything about a run goes, the palette and settings stay.
    g.x = 0;
    g.y = ROOF_LINE;
    g.vy = 0;
    g.v = V_START;
    g.cam_y = 0;
    g.grounded = true;
    g.grinding = false;
    g.jump_t = -1;
    g.coyote = g.fall_t = g.stumble_t = g.push_t = g.shake = g.grind_tick = 0;
    g.grind_bonus = 0;
    g.trick_t = g.glow_t = g.pop_t = 0;
    g.tricks = g.pending = g.aura = 0;
    g.push = 0;
    memset(g.parts, 0, sizeof(g.parts));
    memset(g.flyers, 0, sizeof(g.flyers));
    g.n_roofs = 0;
    Roof *r = &g.roofs[g.n_roofs++];
    memset(r, 0, sizeof(*r));
    r->x0 = -160.0f;
    r->x1 = 380.0f;   /* a long clear run-up: the first thing she meets is not a gap */
    r->y = ROOF_LINE;
    r->look = hash(g.seed, 1);
    extend();
    g.cv = cv;
    g.best = best;
    g.sens = sens;
    g.phase = READY;
    g.phase_t = 0;
    g.dirty = true;
}

static void die(Death how)
{
    g.death = how;
    g.phase = BAILING;
    g.phase_t = 0;
    g.grounded = g.grinding = false;
    g.jump_t = -1;
    g.pending = 0;
    g.trick_t = 0;
    if (how == DIED_WALL) {
        g.v = 0;
        g.vy = -60;
        g.shake = 1.0f;
        sfx_wall();
        for (int i = 0; i < 10; i++)
            puff(g.x + 5, g.y - frand(2, 18), frand(-90, -10), frand(-80, 20), frand(0.3f, 0.6f), g.c_dust);
    } else {
        sfx_fall();
    }
}

static void stumble(float keep)
{
    g.v *= keep;
    if (g.v < V_MIN) g.v = V_MIN;
    g.stumble_t = 0.6f;
    g.shake = 0.6f;
}

// ---------------------------------------------------------------- tricks

// Which trick she does is the luck of the tap: she is showing off, not taking requests. They
// are not equal. The flashier the trick, the longer the board is away from her feet - so
// the more it is worth, and the more air it needs to be landed at all. The second trick in
// the same air is worth twice its Aura and the third three times.
typedef struct {
    const char *name;
    float seconds;   /* the board is off her feet for this long */
    int aura;
    int odds;        /* out of the total, how often it comes up */
} Trick;
enum { KICKFLIP, KICKFLICK, KICKFLIP_360, DOLPHIN_FLIP, N_TRICKS };
static const Trick TRICKS[N_TRICKS] = {
    {"KICKFLIP", 0.32f, 10, 40},       /* rolls once about its length */
    {"KICKFLICK", 0.28f, 15, 25},      /* the same, flicked: twice round in less time */
    {"KICKFLIP 360", 0.40f, 20, 20},   /* rolls and spins flat at once */
    {"DOLPHIN FLIP", 0.46f, 25, 15},   /* nose down and right over, end over end */
};

static int pick_trick(void)
{
    int roll = (int)frand(0, 100.0f);
    for (int i = 0; i < N_TRICKS; i++) {
        roll -= TRICKS[i].odds;
        if (roll < 0) return i;
    }
    return KICKFLIP;
}

// Touching down, on a roof or a rail. If the board is still turning she has not landed it:
// the Aura from this air is gone and she stumbles. Otherwise it is hers, doubled if she put
// it down on a rail, which is the hardest place to put it.
static void touch_down(bool on_rail)
{
    if (g.trick_t > 0) {
        g.trick_t = 0;
        g.pending = 0;
        g.tricks = 0;
        pop("SKETCHY!", g.c_danger);
        sfx_sketchy();
        T->log("sketchy landing at %d m", metres());
        stumble(0.8f);
        return;
    }
    if (g.pending > 0) {
        const int got = g.pending * (on_rail ? 2 : 1);
        char line[20];
        snprintf(line, sizeof(line), "+%d AURA", got);
        pop(line, g.c_aura_hi);
        g.aura += got;
        g.glow_t = 0.7f;
        sfx_aura();
        T->log("landed %d trick%s at %d m: +%d aura%s", g.tricks, g.tricks == 1 ? "" : "s", metres(), got,
               on_rail ? " (rail)" : "");
        for (int i = 0; i < 10; i++)
            puff(g.x + frand(-8, 8), g.y - frand(2, 22), frand(-30, 30) + g.v * 0.5f, frand(-90, -20), frand(0.3f, 0.6f),
                 i & 1 ? g.c_aura : g.c_aura_hi);
    }
    g.pending = 0;
    g.tricks = 0;
}

// ---------------------------------------------------------------- skating

// The control is the one GRAND PRIX steers with: the watch turned like a wheel. She is
// heading right, so turning it right is pushing on and turning it left is digging in. It was
// tipping the watch forward at first, which is a throttle for a car seen from behind, not
// for someone crossing the screen sideways - the turn points the way she is going.
static void read_tilt(const tat_input_t *in, float dt)
{
    // Gravity gives the angle, so there is nothing to drift. Upright is coasting.
    const float k = 1.0f - expf(-dt / 0.10f);
    g.grav_x += (in->tilt.ax - g.grav_x) * k;
    g.grav_y += (in->tilt.ay - g.grav_y) * k;
    g.grav_z += (in->tilt.az - g.grav_z) * k;
    // Lying flat there is no "down" to read, so hold the last angle.
    if (g.grav_x * g.grav_x + g.grav_y * g.grav_y > 0.09f) g.roll = atan2f(g.grav_x, g.grav_y);
    static const float RANGE[3] = {0.80f, 0.55f, 0.36f};   /* radians of turn to flat out */
    float s = g.roll / RANGE[g.sens];
    const float dead = 0.10f;
    s = fabsf(s) < dead ? 0 : (s - copysignf(dead, s)) / (1 - dead);
    g.push = clampf(s, -1.0f, 1.0f);
}

// Testing a runner over Wi-Fi, where a tap arrives a second late, needs something that can
// jump on time. Built only when SK_AUTOPILOT is defined, which no shipped build does: it
// jumps at the end of each roof and holds for as long as the gap needs, and ignores bins
// and rails, so those get hit and landed on by chance.
#ifdef SK_AUTOPILOT
static Roof *roof_at(float wx);
static void autopilot(bool *pressed, bool *down)
{
    static bool holding;
    const Roof *r = roof_at(g.x);
    if (g.grounded && r && r->x1 - g.x < g.v * 0.06f + 4.0f) holding = true;
    if (holding && g.jump_t < 0 && !g.grounded && !g.grinding) holding = false;
    *pressed = holding && (g.grounded || g.grinding);
    *down = holding;
    // Once it has let go, a trick - and sometimes a second, which it will not always land.
    if (!holding && !g.grounded && !g.grinding && g.trick_t <= 0 && g.tricks < 2 && g.vy > -60.0f && g.vy < 90.0f)
        *pressed = true;
}
#endif

static void step(const tat_input_t *in, float dt)
{
    read_tilt(in, dt);
    bool pressed = in->touch.pressed, down = in->touch.down;
#ifdef SK_AUTOPILOT
    autopilot(&pressed, &down);
#endif

    // Speed. Left alone she gathers it slowly, and the faster she is going the slower it
    // comes. Turning right is pushing, and is worth nearly four times that; turning left is
    // dragging a foot. The dead zone around upright is coasting.
    float gather = g.v < 100 ? 22.0f : g.v < 160 ? 14.0f : g.v < 220 ? 9.0f : 5.0f;
    const bool rolling = g.grounded && !g.grinding;
    if (g.push > 0.02f) {
        if (rolling) g.v += gather * (0.6f + 3.2f * g.push) * dt;
    } else if (g.push < -0.02f) {
        if (rolling) g.v += BRAKE * g.push * dt;
    } else if (rolling) {
        g.v += gather * 0.6f * dt;
    }
    g.v = clampf(g.v, V_MIN, V_MAX);

    // The pushing stride, for the picture and the sound: quicker the harder she pushes.
    if (rolling && g.push > 0.02f && g.stumble_t <= 0) {
        const float before = g.push_t;
        g.push_t += dt * (1.3f + 1.6f * g.push);
        if ((int)before != (int)g.push_t) sfx_push();
    } else {
        g.push_t = floorf(g.push_t) + 0.0f;
    }

    // The ollie: from the roof, from a rail, or a moment after rolling off an edge.
    if (g.coyote > 0) g.coyote -= dt;
    if (pressed && (g.grounded || g.grinding || g.coyote > 0)) {
        g.jump_t = 0;
        g.grounded = g.grinding = false;
        g.coyote = 0;
        sfx_ollie();
        dust(3);
    } else if (pressed && g.trick_t <= 0) {
        // A second tap in the air. One at a time: she has to be back on the board before
        // the next, which is what stops it being a matter of tapping as fast as you can.
        g.trick_kind = pick_trick();
        g.trick_t = TRICKS[g.trick_kind].seconds;
        g.jump_t = -1;
        g.pending += TRICKS[g.trick_kind].aura * (g.tricks + 1);
        pop(TRICKS[g.trick_kind].name, g.c_aura);
        sfx_trick(g.trick_kind);
        T->log("%s at %d m", TRICKS[g.trick_kind].name, metres());
        g.tricks++;
    }
    if (g.trick_t > 0) g.trick_t -= dt;
    if (g.jump_t >= 0) {
        g.jump_t += dt;
        if (!down || g.jump_t > jump_limit(g.v)) g.jump_t = -1;
        else g.vy = g.jump_t < JUMP_KICK_S ? -JUMP_V * 0.65f : -JUMP_V;
    }

    const float prev_y = g.y, prev_front = g.x + 5.0f;
    g.x += g.v * dt;

    if (g.grinding) {
        Roof *r = roof_at(g.x);
        if (!r || !r->rail || g.x > r->rail_x1) {
            g.grinding = false;   /* off the end, with a little pop */
            g.vy = -45.0f;
        } else {
            g.y = r->rail_y;
            g.grind_tick += dt;
            if (g.grind_tick > 0.09f) {
                g.grind_tick = 0;
                sfx_grind();
                puff(g.x - 6, g.y, frand(-120, -40), frand(-70, -10), 0.25f, g.c_spark);
                puff(g.x + 4, g.y, frand(-100, -20), frand(-50, 10), 0.2f, g.c_spark);
            }
        }
    } else if (!g.grounded) {
        g.vy += GRAVITY * dt;
        if (g.vy >= FALL_MAX) {
            g.vy = FALL_MAX;
            g.fall_t += dt;
        }
        g.y += g.vy * dt;
    }

    // The side of a building. If her feet are only just below its edge she catches the lip
    // and rides on - being robbed by three pixels is not difficulty - but lower than that
    // and it is a wall, and walls do not move.
    const float front = g.x + 5.0f;
    for (int i = 0; i < g.n_roofs; i++) {
        const Roof *r = &g.roofs[i];
        if (prev_front < r->x0 && front >= r->x0) {
            if (g.y > r->y + LIP) {
                g.x = r->x0 - 5.0f;
                // Clipping the edge is hitting a wall. Meeting it a long way down is having
                // missed the gap, whatever she ran into afterwards.
                die(g.y > r->y + 34.0f ? DIED_GAP : DIED_WALL);
                g.v = 0;
                return;
            }
            if (g.y > r->y) g.y = r->y;
        }
    }

    Roof *under = roof_at(g.x);
    if (g.grounded) {
        if (!under) {
            g.grounded = false;   /* rolled off the edge */
            g.vy = 0;
            g.coyote = COYOTE_S;
            g.fall_t = 0;
        } else {
            g.y = under->y;
        }
    } else if (!g.grinding && g.vy >= 0) {
        // Coming down: a rail first, since it is above the roof it stands on.
        if (under && under->rail && g.x >= under->rail_x0 && g.x <= under->rail_x1 && prev_y <= under->rail_y + 2
            && g.y >= under->rail_y) {
            g.y = under->rail_y;
            g.vy = 0;
            g.grinding = true;
            g.fall_t = 0;
            g.grind_tick = 1;
            g.grind_bonus += 5;
            T->log("grind at %d m, %.0f px/s", metres(), (double)g.v);
            touch_down(true);
            g.v = clampf(g.v + 12.0f, V_MIN, V_MAX);   /* a rail gives a little back */
        } else if (under && prev_y <= under->y + 2 && g.y >= under->y) {
            g.y = under->y;
            g.vy = 0;
            g.grounded = true;
            sfx_land();
            touch_down(false);
            if (g.fall_t > HARD_LANDING_S) {
                T->log("hard landing at %d m", metres());
                stumble(0.93f);   /* came down hard */
                dust(9);
            } else {
                dust(4);
                if (g.shake < 0.25f) g.shake = 0.25f;
            }
            g.fall_t = 0;
        }
    }

    // Bins. Hitting one is a stumble and a third of her speed, and the bin goes flying -
    // which is a disaster at the wrong moment and the only brake there is at the right one.
    if (under && !g.grinding && g.y > under->y - 9.0f) {
        for (int k = 0; k < under->n_bins; k++) {
            if (!under->bin_up[k] || fabsf(g.x + 3.0f - under->bin_x[k]) > 6.0f) continue;
            under->bin_up[k] = false;
            for (int f = 0; f < MAX_FLYERS; f++)
                if (!g.flyers[f].alive) {
                    g.flyers[f] = (Flyer){under->bin_x[k], under->y - 6, g.v * 0.9f + frand(-20, 40), frand(-150, -95),
                                          frand(7, 13), true};
                    break;
                }
            for (int i = 0; i < 8; i++)
                puff(under->bin_x[k], under->y - frand(2, 10), frand(-30, 90), frand(-110, -20), frand(0.3f, 0.6f),
                     i & 1 ? g.c_bin : g.c_bin_lid);
            T->log("bin at %d m, %.0f -> %.0f px/s", metres(), (double)g.v, (double)(g.v * 0.7f));
            stumble(0.7f);
            pop("OOF!", g.c_danger);
            sfx_bin();
        }
    }

    // Gone below the bottom of the screen: that was the gap.
    if (sy_of(g.y) > CW + 30) {
        die(DIED_GAP);
        return;
    }
    extend();
}

static void step_world(float dt)
{
    g.anim_t += dt;
    if (g.shake > 0) g.shake -= dt * 2.2f;
    if (g.stumble_t > 0) g.stumble_t -= dt;
    if (g.glow_t > 0) g.glow_t -= dt;
    if (g.pop_t > 0) g.pop_t -= dt;

    for (int i = 0; i < MAX_PARTS; i++) {
        Part *p = &g.parts[i];
        if (p->life <= 0) continue;
        p->life -= dt;
        p->vy += 300.0f * dt;
        p->x += p->vx * dt;
        p->y += p->vy * dt;
    }
    for (int i = 0; i < MAX_FLYERS; i++) {
        Flyer *f = &g.flyers[i];
        if (!f->alive) continue;
        f->vy += 330.0f * dt;
        f->x += f->vx * dt;
        f->y += f->vy * dt;
        if (sy_of(f->y) > CW + 20) f->alive = false;
    }

    // The camera follows the roofline rather than her: it looks at the roof she is on, or
    // the one she is heading for, and it takes its time, so a jump is a jump on screen.
    const Roof *look = roof_at(g.x);
    if (!look)
        for (int i = 0; i < g.n_roofs && !look; i++)
            if (g.roofs[i].x0 > g.x) look = &g.roofs[i];
    if (look && g.phase != BAILING) {
        const float want = look->y - ROOF_LINE;
        g.cam_y += (want - g.cam_y) * (1.0f - expf(-dt * 3.0f));
    }
}

// ---------------------------------------------------------------- drawing

static void rect(int x, int y, int w, int h, uint8_t c) { T->canvas_fill_rect(g.cv, x, y, w, h, c); }

static void draw_sky(void)
{
    const int band = ROOF_LINE / 6 + 8;
    for (int i = 0; i < 6; i++) rect(0, i * band, CW, band + 1, g.c_sky[i]);
    rect(0, 6 * band, CW, CW - 6 * band, g.c_sky[5]);
    T->canvas_fill_circle(g.cv, 164, 118, 26, g.c_sun);

    // Two skylines, the far one drifting slower. With the roofs in front plain-walled, this
    // and the streaks are what make the speed something you can see.
    for (int layer = 0; layer < 2; layer++) {
        const float par = layer == 0 ? 0.10f : 0.26f;
        const uint8_t col = layer == 0 ? g.c_far : g.c_near, lit = layer == 0 ? g.c_far_lit : g.c_near_lit;
        const int span = layer == 0 ? 46 : 34;
        const float off = g.x * par;
        const int first = (int)floorf(off / span);
        for (int i = -1; i < CW / span + 2; i++) {
            const int idx = first + i;
            const int sx = (int)(idx * span - off);
            const uint32_t h = hash((uint32_t)idx, (uint32_t)layer + 11);
            const int tall = 40 + (int)(h % 58) + layer * 10;
            const int top = ROOF_LINE + 30 - tall - (int)(g.cam_y * (layer == 0 ? 0.08f : 0.2f));
            rect(sx, top, span - 4, CW - top, col);
            for (int wy = top + 5; wy < top + tall - 4; wy += 9)
                for (int wx = sx + 3; wx < sx + span - 8; wx += 7)
                    if (hash(h, (uint32_t)(wx - sx) * 31 + (uint32_t)(wy - top)) % 7 == 0) rect(wx, wy, 3, 4, lit);
        }
    }

    // Streaks, once she is properly moving: more of them and longer the faster she goes.
    if (g.v > 150 && g.phase == SKATING) {
        const int n = (int)((g.v - 150) / 14);
        for (int i = 0; i < n; i++) {
            const uint32_t h = hash((uint32_t)i, 77);
            const float speed = 1.6f + (h % 100) / 60.0f;
            const int len = 10 + (int)((g.v - 150) / 6) + (int)(h % 9);
            const int x = CW + 40 - (int)fmodf(g.x * speed + (float)(h % 997), (float)(CW + 80));
            const int y = 30 + (int)(h / 7 % 110);
            rect(x, y, len, 1, g.c_streak);
        }
    }
}

static void draw_roof(const Roof *r)
{
    const int x0 = sx_of(r->x0), x1 = sx_of(r->x1);
    if (x1 < -4 || x0 > CW + 4) return;
    const int y = sy_of(r->y);
    const uint8_t wall = g.c_wall[r->look & 1];

    // Clutter first, so it stands behind everything: tanks, vents and aerials that belong
    // to the roof and are not in her way. They are wall-coloured on purpose - a bin, which
    // is in her way, is the only green thing on a roof.
    const int w = x1 - x0;
    for (int k = 0; k < 4; k++) {
        const uint32_t h = hash(r->look, (uint32_t)k + 3);
        const int cx = x0 + 18 + (int)(h % (uint32_t)(w > 60 ? w - 40 : 20));
        switch (h >> 8 & 3) {
        case 0:   /* a water tank on legs */
            rect(cx, y - 17, 12, 10, g.c_clutter);
            rect(cx + 1, y - 7, 2, 7, g.c_clutter);
            rect(cx + 9, y - 7, 2, 7, g.c_clutter);
            break;
        case 1:   /* an aerial */
            rect(cx, y - 26, 1, 26, g.c_clutter);
            rect(cx - 3, y - 22, 7, 1, g.c_clutter);
            rect(cx - 2, y - 17, 5, 1, g.c_clutter);
            break;
        case 2:   /* an air conditioner */
            rect(cx, y - 8, 14, 8, g.c_clutter);
            rect(cx + 2, y - 6, 10, 1, wall);
            rect(cx + 2, y - 4, 10, 1, wall);
            break;
        default: break;   /* and sometimes nothing */
        }
    }

    rect(x0, y, w, CW - y + 4, wall);
    rect(x0, y, w, 2, g.c_lip);
    rect(x0, y + 2, w, 4, g.c_roof);
    rect(x0, y + 6, 3, CW - y, g.c_wall_edge);   /* the face she must not meet */

    // Windows, in a grid that belongs to the building and so stays put as it goes by.
    const int first = x0 + 7;
    for (int wy = y + 12, row = 0; wy < CW; wy += 13, row++)
        for (int wx = first, col = 0; wx < x1 - 7; wx += 10, col++) {
            if (wx < -6 || wx > CW) continue;
            const bool on = hash(r->look, (uint32_t)(col * 57 + row)) % 5 == 0;
            rect(wx, wy, 5, 7, on ? g.c_lit : g.c_glass);
        }

    if (r->rail) {
        const int rx0 = sx_of(r->rail_x0), rx1 = sx_of(r->rail_x1), ry = sy_of(r->rail_y);
        rect(rx0, ry, rx1 - rx0, 2, g.c_rail);
        rect(rx0 + 2, ry + 2, 2, y - ry - 2, g.c_rail_leg);
        rect(rx1 - 4, ry + 2, 2, y - ry - 2, g.c_rail_leg);
        if ((rx1 - rx0) > 70) rect((rx0 + rx1) / 2 - 1, ry + 2, 2, y - ry - 2, g.c_rail_leg);
    }
    for (int k = 0; k < r->n_bins; k++) {
        if (!r->bin_up[k]) continue;
        const int bx = sx_of(r->bin_x[k]);
        rect(bx - 4, y - 11, 8, 11, g.c_bin);
        rect(bx - 5, y - 13, 10, 3, g.c_bin_lid);
        rect(bx - 2, y - 9, 1, 7, g.c_bin_lid);
        rect(bx + 1, y - 9, 1, 7, g.c_bin_lid);
    }
}

// She is drawn from a pose, not from frames: a lean, a crouch, a board angle and a stride,
// each of which comes straight from what she is doing. It keeps her moving between states
// instead of snapping, and it means her hair can stream further the faster she goes.
static void draw_skater(void)
{
    const int x = SKATER_X;
    const int y = sy_of(g.y);

    if (g.phase == BAILING || g.phase == OVER) {
        // Off the board: a tumbling bundle, and the board going its own way.
        const int turn = (int)(g.phase_t * 9.0f) & 3;
        const int bw = (turn & 1) ? 16 : 9, bh = (turn & 1) ? 9 : 16;
        rect(x - bw / 2, y - bh, bw, bh, turn < 2 ? g.c_shirt : g.c_jeans);
        rect(x - bw / 2 + ((turn == 1 || turn == 2) ? bw - 5 : 0), y - bh + (turn >= 2 ? bh - 5 : 0), 5, 5, g.c_skin);
        rect(x - bw / 2 + ((turn == 1 || turn == 2) ? bw - 7 : 0), y - bh - 2 + (turn >= 2 ? bh - 3 : 0), 7, 3, g.c_hair);
        const int bx = x + 10 + (int)(g.phase_t * 40.0f), by = y - 4 - (int)(sinf(g.phase_t * 7.0f) * 6.0f);
        T->canvas_line(g.cv, bx - 7, by + (turn & 1 ? 3 : -3), bx + 7, by + (turn & 1 ? -3 : 3), g.c_board);
        return;
    }

    const bool air = !g.grounded && !g.grinding;
    const float speed01 = clampf((g.v - V_MIN) / (V_MAX - V_MIN), 0, 1);

    // The board: nose up on the way up, level at the top, nose down coming in.
    int nose = 0;
    if (air) nose = g.vy < -40 ? -4 : g.vy > 60 ? 2 : -1;
    if (g.grinding) nose = -1;
    const int wobble = g.stumble_t > 0 ? (int)(sinf(g.anim_t * 38.0f) * 2.5f) : 0;
    const int by = y - 3;
    // Just banked some Aura: she shines, in rings that open out and fade.
    if (g.glow_t > 0) {
        const int r = 10 + (int)((0.7f - g.glow_t) * 26.0f);
        for (int a = 0; a < 16; a++) {
            const float t = a * (2 * PI / 16) + g.anim_t * 3.0f;
            T->canvas_pixel(g.cv, x + (int)(cosf(t) * r), y - 12 + (int)(sinf(t) * r), a & 1 ? g.c_aura : g.c_aura_hi);
        }
    }
    if (g.trick_t > 0) {
        // The board has left her feet and is turning under them, and each trick turns it its
        // own way, so they can be told apart without reading the word. Rolling about its
        // length shows deck, edge, grip, edge. Spinning flat shortens it to nothing and
        // back. End over end is the whole board wheeling round in the plane of the screen.
        const float turn = 1.0f - g.trick_t / TRICKS[g.trick_kind].seconds;   /* 0..1 through it */
        const int drop = 3 + (int)(sinf(turn * PI) * 5.0f);   /* it falls away and comes back */
        if (g.trick_kind == DOLPHIN_FLIP) {
            const float a = turn * 2 * PI;
            const int dx = (int)(cosf(a) * 8.0f), dy = (int)(sinf(a) * 8.0f);
            const uint8_t c = cosf(a) < 0 ? g.c_grip : g.c_board;
            T->canvas_line(g.cv, x - dx, by + drop - dy, x + dx, by + drop + dy, c);
            T->canvas_line(g.cv, x - dx, by + drop - dy + 1, x + dx, by + drop + dy + 1, c);
            rect(x + dx - 1, by + drop + dy - 1, 2, 2, g.c_wheel);
            rect(x - dx - 1, by + drop - dy - 1, 2, 2, g.c_wheel);
        } else {
            const int rolls = g.trick_kind == KICKFLICK ? 8 : 4;   /* quarter turns about its length */
            const int face = (int)(turn * rolls) & 3;
            int half = 8;
            if (g.trick_kind == KICKFLIP_360) half = (int)(fabsf(cosf(turn * 2 * PI)) * 8.0f) + 1;
            const uint8_t c = face == 2 ? g.c_grip : g.c_board;
            rect(x - half, by + drop, half * 2, (face & 1) ? 1 : 3, c);
            if (!(face & 1)) {
                rect(x - half + 1, by + drop + (face == 2 ? -2 : 3), 2, 2, g.c_wheel);
                rect(x + half - 3, by + drop + (face == 2 ? -2 : 3), 2, 2, g.c_wheel);
            }
        }
    } else {
        T->canvas_line(g.cv, x - 8, by - nose / 2 + 0, x + 8, by + nose, g.c_board);
        T->canvas_line(g.cv, x - 8, by - nose / 2 + 1, x + 8, by + nose + 1, g.c_board);
        rect(x - 6, by + 2 - nose / 3, 2, 2, g.c_wheel);
        rect(x + 4, by + 2 + nose / 2, 2, 2, g.c_wheel);
    }

    // Crouch: deep at the top of an ollie and on a rail, a little when braking.
    int crouch = air ? 4 : g.grinding ? 3 : g.push < -0.02f ? 2 : 0;
    if (g.trick_t > 0) crouch = 1;   /* knees up, out of the board's way */
    if (g.stumble_t > 0) crouch = 2;
    const int lean = (int)(speed01 * 3.0f) + (g.push > 0.02f ? 1 : 0) + wobble;
    const int hip = by - 6 + crouch;

    // Legs. When she pushes, the back leg swings down and behind to the roof and up again.
    const float stride = g.push_t - floorf(g.push_t);
    const bool pushing = g.grounded && g.push > 0.02f && g.stumble_t <= 0 && stride < 0.55f;
    rect(x + 1, hip, 3, by - hip, g.c_jeans);                       /* front leg, on the board */
    if (pushing) {
        const int reach = (int)(sinf(stride / 0.55f * PI) * 7.0f);
        T->canvas_line(g.cv, x - 1, hip + 1, x - 4 - reach, y - 1, g.c_jeans);
        T->canvas_line(g.cv, x - 2, hip + 1, x - 5 - reach, y - 1, g.c_jeans);
        rect(x - 6 - reach, y - 2, 3, 2, g.c_shoe);
    } else {
        rect(x - 4, hip, 3, by - hip, g.c_jeans);
    }
    rect(x + 1, by - 2, 4, 2, g.c_shoe);
    if (!pushing) rect(x - 5, by - 2, 4, 2, g.c_shoe);

    // Body, leaning into it.
    const int tx = x - 4 + lean;
    rect(tx, hip - 9, 8, 9, g.c_shirt);
    // Arms: out for balance in the air and when stumbling, swinging when pushing.
    if (air || g.stumble_t > 0) {
        rect(tx - 5, hip - 9 + (wobble > 0 ? 2 : 0), 5, 2, g.c_skin);
        rect(tx + 8, hip - 8 - (wobble > 0 ? 2 : 0), 5, 2, g.c_skin);
    } else {
        const int swing = pushing ? (int)(sinf(stride / 0.55f * PI) * 3.0f) : 0;
        rect(tx + 6 + swing, hip - 7, 2, 6, g.c_skin);
        rect(tx - 1 - swing, hip - 7, 2, 5, g.c_skin);
    }
    // Head, and the hair, which is the speedometer: it streams out behind her in a wave
    // that gets longer the faster she is going.
    const int hx = tx + 1 + (lean > 1 ? 1 : 0), hy = hip - 15;
    rect(hx, hy, 6, 6, g.c_skin);
    rect(hx, hy - 2, 7, 3, g.c_hair);
    const int len = 4 + (int)(speed01 * 13.0f) + (air ? 2 : 0);
    for (int i = 0; i < len; i++) {
        const int wave = (int)(sinf(g.anim_t * 14.0f + i * 0.7f) * (1.0f + i * 0.12f));
        rect(hx - 1 - i, hy - 1 + wave + i / 4 - (air && g.vy > 0 ? i / 3 : 0), 1, 4 - (i * 3) / len, g.c_hair);
    }
}

static void draw_hud(void)
{
    char buf[24];
    snprintf(buf, sizeof(buf), "%d", score());
    T->canvas_text_centered(g.cv, CW / 2, 17, buf, g.c_text, 2, true);

    // Speed, as a bar: green, then gold, then red when the gaps are about to get serious.
    const float s = clampf((g.v - V_MIN) / (V_MAX - V_MIN), 0, 1);
    const int bw = 56, bx = CW / 2 - bw / 2, by = 30;
    rect(bx - 1, by - 1, bw + 2, 5, g.c_panel);
    rect(bx, by, (int)(bw * s), 3, s > 0.72f ? g.c_danger : s > 0.42f ? g.c_accent : g.c_go);
    snprintf(buf, sizeof(buf), "%dM", metres());
    T->canvas_text(g.cv, CW / 2 - 4 - T->text_width(buf, 1, true), 39, buf, g.c_dim, 1, true);
    snprintf(buf, sizeof(buf), "%d AURA", g.aura);
    T->canvas_text(g.cv, CW / 2 + 4, 39, buf, g.aura ? g.c_aura_hi : g.c_dim, 1, true);

    // A word beside her, floating up and gone: what she just did, or what it cost.
    if (g.pop_t > 0) {
        // Never up into the score: at the top of a big ollie she is nearly there herself.
        const int rise = (int)((0.9f - g.pop_t) * 22.0f);
        int py = sy_of(g.y) - 40 - rise;
        if (py < 58) py = 58;
        T->canvas_text_centered(g.cv, SKATER_X + 34, py, g.pop, g.pop_col, 1, true);
    } else if (g.grinding) {
        int py = sy_of(g.y) - 40;
        if (py < 58) py = 58;
        T->canvas_text_centered(g.cv, SKATER_X + 34, py, "GRIND", g.c_accent, 1, true);
    }
}

static void banner(const char *top, const char *mid, const char *bottom, uint8_t col, int y)
{
    const tat_banner_t b = {
        .top = top, .mid = mid, .bottom = bottom,
        .top_color = col, .mid_color = g.c_text, .bottom_color = g.c_dim,
        .panel = g.c_panel, .border = col,
        .bars = true,
    };
    T->canvas_banner(g.cv, CW / 2, y, CW - 40, &b);
}

// ---------------------------------------------------------------- the game

static uint8_t col(uint8_t r, uint8_t gg, uint8_t b) { return T->canvas_color(g.cv, T->rgb(r, gg, b)); }

static void sk_begin(const tat_api_t *api)
{
    T = api;
    memset(&g, 0, sizeof(g));
    g.cv = T->canvas_create(SCALE, 0);
    if (!g.cv) {
        T->log("no memory for the street");
        return;
    }
    static const uint8_t SKY[6][3] = {{36, 28, 66}, {58, 38, 92}, {100, 54, 110}, {160, 78, 116}, {222, 120, 108}, {250, 170, 110}};
    for (int i = 0; i < 6; i++) g.c_sky[i] = col(SKY[i][0], SKY[i][1], SKY[i][2]);
    g.c_sun = col(255, 222, 150);
    g.c_far = col(92, 60, 108);
    g.c_far_lit = col(190, 130, 130);
    g.c_near = col(62, 44, 86);
    g.c_near_lit = col(236, 176, 110);
    g.c_wall[0] = col(40, 36, 58);
    g.c_wall[1] = col(48, 40, 62);
    g.c_wall_edge = col(24, 22, 38);
    g.c_roof = col(70, 66, 92);
    g.c_lip = col(168, 160, 190);
    g.c_glass = col(28, 28, 46);
    g.c_lit = col(255, 206, 110);
    g.c_clutter = col(34, 30, 50);
    g.c_rail = col(232, 236, 248);
    g.c_rail_leg = col(130, 134, 154);
    g.c_bin = col(70, 170, 96);
    g.c_bin_lid = col(150, 226, 160);
    g.c_skin = col(246, 200, 164);
    g.c_hair = col(255, 206, 70);
    g.c_shirt = col(244, 72, 148);
    g.c_jeans = col(66, 104, 190);
    g.c_board = col(90, 222, 216);
    g.c_wheel = col(250, 250, 255);
    g.c_shoe = col(250, 250, 255);
    g.c_text = col(248, 248, 255);
    g.c_dim = col(150, 146, 176);
    g.c_accent = col(255, 214, 61);
    g.c_danger = col(255, 84, 92);
    g.c_go = col(60, 220, 120);
    g.c_panel = col(14, 12, 22);
    g.c_dust = col(200, 190, 200);
    g.c_spark = col(255, 236, 140);
    g.c_streak = col(255, 214, 190);
    g.c_aura = col(176, 108, 255);
    g.c_aura_hi = col(226, 186, 255);
    g.c_grip = col(30, 28, 40);

    g.seed = (uint32_t)T->now_us() | 1u;
    g.sens = 1;
    T->save_get("best_m", &g.best, 0);   /* metres; the old "best" was in another unit */
    T->save_get("sens", &g.sens, 3);
    start_run();
    T->log("ready, best %d m", g.best);
}

static void sk_enter(void) { g.dirty = true; }

static void sk_update(float dt)
{
    if (!g.cv) return;
    if (dt > 1.0f / 30) dt = 1.0f / 30;

    const tat_input_t *in = T->input();
    const tat_gestures_t *ges = T->gestures();

    if (T->menu_is_open()) {
        switch (T->menu_update()) {
        case 0: T->menu_toggle_sound(); break;
        case 1:
            g.sens = (g.sens + 1) % 3;
            T->save_set("sens", g.sens);
            T->menu_invalidate();
            break;
        case 2:
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
        read_tilt(in, dt);
        if (ges->tap) {
            g.phase = SKATING;
            g.phase_t = 0;
            sfx_start();
        }
        break;

    case SKATING:
        step(in, dt);
        break;

    case BAILING:
        // The run is over; this is just watching it end. Off a wall she drops down its face.
        g.vy += GRAVITY * dt;
        if (g.vy > 260) g.vy = 260;
        g.y += g.vy * dt;
        g.x += g.v * dt;
        if (g.v > 0) g.v *= 1.0f - dt;
        if (g.phase_t > 1.3f || sy_of(g.y) > CW + 60) {
            g.phase = OVER;
            g.phase_t = 0;
            if (score() > g.best) {
                g.best = score();
                T->save_set("best_m", g.best);
            }
            T->log("%d m + %d aura = %d, %s, best %d", metres(), g.aura, score(), g.death == DIED_WALL ? "wall" : "gap",
                   g.best);
            sfx_over();
        }
        break;

    case OVER:
        if (g.phase_t > 0.7f && ges->tap) start_run();
        break;
    }
    step_world(dt);
}

static void sk_draw(void)
{
    if (!g.cv) return;

    if (T->menu_is_open()) {
        char best[16];
        static const char *const SENS[3] = {"GENTLE", "NORMAL", "TWITCHY"};
        snprintf(best, sizeof(best), "%d", g.best);
        const tat_menu_row_t rows[] = {
            T->menu_sound_row(),
            {"TILT", SENS[g.sens], 0},
            {"NEW RUN", "GO", T->ui_color(TAT_UI_ACCENT)},
            {"BEST", best, T->ui_color(TAT_UI_LABEL)},
        };
        T->menu_draw(rows, 4, "PAUSED");
        return;
    }
    if (!g.dirty) return;
    g.dirty = false;

    draw_sky();
    for (int i = 0; i < g.n_roofs; i++) draw_roof(&g.roofs[i]);
    for (int i = 0; i < MAX_FLYERS; i++) {
        const Flyer *f = &g.flyers[i];
        if (!f->alive) continue;
        const int fx = sx_of(f->x), fy = sy_of(f->y);
        const bool side = ((int)(g.anim_t * f->spin) & 1) != 0;   /* tumbling, in two frames */
        rect(fx - (side ? 6 : 4), fy - (side ? 4 : 6), side ? 12 : 8, side ? 8 : 12, g.c_bin);
        rect(fx - (side ? 6 : 5), fy - (side ? 4 : 7), side ? 3 : 10, side ? 8 : 3, g.c_bin_lid);
    }
    draw_skater();
    for (int i = 0; i < MAX_PARTS; i++) {
        const Part *p = &g.parts[i];
        if (p->life > 0) rect(sx_of(p->x), sy_of(p->y), p->life > 0.2f ? 2 : 1, p->life > 0.2f ? 2 : 1, p->col);
    }
    draw_hud();

    if (g.phase == READY) {
        banner("TURN RIGHT TO PUSH", "TAP: OLLIE  TAP AGAIN: TRICK", "TAP TO START", g.c_shirt, 62);
    } else if (g.phase == OVER) {
        char mid[32], bottom[32];
        char top[32];
        snprintf(top, sizeof(top), g.death == DIED_WALL ? "%d - HIT A WALL" : "%d - MISSED THE GAP", score());
        snprintf(mid, sizeof(mid), "%dM + %d AURA", metres(), g.aura);
        snprintf(bottom, sizeof(bottom), "BEST %d   TAP TO RETRY", g.best);
        banner(top, mid, bottom, g.c_danger, 62);
    }
    T->canvas_present(g.cv);
}

static void sk_redraw(void) { g.dirty = true; }

static bool sk_keep_awake(void) { return (g.phase == SKATING || g.phase == BAILING) && !T->menu_is_open(); }

static void sk_unload(void)
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
    .begin = sk_begin,
    .enter = sk_enter,
    .update = sk_update,
    .draw = sk_draw,
    .leave = NULL,
    .unload = sk_unload,
    .redraw = sk_redraw,
    .keep_awake = sk_keep_awake,
};
