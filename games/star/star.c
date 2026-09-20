// SLEEPY STAR - a laser comes straight down onto rings of walls with holes and mirrors cut
// into them. Turn the rings until the light reaches the red box on the star, and it wakes up.
//
//   drag a ring   - it turns with your finger, like a dial on a combination lock, and clicks
//                   into one of eight places. The light is redrawn as it turns, so the way
//                   through is found by looking
//   tap the star  - a hint: the ring that is in the wrong place
//   PWR           - pause menu (level, sound, start the level over)
//   swipe left    - pause menu too, from the star or the rim; a swipe that starts on a ring
//                   is a turn of that ring
//
// This is the third set of rules. The first two had the laser come from wherever real-world
// "up" was, so that turning the watch moved it round the rim, and had each ring geared to the
// one inside it, so that a tap turned two rings in opposite directions and the puzzle was in
// the gearing. It was clever, and played on a wrist it was work: two things moving for every
// one that was touched, and a laser that wandered off when the hand did. What was good in it
// was the light, the rings and the star, and those are what is left. The laser stays at the
// top. Every ring turns by itself, under the finger that is on it. The puzzle is the light's
// path: a hole lets it straight through, a mirror sends it one place round, a splitter does
// both - and from the first mirrors on, some ring has no plain hole at all, so there is no
// lining up a straight shot.
//
// Geometry is docs/SLEEPY_STAR_SPEC.md's, in device pixels; this draws on the 2x pixel
// canvas, so every spec number appears here halved. The board is drawn half a turn round
// from the spec (VIEW), which puts the star's target at the top, under the laser.
//
// Written against tat_api.h alone: it includes nothing else from the console and calls
// nothing by name.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tat/tat_api.h"

static const tat_api_t *T;

#define PI 3.14159265f
#define TAU (2 * PI)
#define SCALE 2
#define CW ((TAT_SCREEN + SCALE - 1) / SCALE)   /* 233 */
#define C (CW / 2.0f)

// ---------------------------------------------------------------- model

enum { BLOCK = 0, GAP, MIRROR_L, MIRROR_R, SPLIT };

#define SPOKES 8
#define MAX_RINGS 4
#define DOOR 4    /* the spoke the star's target is on... */
#define LASER 4   /* ...and the one the laser comes in on: the same, so the light has to come back to it */
#define VIEW 4    /* the board is drawn this many spokes round, which puts both at the top */

typedef struct {
    uint8_t n_rings;
    uint8_t ring[MAX_RINGS][SPOKES];   /* ring[0] = outermost */
    uint8_t start[MAX_RINGS];
    uint8_t best;                      /* the fewest rings that have to be moved */
} Level;

static int m8(int n) { return ((n % 8) + 8) % 8; }

// Ring layout by ring count (spec section 3, halved): radii outer -> inner, teeth, star radius.
typedef struct {
    float rad[MAX_RINGS];
    int teeth[MAX_RINGS];
    float star;
} Geo;

// Entries 0 and 1 are never looked at - no level has fewer than two rings - but the table is
// indexed by ring count, so they have to exist.
static const Geo GEO[MAX_RINGS + 1] = {
    {{0}, {0}, 0},
    {{0}, {0}, 0},
    {{56.5f, 35.5f}, {24, 16}, 16.0f},
    {{64.0f, 46.0f, 28.0f}, {32, 24, 16}, 14.0f},
    {{67.0f, 51.5f, 36.0f, 20.5f}, {32, 24, 16, 8}, 11.0f},
};

#define BAND_HALF 6.25f
#define HOLE_HALF 6.75f
#define TEETH_OUT 9.5f     /* how far a ring's teeth stand out past its band */

// Everything above is the spec's geometry, and it is drawn scaled up by zoom(): the outer
// ring's teeth come out to BOARD_PX on the canvas whatever the number of rings, which is
// most of the 116 there are. The emitter rides outside that, on the rim of the screen.
#define BOARD_PX 99.0f
#define EMIT_PX 101.0f     /* where the beam leaves the emitter */
#define DISC_PX 104.0f

#define MAX_STATES 4096    /* 8 places to the power of 4 rings */

typedef enum { TUTORIAL, PLAYING, SOLVED } Phase;

typedef struct {
    float x0, y0, x1, y1;
} Seg;

typedef struct {
    float x, y;
} Pt;

// One branch of the beam part way through its walk: which ring it is about to meet, the
// radius it set off from, and the spokes it left on and arrives on.
typedef struct {
    int ring;
    float from_r;
    int from_s, meet_s;
} Branch;

#define HINT_S 3.0f         /* how long a hint stays up */
#define STUCK_S 28.0f       /* no progress for this long and the star offers */
#define SNAP_S 0.12f        /* a ring let go of settles into its place in this long */

static const char *const CHEERS[] = {"NICE!", "YES!", "GOT IT!", "LOVELY!", "BRIGHT!", "WAHOO!"};

// Every arrangement of the current level's rings that the light gets through, packed. The
// hints are read from it, and so is the level's par.
static uint16_t s_wins[MAX_STATES];
static int s_n_wins;

static struct {
    tat_canvas_t *cv;
    const uint16_t *pol_a, *pol_r;   /* the screen's polar tables, fetched once */

    /* progress */
    int level, depth;
    bool seen_tut;

    /* play */
    Level lvl;
    uint8_t rot[MAX_RINGS];
    int moves;     /* rings turned to a new place: what is counted against the level's best */
    Phase phase;
    float phase_t;
    int reached;   /* how many rings the beam got through on the last trace */
    bool won;
    Seg segs[32];
    int n_segs;
    Pt stops[12];
    int n_stops;

    /* the ring under the finger */
    int grab;               /* or -1 */
    bool touch_on_ring;     /* this touch began on a ring, so it is not a swipe for the menu */
    float grab_vis, turned; /* where the ring was when it was taken, and how far it has been turned since, in places */
    float last_a;
    uint8_t grab_rot;

    /* where each ring is drawn: a ring let go of between places settles into the nearest */
    float vis_from[MAX_RINGS], vis_to[MAX_RINGS];
    float tween_t;

    /* ui */
    bool dirty;
    int stars_earned;

    /* hints */
    float hint_t;      /* > 0 while one is showing */
    int hint_ring;     /* the ring that is in the wrong place, or -1 */
    bool hinted;       /* this level has had one, which caps it at two stars */
    float idle_t;      /* since the player last got anywhere */
    float blink_t;

    /* palette */
    uint8_t c_void, c_disc, c_plate[4], c_tooth[4], c_lip, c_mirror, c_split;
    uint8_t c_beam, c_glow, c_white, c_star, c_star_lit, c_star_edge, c_halo, c_box, c_box_edge, c_box_lit;
    uint8_t c_face, c_cheek, c_text, c_dim, c_stop, c_emit, c_emit_hi, c_panel, c_rays;
    uint8_t c_orange, c_cyan, c_hint;
} g;

// ---------------------------------------------------------------- the beam

// What an element does to an inbound beam on spoke s: a bit mask of outbound spokes.
static uint8_t outs(uint8_t el, int s)
{
    switch (el) {
    case GAP: return (uint8_t)(1u << s);
    case MIRROR_R: return (uint8_t)(1u << m8(s + 1));
    case MIRROR_L: return (uint8_t)(1u << m8(s - 1));
    case SPLIT: return (uint8_t)((1u << s) | (1u << m8(s + 1)));
    default: return 0;
    }
}

// Does any branch of the light reach the star's target?
static bool wins(const Level *L, const uint8_t *rot)
{
    uint8_t mask = 1u << LASER;
    for (int i = 0; i < L->n_rings && mask; i++) {
        uint8_t next = 0;
        for (int s = 0; s < SPOKES; s++)
            if (mask & (1u << s)) next |= outs(L->ring[i][m8(s - rot[i])], s);
        mask = next;
    }
    return (mask & (1u << DOOR)) != 0;
}

// ---------------------------------------------------------------- levels

static int pack(const uint8_t *rot, int n)
{
    int v = 0;
    for (int i = n - 1; i >= 0; i--) v = v * 8 + rot[i];
    return v;
}

static void unpack(int v, uint8_t *rot, int n)
{
    for (int i = 0; i < n; i++, v /= 8) rot[i] = (uint8_t)(v % 8);
}

// Fills s_wins with every arrangement the light gets through. Four rings is 4096 to try.
static void find_wins(const Level *L)
{
    int total = 1;
    for (int i = 0; i < L->n_rings; i++) total *= 8;
    s_n_wins = 0;
    uint8_t rot[MAX_RINGS];
    for (int v = 0; v < total; v++) {
        unpack(v, rot, L->n_rings);
        if (wins(L, rot)) s_wins[s_n_wins++] = (uint16_t)v;
    }
}

// The winning arrangement that takes the fewest rings moved from `rot`, and how many that is
// (99 if there is none). Rings are independent, so that is simply how many differ.
static int nearest_win(const Level *L, const uint8_t *rot, uint8_t *out)
{
    int best = 99;
    for (int k = 0; k < s_n_wins; k++) {
        uint8_t w[MAX_RINGS];
        unpack(s_wins[k], w, L->n_rings);
        int differ = 0;
        for (int i = 0; i < L->n_rings; i++) differ += w[i] != rot[i];
        if (differ < best) {
            best = differ;
            if (out) memcpy(out, w, sizeof(w));
        }
    }
    return best;
}

// The generator's own xorshift, kept rather than handed over to T->random(). A level is
// nothing but the numbers this produces from its own seed, so everybody playing level 12
// has to get the same board - which a console-wide random source could never promise.
typedef struct {
    uint32_t s;
} Rng;

static uint32_t rng_next(Rng *r)
{
    r->s ^= r->s << 13;
    r->s ^= r->s >> 17;
    r->s ^= r->s << 5;
    return r->s;
}

static int rng_below(Rng *r, int n) { return (int)(rng_next(r) % (uint32_t)n); }

// What a level is made of, by how far in it is. The first few are holes only, and are about
// learning to turn a ring. After that one ring (`bent`) has no plain hole in it - only
// mirrors - so the light cannot simply be lined up: it has to be sent round and brought back.
static void tier(int n, int *rings, bool *mirrors, bool *splits, int *bent, float *max_rarity)
{
    *splits = false, *bent = 0;
    if (n <= 3) *rings = 2, *mirrors = false, *max_rarity = 1.0f;
    else if (n <= 7) *rings = 2, *mirrors = true, *bent = 1, *max_rarity = 0.20f;
    else if (n <= 12) *rings = 3, *mirrors = true, *bent = 1, *max_rarity = 0.10f;
    else if (n <= 18) *rings = 3, *mirrors = true, *splits = true, *bent = 1, *max_rarity = 0.08f;
    else if (n <= 30) *rings = 4, *mirrors = true, *splits = true, *bent = 1, *max_rarity = 0.05f;
    else *rings = 4, *mirrors = true, *splits = true, *bent = 2, *max_rarity = 0.03f;
}

// The same level for everybody: the generator is seeded by the level number.
static void make_level(int n, Level *out)
{
    int rings, bent;
    bool mirrors, splits;
    float max_rarity;
    tier(n, &rings, &mirrors, &splits, &bent, &max_rarity);
    Rng rng = {(uint32_t)n * 2654435761u + 977u};
    int total = 1;
    for (int i = 0; i < rings; i++) total *= 8;

    Level L;
    for (int attempt = 0;; attempt++) {
        memset(&L, 0, sizeof(L));
        L.n_rings = (uint8_t)rings;
        // Which rings have no straight way through.
        bool is_bent[MAX_RINGS] = {false};
        for (int k = 0; k < bent; k++) is_bent[rng_below(&rng, rings)] = true;
        for (int i = 0; i < rings; i++) {
            const int open = mirrors ? 3 + rng_below(&rng, 2) : 1 + rng_below(&rng, 2);
            bool split_used = false;
            for (int k = 0; k < open; k++) {
                int s;
                do s = rng_below(&rng, 8);
                while (L.ring[i][s] != BLOCK);
                uint8_t el = GAP;
                const int pick = rng_below(&rng, 10);
                if (mirrors && (pick >= 4 || is_bent[i])) el = pick & 1 ? MIRROR_L : MIRROR_R;
                if (splits && !split_used && !is_bent[i] && pick == 9) el = SPLIT, split_used = true;
                L.ring[i][s] = el;
            }
        }
        find_wins(&L);
        // After a while, take a commoner solution rather than never finish.
        const float ceiling = max_rarity * (attempt < 200 ? 1.0f : 3.0f);
        if (s_n_wins == 0 || (float)s_n_wins / total > ceiling) {
            if (attempt < 600) continue;
            if (s_n_wins == 0) continue;
        }
        break;
    }
    // Start as far from solved as a start can be found: every ring in the wrong place.
    int far = -1;
    for (int k = 0; k < 64 && far < rings; k++) {
        uint8_t rot[MAX_RINGS] = {0};
        unpack(rng_below(&rng, total), rot, rings);
        const int d = nearest_win(&L, rot, NULL);
        if (d > far && d < 99) {
            far = d;
            memcpy(L.start, rot, sizeof(L.start));
        }
    }
    L.best = (uint8_t)(far < 1 ? 1 : far);
    *out = L;
    T->log("level %d: %d rings, %d of %d arrangements win, %d to move", n, rings, s_n_wins, total, L.best);
}

// ---------------------------------------------------------------- sound

static void tone1(float f0, float f1, uint16_t ms, uint8_t wave, float vol, uint16_t delay)
{
    const tat_tone_t t = {f0, f1, ms, wave, vol, delay};
    T->tone(&t);
}

static void sfx_detent(void) { tone1(1200, 0, 25, TAT_SQUARE, 0.3f, 0); }
static void sfx_settle(void) { tone1(800, 500, 40, TAT_SQUARE, 0.5f, 0); }

static void sfx_deeper(int depth)
{
    const float f = 520.0f + 130.0f * depth;
    tone1(f, f * 1.35f, 90, TAT_TRIANGLE, 0.5f, 0);
}

static void sfx_solve(void)
{
    tone1(523, 0, 130, TAT_SQUARE, 0.6f, 0);
    tone1(659, 0, 130, TAT_SQUARE, 0.6f, 140);
    tone1(784, 0, 130, TAT_SQUARE, 0.6f, 280);
    tone1(1047, 0, 220, TAT_SQUARE, 0.7f, 420);
}

// ---------------------------------------------------------------- geometry

// The angle on the screen of one of the board's spokes.
static float spoke_angle(float spoke) { return ((spoke + VIEW) * 45.0f - 90.0f) * PI / 180.0f; }

// How much bigger than the spec the board is drawn, for this level's number of rings.
static float zoom(void) { return BOARD_PX / (GEO[g.lvl.n_rings < 2 ? 2 : g.lvl.n_rings].rad[0] + TEETH_OUT); }

// The star fills the hole in the innermost ring, in the spec's units.
static float star_r(void)
{
    const int n = g.lvl.n_rings < 2 ? 2 : g.lvl.n_rings;
    return GEO[n].rad[n - 1] - BAND_HALF - 1.5f;
}

// A point on the board: r in the spec's units, turned and scaled onto the canvas.
static Pt polar(float r, float spoke)
{
    const float a = spoke_angle(spoke), k = zoom();
    const Pt p = {C + r * k * cosf(a), C + r * k * sinf(a)};
    return p;
}

// ---------------------------------------------------------------- persistence

static void load_progress(void)
{
    T->save_get("depth", &g.depth, 0);
    T->save_get("level", &g.level, 0);
    if (g.depth < 1) g.depth = 1;
    if (g.level < 1) g.level = 1;
    if (g.level > g.depth) g.level = g.depth;
    // A new key: the rules are new, so everyone is shown them once, whatever they had seen.
    int seen = 0;
    T->save_get("seen_tut3", &seen, 0);
    g.seen_tut = seen != 0;
}

static void save_progress(void)
{
    T->save_set("depth", g.depth);
    T->save_set("level", g.level);
    T->save_set("seen_tut3", g.seen_tut);
}

// ---------------------------------------------------------------- level flow

// Beam walk with geometry for drawing (spec section 4.2).
static void retrace(bool sounds)
{
    const Geo *G = &GEO[g.lvl.n_rings];
    g.n_segs = g.n_stops = 0;
    Branch cur[8], nxt[8];
    int n_cur = 1, deepest = 0;
    cur[0].ring = 0;
    cur[0].from_r = EMIT_PX / zoom();
    cur[0].from_s = LASER;
    cur[0].meet_s = LASER;
    bool win = false;
    for (int guard = 0; n_cur > 0 && guard < 16; guard++) {
        int n_nxt = 0;
        for (int i = 0; i < n_cur; i++) {
            const Branch b = cur[i];
            const float R = G->rad[b.ring];
            const Pt p0 = polar(b.from_r, (float)b.from_s), p1 = polar(R, (float)b.meet_s);
            if (g.n_segs < 32) {
                const Seg sg = {p0.x, p0.y, p1.x, p1.y};
                g.segs[g.n_segs++] = sg;
            }
            const uint8_t el = g.lvl.ring[b.ring][m8(b.meet_s - g.rot[b.ring])];
            const uint8_t mask = outs(el, b.meet_s);
            if (!mask) {
                if (g.n_stops < 12) g.stops[g.n_stops++] = p1;
                continue;
            }
            if (b.ring + 1 > deepest) deepest = b.ring + 1;
            for (int s = 0; s < SPOKES; s++) {
                if (!(mask & (1u << s))) continue;
                if (b.ring == g.lvl.n_rings - 1) {
                    // Through the last ring: onto the star's red box, or into the dark beside it.
                    const Pt e = polar(star_r() * (s == DOOR ? 0.86f : 1.0f), (float)s);
                    if (g.n_segs < 32) {
                        const Seg seg = {p1.x, p1.y, e.x, e.y};
                        g.segs[g.n_segs++] = seg;
                    }
                    if (s == DOOR) win = true;
                    else if (g.n_stops < 12) g.stops[g.n_stops++] = e;
                } else if (n_nxt < 8) {
                    nxt[n_nxt].ring = b.ring + 1;
                    nxt[n_nxt].from_r = R;
                    nxt[n_nxt].from_s = b.meet_s;
                    nxt[n_nxt].meet_s = s;
                    n_nxt++;
                }
            }
        }
        memcpy(cur, nxt, sizeof(Branch) * n_nxt);
        n_cur = n_nxt;
    }
    // "Getting warmer": a rising blip whenever the light gets one ring deeper than before.
    if (sounds && deepest > g.reached && !win) sfx_deeper(deepest);
    if (deepest > g.reached) g.idle_t = 0;   /* that was progress */
    g.reached = deepest;
    g.won = win;   /* the star opens its eyes the moment the light is on it, finger down or not */
    g.dirty = true;
}

// Called when a ring is let go of with the light on the star.
static void solve_level(void)
{
    g.phase = SOLVED;
    g.phase_t = 0;
    g.stars_earned = g.moves <= g.lvl.best ? 3 : g.moves <= g.lvl.best + 2 ? 2 : 1;
    if (g.hinted && g.stars_earned > 2) g.stars_earned = 2;   /* help is free, but it is not perfect */
    g.hint_t = 0;
    if (g.level + 1 > g.depth) g.depth = g.level + 1;
    save_progress();
    sfx_solve();
    g.dirty = true;
}

static void start_level(int n)
{
    g.level = n < 1 ? 1 : n;
    make_level(g.level, &g.lvl);
    memcpy(g.rot, g.lvl.start, sizeof(g.rot));
    for (int i = 0; i < MAX_RINGS; i++) g.vis_from[i] = g.vis_to[i] = g.rot[i];
    g.tween_t = 1;
    g.grab = -1;
    g.touch_on_ring = false;
    g.moves = 0;
    g.reached = 0;
    g.won = false;
    g.hint_t = g.idle_t = 0;
    g.hint_ring = -1;
    g.hinted = false;
    g.phase = g.seen_tut ? PLAYING : TUTORIAL;
    g.phase_t = 0;
    retrace(false);
    g.dirty = true;
    save_progress();
}

// What the star says when it is asked: which ring is in the wrong place. Not where it should
// go - finding that, with the light showing the way, is the game.
static void give_hint(void)
{
    uint8_t want[MAX_RINGS] = {0};
    g.hint_ring = -1;
    if (nearest_win(&g.lvl, g.rot, want) < 99)
        for (int i = 0; i < g.lvl.n_rings && g.hint_ring < 0; i++)
            if (want[i] != g.rot[i]) g.hint_ring = i;
    if (g.hint_ring < 0) return;
    g.hint_t = HINT_S;
    g.hinted = true;
    g.idle_t = 0;
    g.dirty = true;
    tone1(660, 0, 60, TAT_TRIANGLE, 0.4f, 0);
    tone1(990, 0, 90, TAT_TRIANGLE, 0.4f, 70);
    T->log("hint on level %d: ring %d", g.level, g.hint_ring);
}

// ---------------------------------------------------------------- turning a ring

static float wrap_pi(float a)
{
    while (a > PI) a -= TAU;
    while (a < -PI) a += TAU;
    return a;
}

static float minf(float a, float b) { return a < b ? a : b; }

// Where a ring is drawn right now, in places round from its spoke 0.
static float vis_rot(int i) { return g.vis_from[i] + (g.vis_to[i] - g.vis_from[i]) * g.tween_t; }

// The ring a touch is on: the nearest band, with most of the space between bands counting
// as the band - a finger is wider than a ring. -1 on the star or out on the rim.
static int ring_at(float r)
{
    const Geo *G = &GEO[g.lvl.n_rings];
    if (r < star_r() || r > G->rad[0] + 14) return -1;
    int hit = 0;
    for (int i = 1; i < g.lvl.n_rings; i++)
        if (fabsf(r - G->rad[i]) < fabsf(r - G->rad[hit])) hit = i;
    return hit;
}

static void update_ring(const tat_input_t *in)
{
    const float dx = in->touch.x / (float)SCALE - C, dy = in->touch.y / (float)SCALE - C;
    const float r_px = hypotf(dx, dy), a = atan2f(dy, dx);

    if (in->touch.pressed) {
        g.grab = ring_at(r_px / zoom());
        g.touch_on_ring = g.grab >= 0;
        if (g.grab >= 0) {
            // Taken where it is drawn, even if it was still settling from the last turn.
            g.grab_vis = vis_rot(g.grab);
            for (int i = 0; i < MAX_RINGS; i++) g.vis_from[i] = g.vis_to[i] = i == g.grab ? g.grab_vis : g.vis_to[i];
            g.tween_t = 1;
            g.turned = 0;
            g.last_a = a;
            g.grab_rot = g.rot[g.grab];
        }
    }
    if (g.grab < 0) return;
    const int k = g.grab;

    if (in->touch.down) {
        // Too near the middle an angle means nothing: a hair's movement is half a turn.
        if (r_px > 10) {
            g.turned += wrap_pi(a - g.last_a) / (PI / 4);
            g.last_a = a;
        }
        const float vis = g.grab_vis + g.turned;
        if (vis != g.vis_to[k]) {
            g.vis_from[k] = g.vis_to[k] = vis;
            g.dirty = true;
        }
        const uint8_t place = (uint8_t)m8((int)lroundf(vis));
        if (place != g.rot[k]) {
            g.rot[k] = place;
            g.hint_t = 0;   /* whatever was suggested, something is being done about it */
            sfx_detent();
            retrace(true);
        }
    }
    if (in->touch.released) {
        // Let go of between places, it settles into the nearest one.
        g.vis_from[k] = g.vis_to[k];
        g.vis_to[k] = roundf(g.vis_to[k]);
        g.tween_t = g.vis_from[k] == g.vis_to[k] ? 1 : 0;
        if (g.rot[k] != g.grab_rot) {
            g.moves++;
            sfx_settle();
        }
        g.grab = -1;
        g.dirty = true;
        if (g.won) solve_level();
    }
}

// ---------------------------------------------------------------- update

static void star_update(float dt)
{
    if (!g.cv) return;
    if (dt > 0.1f) dt = 0.1f;

    const tat_input_t *in = T->input();
    const tat_gestures_t *ges = T->gestures();
    g.phase_t += dt;

    if (T->menu_is_open()) {
        switch (T->menu_update()) {
        case 0: start_level(g.level >= g.depth ? 1 : g.level + 1); break;   /* step through the levels reached so far */
        case 1: T->menu_toggle_sound(); break;
        case 2:
            start_level(g.level);
            T->menu_close();
            break;
        }
        if (!T->menu_is_open()) g.dirty = true;   /* the board is showing again */
        return;
    }
    // A swipe that began on a ring was a turn of that ring, however fast and however straight.
    if ((ges->swipe_left && !g.touch_on_ring) || ((in->clicked & TAT_BTN_B) && g.phase != TUTORIAL)) {
        g.grab = -1;
        T->menu_open();
        return;
    }
    if (g.tween_t < 1) {
        g.tween_t = minf(1.0f, g.tween_t + dt / SNAP_S);
        g.dirty = true;
    }
    if (g.phase == SOLVED && g.phase_t < 0.6f) g.dirty = true;   /* banner pop */

    if (g.phase == TUTORIAL) {
        if (ges->tap) {
            g.seen_tut = true;
            save_progress();
            g.phase = PLAYING;
            g.dirty = true;
        }
        return;
    }
    if (g.phase == SOLVED) {
        if ((ges->tap && g.phase_t > 0.8f) || g.phase_t > 4.0f) start_level(g.level + 1);
        return;
    }

    if (g.hint_t > 0) {
        g.hint_t -= dt;
        g.dirty = true;   /* it pulses */
    }
    // Nothing getting any deeper for a while: the star lets it be known that it can help.
    g.idle_t += dt;
    if (g.idle_t > STUCK_S && g.hint_t <= 0) {
        g.blink_t += dt;
        g.dirty = true;
    }

    update_ring(in);
    if (g.phase != PLAYING) return;
    if (ges->tap && !g.touch_on_ring && hypotf(ges->x - 233.0f, ges->y - 233.0f) / SCALE / zoom() < star_r()) give_hint();
}

// ---------------------------------------------------------------- drawing

// Background, bands, teeth, holes and glyphs in one pass over the canvas. This is what the
// polar tables are for: an atan2 and a sqrt per pixel here would be 54,000 of each a frame.
static void draw_board(void)
{
    uint8_t *px = T->canvas_pixels(g.cv);
    if (!px) return;
    if (!g.pol_a || !g.pol_r) {
        // No tables, no board. Clear to the surround so the star, the beam and the HUD still
        // land on something rather than on whatever the last frame left behind.
        T->canvas_clear(g.cv, g.c_void);
        return;
    }
    const Geo *G = &GEO[g.lvl.n_rings];
    int32_t rot16[MAX_RINGS];
    // Each ring's angle, and the half turn the whole board is drawn round by.
    for (int i = 0; i < g.lvl.n_rings; i++) rot16[i] = (int32_t)((vis_rot(i) + VIEW) * 8192.0f);
    const float inv_zoom = 1.0f / zoom(), disc_r = DISC_PX * inv_zoom;
    const bool pulse = g.hint_t > 0 && g.hint_ring >= 0 && ((int)(g.hint_t * 5.0f) & 1) == 0;
    for (int y = 0; y < CW; y++) {
        for (int x = 0; x < CW; x++) {
            const int idx = (SCALE * y) * TAT_SCREEN + SCALE * x;
            const float r = g.pol_r[idx] / 32.0f * inv_zoom;   /* in the spec's units from here on */
            uint8_t c = r < disc_r ? g.c_disc : g.c_void;
            if (r < G->rad[0] + 10 && r > G->star) {
                for (int k = 0; k < g.lvl.n_rings; k++) {
                    const float dr = r - G->rad[k];
                    if (dr < -BAND_HALF || dr > 9.5f) continue;
                    /* Angle in the ring's own frame, 0 at its spoke 0. */
                    const uint16_t a16 = (uint16_t)((int32_t)g.pol_a[idx] + 16384 - rot16[k]);
                    if (dr <= BAND_HALF) {
                        c = pulse && k == g.hint_ring ? g.c_hint : g.c_plate[k];
                        const int spoke = ((a16 + 4096) >> 13) & 7;
                        const uint8_t el = g.lvl.ring[k][spoke];
                        if (el != BLOCK) {
                            const int16_t off = (int16_t)((uint16_t)(a16 + 4096 - (spoke << 13))) - 4096;
                            const float tang = off * (TAU / 65536.0f) * r;   /* + = clockwise */
                            if (fabsf(tang) <= HOLE_HALF) {
                                c = g.c_disc;   /* a hole: the wall is cut here */
                                /* Glyphs are 5x5 grids of 2 px blocks; x runs outward, y clockwise. */
                                const int bx = (int)floorf((dr + 5.0f) / 2.0f), by = (int)floorf((tang + 5.0f) / 2.0f);
                                const bool in_grid = bx >= 0 && bx < 5 && by >= 0 && by < 5;
                                if (el == GAP) {
                                    if (fabsf(tang) >= 5.25f) c = g.c_lip;
                                } else if (el == MIRROR_L) {
                                    if (in_grid && bx == by) c = g.c_mirror;
                                } else if (el == MIRROR_R) {
                                    if (in_grid && bx + by == 4) c = g.c_mirror;
                                } else if (in_grid && bx + by == 4 && bx != 2) {
                                    c = g.c_split;
                                }
                            }
                        }
                    } else if (dr >= 6.0f) {
                        // Teeth, so rotation is visible: every tooth index that isn't a spoke.
                        const int nt = G->teeth[k];
                        const uint32_t t16 = (uint32_t)a16 * nt + 32768;
                        const int ti = (int)(t16 >> 16) % nt;
                        if (ti % (nt / 8) != 0) {
                            const float toff = ((int32_t)(t16 & 0xFFFF) - 32768) / 65536.0f * (TAU * r / nt);
                            if (fabsf(toff) <= 2.5f && dr <= 9.0f) c = g.c_tooth[k];
                        }
                    }
                    break;
                }
            }
            px[y * CW + x] = c;
        }
    }
}

static void thick_line(const Seg *s, int w, uint8_t col)
{
    const float dx = s->x1 - s->x0, dy = s->y1 - s->y0;
    const float len = fmaxf(0.001f, hypotf(dx, dy));
    const float nx = -dy / len, ny = dx / len;
    for (int k = 0; k < w; k++) {
        const float o = k - (w - 1) / 2.0f;
        T->canvas_line(g.cv, (int)lroundf(s->x0 + nx * o), (int)lroundf(s->y0 + ny * o),
                       (int)lroundf(s->x1 + nx * o), (int)lroundf(s->y1 + ny * o), col);
    }
}

// A rectangle in the emitter's frame: u runs outward along the spoke, v across it.
static void spoke_rect(float ang, float u0, float u1, float half_v, uint8_t col)
{
    const float ca = cosf(ang), sa = sinf(ang);
    const int x0 = (int)(C + ca * (u0 + u1) / 2 - 12), y0 = (int)(C + sa * (u0 + u1) / 2 - 12);
    for (int y = y0; y < y0 + 24; y++)
        for (int x = x0; x < x0 + 24; x++) {
            const float rx = x + 0.5f - C, ry = y + 0.5f - C;
            const float u = rx * ca + ry * sa, v = -rx * sa + ry * ca;
            if (u >= u0 && u <= u1 && fabsf(v) <= half_v) T->canvas_pixel(g.cv, x, y, col);
        }
}

// The star: five points, chubby, with a face. Asleep it is a soft yellow with its eyes shut;
// with the light on its box it is bright, wide awake and smiling. Its top point carries the
// red box the light has to reach.
static void draw_star(void)
{
    const bool awake = g.won;
    const float k = zoom(), R = star_r() * k;
    // Offering a hint, the star stirs: it brightens and dims.
    const bool stir = !awake && g.phase == PLAYING && g.idle_t > STUCK_S && g.hint_t <= 0 && ((int)(g.blink_t * 2.0f) & 1);
    const uint8_t body = awake || stir ? g.c_star_lit : g.c_star;
    const float fat = 0.60f, s36 = 0.587785f, c36 = 0.809017f;   /* how deep the notches are: not very */
    const float reach = R + (awake ? 7 : 0);
    const int ri = (int)ceilf(reach) + 1;
    for (int y = -ri; y <= ri; y++)
        for (int x = -ri; x <= ri; x++) {
            const float rr = hypotf((float)x, (float)y);
            if (rr > reach) continue;
            // The angle from straight up, folded into the tenth of a turn between a point and a notch.
            float a = fmodf(fabsf(atan2f((float)x, (float)-y)), TAU / 5);
            if (a > TAU / 10) a = TAU / 5 - a;
            const float edge = fat * s36 / (cosf(a) * fat * s36 - sinf(a) * (fat * c36 - 1.0f));   /* of R */
            uint8_t c;
            if (rr <= (R - 1.6f) * edge) c = body;
            else if (rr <= R * edge) c = g.c_star_edge;
            else if (awake && rr <= reach * edge) c = g.c_halo;
            else continue;
            T->canvas_pixel(g.cv, (int)C + x, (int)C + y, c);
        }

    // The face sits a little low: a star's middle is below where its points make it look.
    const int u0 = (int)lroundf(R / 8), u = u0 < 1 ? 1 : u0;
    const int cx = (int)C, cy = (int)C + u;
    if (awake) {
        T->canvas_fill_rect(g.cv, cx - 3 * u, cy - 2 * u, 2 * u, 2 * u, g.c_face);
        T->canvas_fill_rect(g.cv, cx + u, cy - 2 * u, 2 * u, 2 * u, g.c_face);
        T->canvas_fill_rect(g.cv, cx - 2 * u, cy + 2 * u, 4 * u, u, g.c_face);
        T->canvas_fill_rect(g.cv, cx - 3 * u, cy + u, u, u, g.c_face);
        T->canvas_fill_rect(g.cv, cx + 2 * u, cy + u, u, u, g.c_face);
    } else {
        T->canvas_fill_rect(g.cv, cx - 3 * u, cy - u, 2 * u, u, g.c_face);
        T->canvas_fill_rect(g.cv, cx + u, cy - u, 2 * u, u, g.c_face);
        T->canvas_fill_rect(g.cv, cx - u / 2 - (u > 1), cy + 2 * u, u + (u > 1), u, g.c_face);
    }
    if (u >= 2) {
        T->canvas_fill_rect(g.cv, cx - 5 * u, cy + u, u, u, g.c_cheek);
        T->canvas_fill_rect(g.cv, cx + 4 * u, cy + u, u, u, g.c_cheek);
    }

    // The red box, on the star's top point: what the light is for.
    const int half = (int)fmaxf(3.0f, R * 0.18f);
    const int by = (int)lroundf(C - R * 0.86f);
    T->canvas_fill_rect(g.cv, cx - half - 1, by - half - 1, 2 * half + 3, 2 * half + 3, awake ? g.c_white : g.c_box_edge);
    T->canvas_fill_rect(g.cv, cx - half, by - half, 2 * half + 1, 2 * half + 1, awake ? g.c_box_lit : g.c_box);
    if (awake)
        for (int s = 1; s < 5; s++) {   /* a sparkle off every point but the one the light is on */
            const float a = -PI / 2 + s * TAU / 5;
            T->canvas_fill_rect(g.cv, (int)(C + cosf(a) * (R + 10)) - 1, (int)(C + sinf(a) * (R + 10)) - 1, 3, 3, g.c_rays);
        }
}

static void banner(const char *top, const char *mid, const char *bottom, uint8_t col)
{
    const tat_banner_t b = {
        .top = top, .mid = mid, .bottom = bottom,
        .top_color = col, .mid_color = g.c_text, .bottom_color = g.c_star_lit,
        .panel = g.c_panel, .border = col,
        .top_scale = 2, .bars = true, .bottom_bold = true,
    };
    T->canvas_banner(g.cv, (int)C, 164, CW - 40, &b);   /* under the star, which has just woken up and should be seen */
}

static void draw_menu(void)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%d / %d", g.level, g.depth);
    const tat_menu_row_t rows[] = {
        {"LEVEL", buf, 0},
        T->menu_sound_row(),
        {"RESET LEVEL", "GO", T->ui_color(TAT_UI_ACCENT)},
    };
    T->menu_draw(rows, 3, "PAUSED");
}

static void star_draw(void)
{
    if (!g.cv) return;
    if (T->menu_is_open()) {
        draw_menu();
        return;
    }
    // Nothing moves unless something changed, so don't redraw (or send a frame).
    if (!g.dirty) return;
    g.dirty = false;

    draw_board();
    for (int i = 0; i < g.n_segs; i++) thick_line(&g.segs[i], 5, g.c_glow);
    for (int i = 0; i < g.n_segs; i++) thick_line(&g.segs[i], 3, g.c_beam);
    for (int i = 0; i < g.n_segs; i++) thick_line(&g.segs[i], 1, g.c_white);
    draw_star();   /* over the end of the beam, so the light goes into the box rather than across it */
    for (int i = 0; i < g.n_stops; i++) {
        T->canvas_fill_rect(g.cv, (int)g.stops[i].x - 2, (int)g.stops[i].y - 2, 5, 5, g.c_stop);
        T->canvas_fill_rect(g.cv, (int)g.stops[i].x - 7, (int)g.stops[i].y - 1, 3, 3, g.c_stop);
        T->canvas_fill_rect(g.cv, (int)g.stops[i].x + 5, (int)g.stops[i].y - 1, 3, 3, g.c_stop);
    }
    /* The emitter, at the top of the rim, pointing in. It does not move. */
    const float ea = spoke_angle((float)LASER);
    spoke_rect(ea, 105, 116, 8.0f, g.c_emit);
    spoke_rect(ea, 109, 113, 5.5f, g.c_emit_hi);
    spoke_rect(ea, EMIT_PX, 105, 3.8f, g.c_beam);
    spoke_rect(ea, EMIT_PX - 3, EMIT_PX, 2.0f, g.c_white);

    // HUD: one line, in the sliver of screen under the board. Level, and rings moved against
    // the fewest it takes. While a hint is up it says what the hint means instead.
    char buf[32];
    const char *line = buf;
    uint8_t line_col = g.moves > g.lvl.best ? g.c_orange : g.c_dim;
    snprintf(buf, sizeof(buf), "L%d  %d/%d", g.level, g.moves, g.lvl.best);
    if (g.hint_t > 0) line = "TURN THIS RING", line_col = g.c_hint;
    else if (g.phase == PLAYING && g.idle_t > STUCK_S) line = "STUCK? TAP STAR", line_col = g.c_hint;
    T->canvas_fill_rect(g.cv, (int)C - 47, 216, 95, 11, g.c_void);
    T->canvas_text_centered(g.cv, (int)C, 221, line, line_col, 1, true);

    if (g.phase == TUTORIAL) {
        T->canvas_fill_rect(g.cv, 22, 46, CW - 44, 142, g.c_panel);
        T->canvas_rect(g.cv, 22, 46, CW - 44, 142, g.c_beam);
        T->canvas_rect(g.cv, 23, 47, CW - 46, 140, g.c_beam);
        T->canvas_text_centered(g.cv, (int)C, 60, "WAKE THE STAR", g.c_beam, 2, true);
        T->canvas_text_centered(g.cv, (int)C, 84, "LIGHT UP ITS RED BOX", g.c_text, 1, true);
        T->canvas_text(g.cv, 34, 102, "DRAG", g.c_cyan, 1, true);
        T->canvas_text(g.cv, 74, 102, "A RING ROUND LIKE THE", g.c_text, 1, false);
        T->canvas_text(g.cv, 74, 112, "DIAL OF A LOCK", g.c_text, 1, false);
        T->canvas_text(g.cv, 34, 128, "HOLES", g.c_beam, 1, true);
        T->canvas_text(g.cv, 74, 128, "LET LIGHT THROUGH.", g.c_text, 1, false);
        T->canvas_text(g.cv, 74, 138, "MIRRORS BEND IT ROUND", g.c_text, 1, false);
        T->canvas_text(g.cv, 34, 154, "STUCK?", g.c_hint, 1, true);
        T->canvas_text(g.cv, 82, 154, "TAP THE STAR", g.c_text, 1, false);
        T->canvas_text_centered(g.cv, (int)C, 175, "TAP TO START", g.c_beam, 1, true);
    } else if (g.phase == SOLVED && g.phase_t > 0.25f) {
        char mid[64], stars[8] = "";
        const char *ring_s = g.moves == 1 ? "RING" : "RINGS";
        if (g.moves <= g.lvl.best && !g.hinted) snprintf(mid, sizeof(mid), "PERFECT - %d %s TURNED", g.moves, ring_s);
        else if (g.moves <= g.lvl.best) snprintf(mid, sizeof(mid), "%d %s, WITH A HINT", g.moves, ring_s);
        else snprintf(mid, sizeof(mid), "%d TURNS - IT TAKES %d", g.moves, g.lvl.best);
        for (int i = 0; i < g.stars_earned; i++) strcat(stars, "* ");
        banner(CHEERS[g.level % 6], mid, stars, g.c_star_lit);
    }
    T->canvas_present(g.cv);
}

// ---------------------------------------------------------------- the game

// The spec gives the palette as hex triples; unpack one into a canvas index.
static uint8_t pal(uint32_t hex) { return T->canvas_color(g.cv, T->rgb((hex >> 16) & 255, (hex >> 8) & 255, hex & 255)); }

static void star_begin(const tat_api_t *api)
{
    T = api;
    memset(&g, 0, sizeof(g));
    g.level = 1;
    g.depth = 1;
    g.grab = -1;
    g.phase = PLAYING;
    g.tween_t = 1;
    g.dirty = true;

    g.cv = T->canvas_create(SCALE, 0);
    if (!g.cv) {
        T->log("no memory for the canvas");
        return;
    }
    g.pol_a = T->polar_angles();
    g.pol_r = T->polar_radii();
    if (!g.pol_a || !g.pol_r) T->log("no polar tables; the board cannot be drawn");

    static const uint32_t PLATE[4] = {0x2E3550, 0x343C5C, 0x3A4368, 0x404A74};
    static const uint32_t TOOTH[4] = {0x414B70, 0x48537C, 0x4F5B88, 0x566394};
    g.c_void = pal(0x0E0E1C);
    g.c_disc = pal(0x16162A);
    for (int i = 0; i < 4; i++) {
        g.c_plate[i] = pal(PLATE[i]);
        g.c_tooth[i] = pal(TOOTH[i]);
    }
    g.c_lip = pal(0x4A5580);
    g.c_mirror = pal(0xDCEBFF);
    g.c_split = pal(0x7FE8FF);
    g.c_beam = pal(0xFF4DD2);
    g.c_glow = pal(0x5C2458);   /* the beam's soft halo over the dark disc */
    g.c_white = pal(0xFFFFFF);
    g.c_star = pal(0xE9BE2E);       /* asleep: a soft yellow */
    g.c_star_lit = pal(0xFFE14A);   /* awake */
    g.c_star_edge = pal(0xB07A12);
    g.c_halo = pal(0x5A4E1E);
    g.c_box = pal(0xE8302A);
    g.c_box_edge = pal(0x7A1512);
    g.c_box_lit = pal(0xFFF3A0);
    g.c_face = pal(0x5A3A00);
    g.c_cheek = pal(0xFF9A6B);
    g.c_text = pal(0xEAF0FF);
    g.c_dim = pal(0x8A97C0);
    g.c_stop = pal(0x6E7BA9);
    g.c_emit = pal(0x3A4260);
    g.c_emit_hi = pal(0x8A97C1);
    g.c_panel = pal(0x07070F);
    g.c_rays = pal(0xFFF3A1);
    g.c_orange = pal(0xFF9A4D);
    g.c_cyan = pal(0x7FE8FE);
    g.c_hint = pal(0xFFD93D);   /* the ring the star points at when it is asked */

    load_progress();
    start_level(g.level);
}

static void star_enter(void)
{
    g.dirty = true;
    g.grab = -1;
}

static void star_redraw(void) { g.dirty = true; }

static void star_unload(void)
{
    if (g.cv) T->canvas_destroy(g.cv);
    g.cv = NULL;
}

const tat_game_t tat_game = {
    .magic = TAT_GAME_MAGIC,
    .api_major = TAT_API_MAJOR,
    .api_minor = TAT_API_MINOR,
    // "sleepystar" rather than "star": the id is also the save namespace, and this is
    // where the player's depth has always lived. Renaming it would quietly put everyone
    // back on level 1.
    .id = "sleepystar",
    .name = "SLEEPY STAR",
    .accent_r = 255, .accent_g = 217, .accent_b = 61,
    .assets = NULL,
    .asset_count = 0,
    .begin = star_begin,
    .enter = star_enter,
    .update = star_update,
    .draw = star_draw,
    .leave = NULL,
    .unload = star_unload,
    .redraw = star_redraw,
    .keep_awake = NULL,   /* a puzzle: no need to hold off sleep */
};
