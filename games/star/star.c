// SLEEPY STAR - a laser falls straight down onto rings of walls with gaps, mirrors and
// splitters cut into them. Line them up so the light reaches the star's door.
//
//   turn watch  - free: the laser always comes from real-world "up", so turning the
//                 watch moves where it enters (8 notches around the rim)
//   tap a ring  - clicks it round one notch; the ring inside it turns the other way.
//                 Taps are what's counted, against the level's par
//   tap the star - a hint: the ring to tap next, or where the laser should come from
//   PWR         - start the level over
//   swipe left  - pause menu
//
// Three things were changed after it was played. The star's door was on its far side - the
// spec puts it at spoke 4, straight down, and the laser comes from the top, so held the way
// a watch is held the light was aimed at the star's back and a straight shot needed the
// watch upside down. The picture is now drawn half a turn round (VIEW), which puts the door
// under the laser and changes nothing else: every level, every solution and every saved
// best is what it was. The board filled barely half the screen, with a reset button beside
// it that PWR and the menu already were; it now fills it. And it asked for a little more
// working-out than most people bring to a watch, with nothing to fall back on - so the star
// gives hints, from the solver that was already in here checking the levels.
//
// Built from docs/SLEEPY_STAR_SPEC.md. The board is fixed to the watch; the laser emitter
// orbits the rim to wherever real-world "up" is (8 spokes, with hysteresis). Geometry in
// the spec is in device pixels; this draws on the 2x pixel canvas, so every spec number
// appears here halved.
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

// ---------------------------------------------------------------- model (spec section 4)

enum { BLOCK = 0, GAP, MIRROR_L, MIRROR_R, SPLIT };

#define SPOKES 8
#define MAX_RINGS 4
#define DOOR 4
#define VIEW 4   /* the board is drawn this many spokes round, which puts the door at the top */

typedef struct {
    uint8_t n_rings;
    uint8_t ring[MAX_RINGS][SPOKES];   /* ring[0] = outermost */
    uint8_t start[MAX_RINGS];
    uint8_t best;                      /* verified minimum taps */
} Level;

static int m8(int n) { return ((n % 8) + 8) % 8; }

// Short names so a level's eight spokes fit on one line and can be read as a picture. The
// C++ called the wall `_`; a bare underscore is legal C but unreadable, so it is W here.
enum { O = GAP, L_ = MIRROR_L, R_ = MIRROR_R, Y = SPLIT, W = BLOCK };

typedef struct {
    int n;
    Level lvl;
} Campaign;

// The six machine-verified campaign levels (spec section 7). C++ would fill the unwritten
// rings in from a bare {}; C needs at least one initialiser per aggregate, which is why the
// short levels below still spell out every brace they open.
static const Campaign CAMPAIGN[] = {
    {1, {2, {{O, W, O, W, W, W, O, W}, {W, W, W, O, O, W, W, O}}, {3, 2}, 1}},
    {2, {2, {{W, W, W, W, W, O, W, O}, {W, W, W, O, O, W, W, W}}, {7, 6}, 2}},
    {4, {2, {{W, W, W, W, O, L_, L_, W}, {W, W, W, W, O, O, L_, W}}, {5, 0}, 3}},
    {7, {3, {{W, W, O, L_, W, W, W, O}, {W, L_, O, W, W, W, W, R_}, {W, L_, R_, W, R_, W, W, W}}, {3, 4, 2}, 3}},
    {11, {3, {{W, W, O, L_, W, O, W, W}, {O, W, W, W, W, L_, R_, W}, {R_, W, W, L_, Y, W, W, L_}}, {4, 3, 0}, 4}},
    {16,
     {4,
      {{L_, W, W, W, O, W, L_, O}, {W, R_, W, W, W, R_, W, L_}, {L_, W, O, R_, W, R_, W, W}, {W, O, W, W, W, R_, R_, W}},
      {0, 6, 1, 6},
      5}},
};

#define CAMPAIGN_N ((int)(sizeof(CAMPAIGN) / sizeof(CAMPAIGN[0])))

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

#define MAX_STATES 4096

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

static const char *const CHEERS[] = {"NICE!", "YES!", "GOT IT!", "LOVELY!", "BRIGHT!", "WAHOO!"};

static struct {
    tat_canvas_t *cv;
    const uint16_t *pol_a, *pol_r;   /* the screen's polar tables, fetched once */

    // The breadth-first solver's working tables. The C++ parked these in PSRAM with
    // EXT_RAM_BSS_ATTR, which a packaged game has no way to ask for: where its memory comes
    // from is the console's business, so they are taken from T->alloc() instead and given
    // back on unload.
    int8_t *dist;      /* taps from each ring configuration to the nearest winning one */
    uint16_t *queue;

    /* progress */
    int level, depth;
    bool seen_tut, flat_play;

    /* play */
    Level lvl;
    uint8_t rot[MAX_RINGS];
    int entry;
    int taps;
    Phase phase;
    float phase_t;
    int reached;   /* how many rings the beam got through on the last trace */
    bool won;
    Seg segs[32];
    int n_segs;
    Pt stops[12];
    int n_stops;

    /* ring tween: both rings step to their new angle in 3 jumps over 260 ms */
    float vis_from[MAX_RINGS], vis_to[MAX_RINGS];
    float tween_t;

    /* tilt (spec section 5) */
    float up;   /* filtered screen angle of world-up */
    bool have_up;
    float flat_t;
    bool nudge;
    float gyro_sign, sign_score, last_target;
    bool had_target;

    /* ui */
    bool dirty;
    int64_t last_tap_us;
    int stars_earned;

    /* hints */
    float hint_t;      /* > 0 while one is showing */
    int hint_ring;     /* the ring to tap, or -1 */
    int hint_entry;    /* the spoke the laser should come in on, or -1 */
    bool hinted;       /* this level has had one, which caps it at two stars */
    float idle_t;      /* since the player last got anywhere */
    float blink_t;

    /* palette */
    uint8_t c_void, c_disc, c_bezel, c_plate[4], c_tooth[4], c_lip, c_mirror, c_split;
    uint8_t c_beam, c_glow, c_white, c_asleep, c_awake, c_halo, c_door, c_door_lit;
    uint8_t c_face_lit, c_text, c_dim, c_stop, c_emit, c_emit_hi, c_panel, c_rays;
    uint8_t c_orange, c_pip_edge, c_cyan, c_hint;
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

// Does any branch reach the door, entering on any spoke in `entry_mask`?
static bool wins(const Level *L, const uint8_t *rot, uint8_t entry_mask)
{
    uint8_t mask = entry_mask;
    for (int i = 0; i < L->n_rings && mask; i++) {
        uint8_t next = 0;
        for (int s = 0; s < SPOKES; s++)
            if (mask & (1u << s)) next |= outs(L->ring[i][m8(s - rot[i])], s);
        mask = next;
    }
    return (mask & (1u << DOOR)) != 0;
}

// ---------------------------------------------------------------- solver / generator (spec section 8)

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

// Fills g.dist for every configuration; returns how many of them win at some wrist angle.
// Only called once the tables are known to exist.
static int solve(const Level *L)
{
    int total = 1;
    for (int i = 0; i < L->n_rings; i++) total *= 8;
    int head = 0, tail = 0, winners = 0;
    uint8_t rot[MAX_RINGS];
    for (int v = 0; v < total; v++) {
        unpack(v, rot, L->n_rings);
        if (wins(L, rot, 0xFF)) {
            g.dist[v] = 0;
            g.queue[tail++] = (uint16_t)v;
            winners++;
        } else {
            g.dist[v] = -1;
        }
    }
    // Breadth-first backwards: the state before "tap ring i" has ring i one notch back and
    // the ring inside it one notch forward.
    while (head < tail) {
        const int v = g.queue[head++];
        unpack(v, rot, L->n_rings);
        for (int i = 0; i < L->n_rings; i++) {
            uint8_t p[MAX_RINGS];
            memcpy(p, rot, sizeof(p));
            p[i] = (uint8_t)m8(p[i] - 1);
            if (i < L->n_rings - 1) p[i + 1] = (uint8_t)m8(p[i + 1] + 1);
            const int pv = pack(p, L->n_rings);
            if (g.dist[pv] < 0) {
                g.dist[pv] = (int8_t)(g.dist[v] + 1);
                g.queue[tail++] = (uint16_t)pv;
            }
        }
    }
    return winners;
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

// Tier schedule (spec section 8): rings, elements, target taps, rarity ceiling.
static void tier(int n, int *rings, bool *mirrors, bool *splits, int *taps, float *max_rarity)
{
    if (n <= 3) {
        *rings = 2, *mirrors = false, *splits = false, *taps = n <= 1 ? 1 : 2, *max_rarity = 0.30f;
    } else if (n <= 6) {
        *rings = 2, *mirrors = true, *splits = false, *taps = 3, *max_rarity = 0.16f;
    } else if (n <= 10) {
        *rings = 3, *mirrors = true, *splits = false, *taps = n <= 8 ? 3 : 4, *max_rarity = 0.10f;
    } else if (n <= 15) {
        *rings = 3, *mirrors = true, *splits = true, *taps = 4, *max_rarity = 0.06f;
    } else if (n <= 30) {
        *rings = 4, *mirrors = true, *splits = true, *taps = n <= 22 ? 5 : 6, *max_rarity = 0.04f;
    } else {
        const int t = 6 + (n - 31) / 10;
        *rings = 4, *mirrors = true, *splits = true, *taps = t < 9 ? t : 9, *max_rarity = 0.02f;
    }
}

// The same level for everybody: the generator is seeded by the level number.
static void make_level(int n, Level *out)
{
    for (int i = 0; i < CAMPAIGN_N; i++)
        if (CAMPAIGN[i].n == n) {
            *out = CAMPAIGN[i].lvl;
            return;
        }
    // Without the solver there is no way to know a generated level is winnable, let alone
    // what its par is, so fall back to handing out the verified levels in turn.
    if (!g.dist || !g.queue) {
        *out = CAMPAIGN[(n - 1) % CAMPAIGN_N].lvl;
        return;
    }

    int rings, taps;
    bool mirrors, splits;
    float max_rarity;
    tier(n, &rings, &mirrors, &splits, &taps, &max_rarity);
    Rng rng = {(uint32_t)n * 2654435761u + 12345u};
    const int64_t t0 = T->now_us();
    Level best_try;
    memset(&best_try, 0, sizeof(best_try));
    int best_gap = 99;
    for (int attempt = 0; attempt < 400; attempt++) {
        Level L;
        memset(&L, 0, sizeof(L));
        L.n_rings = (uint8_t)rings;
        for (int i = 0; i < rings; i++) {
            const int open = 3 + rng_below(&rng, 2);   /* 3..4 holes per ring */
            bool split_used = false;
            for (int k = 0; k < open; k++) {
                int s;
                do s = rng_below(&rng, 8);
                while (L.ring[i][s] != BLOCK);
                uint8_t el = GAP;
                const int pick = rng_below(&rng, 10);
                if (mirrors && pick >= 4) el = pick & 1 ? MIRROR_L : MIRROR_R;
                if (splits && !split_used && pick == 9) el = SPLIT, split_used = true;
                L.ring[i][s] = el;
            }
        }
        const int winners = solve(&L);
        int total = 1;
        for (int i = 0; i < rings; i++) total *= 8;
        const float rarity = (float)winners / total;
        // After a while, loosen the rarity ceiling rather than give up on the tap target.
        const float ceiling = max_rarity * (attempt < 200 ? 1.0f : 2.0f);
        if (winners == 0 || rarity > ceiling) continue;
        // Any opening at exactly `taps` from a win will do; pick one at random.
        int count = 0;
        for (int v = 0; v < total; v++) count += g.dist[v] == taps;
        if (count == 0) {
            // Remember the nearest miss in case nothing better turns up.
            for (int d = taps - 1; d >= 1 && best_gap > taps - d; d--)
                for (int v = 0; v < total; v++)
                    if (g.dist[v] == d) {
                        best_try = L;
                        unpack(v, best_try.start, rings);
                        best_try.best = (uint8_t)d;
                        best_gap = taps - d;
                        break;
                    }
            continue;
        }
        int pick = rng_below(&rng, count);
        for (int v = 0; v < total; v++)
            if (g.dist[v] == taps && pick-- == 0) {
                unpack(v, L.start, rings);
                break;
            }
        L.best = (uint8_t)taps;
        *out = L;
        T->log("level %d: %d rings, %d taps, rarity %d/%d, %d tries, %lld ms", n, rings, taps, winners, total,
               attempt + 1, (long long)((T->now_us() - t0) / 1000));
        return;
    }
    T->log("level %d: settled for %d taps (wanted %d)", n, best_try.best, taps);
    *out = best_gap < 99 ? best_try : CAMPAIGN[5].lvl;
}

// Acceptance tests from the spec (section 14), run once at start-up and logged.
static void self_test(void)
{
    if (!g.dist || !g.queue) return;
    int ok = 0;
    for (int i = 0; i < CAMPAIGN_N; i++) {
        const Level *L = &CAMPAIGN[i].lvl;
        solve(L);
        const int d = g.dist[pack(L->start, L->n_rings)];
        const bool opens_locked = !wins(L, L->start, 0xFF);
        if (d == L->best && opens_locked) ok++;
        else T->log("level %d fails verification: min taps %d (spec %d), locked at start %d", CAMPAIGN[i].n, d, L->best,
                    opens_locked);
    }
    T->log("campaign verified: %d/%d levels match their best tap count", ok, CAMPAIGN_N);
}

// ---------------------------------------------------------------- sound

static void tone1(float f0, float f1, uint16_t ms, uint8_t wave, float vol, uint16_t delay)
{
    const tat_tone_t t = {f0, f1, ms, wave, vol, delay};
    T->tone(&t);
}

static void sfx_click(void) { tone1(800, 500, 40, TAT_SQUARE, 0.5f, 0); }
static void sfx_detent(void) { tone1(1200, 0, 25, TAT_SQUARE, 0.22f, 0); }

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

// A point on the board: r in the spec's units, turned and scaled onto the canvas.
static float zoom(void);
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
    // The API stores ints, so the two flags travel as 0 or 1.
    int seen = g.seen_tut, flat = g.flat_play;
    T->save_get("seen_tut", &seen, 0);
    T->save_get("flat", &flat, 0);
    g.seen_tut = seen != 0;
    g.flat_play = flat != 0;
}

static void save_progress(void)
{
    T->save_set("depth", g.depth);
    T->save_set("level", g.level);
    T->save_set("seen_tut", g.seen_tut);
    T->save_set("flat", g.flat_play);
}

static void save_best(int n, int used)
{
    char key[16];
    snprintf(key, sizeof(key), "best_%d", n);
    int prev = 255;
    T->save_get(key, &prev, 0);
    if (used < prev) T->save_set(key, used);
}

// ---------------------------------------------------------------- level flow

// How much bigger than the spec the board is drawn, for this level's number of rings.
static float zoom(void) { return BOARD_PX / (GEO[g.lvl.n_rings < 2 ? 2 : g.lvl.n_rings].rad[0] + TEETH_OUT); }

// Beam walk with geometry for drawing (spec section 4.2).
static void retrace(bool sounds)
{
    const Geo *G = &GEO[g.lvl.n_rings];
    g.n_segs = g.n_stops = 0;
    Branch cur[8], nxt[8];
    int n_cur = 1, deepest = 0;
    cur[0].ring = 0;
    cur[0].from_r = EMIT_PX / zoom();
    cur[0].from_s = g.entry;
    cur[0].meet_s = g.entry;
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
                    const Pt e = polar(G->star, (float)s);
                    if (g.n_segs < 32) {
                        const Seg seg = {p1.x, p1.y, e.x, e.y};
                        g.segs[g.n_segs++] = seg;
                    }
                    if (s == DOOR) {
                        win = true;
                        if (g.n_segs < 32) {
                            const Seg seg = {e.x, e.y, C, C};
                            g.segs[g.n_segs++] = seg;
                        }
                    } else if (g.n_stops < 12) {
                        g.stops[g.n_stops++] = e;
                    }
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
    if (win && !g.won && g.phase == PLAYING) {
        g.phase = SOLVED;
        g.phase_t = 0;
        g.stars_earned = g.taps <= g.lvl.best ? 3 : g.taps <= g.lvl.best + 2 ? 2 : 1;
        if (g.hinted && g.stars_earned > 2) g.stars_earned = 2;   /* help is free, but it is not perfect */
        g.hint_t = 0;
        save_best(g.level, g.taps);
        if (g.level + 1 > g.depth) g.depth = g.level + 1;
        save_progress();
        sfx_solve();
    }
    g.won = win;
    g.dirty = true;
}

static void start_level(int n)
{
    g.level = n < 1 ? 1 : n;
    make_level(g.level, &g.lvl);
    memcpy(g.rot, g.lvl.start, sizeof(g.rot));
    for (int i = 0; i < MAX_RINGS; i++) g.vis_from[i] = g.vis_to[i] = g.rot[i];
    g.tween_t = 1;
    g.taps = 0;
    g.reached = 0;
    g.hint_t = g.idle_t = 0;
    g.hint_ring = g.hint_entry = -1;
    g.hinted = false;
    solve(&g.lvl);   /* the table the hints are read from: taps to go, from every arrangement */
    g.phase = (g.level == 1 && !g.seen_tut) ? TUTORIAL : PLAYING;
    g.phase_t = 0;
    retrace(false);
    g.dirty = true;
    save_progress();
}

// What the star says when it is asked. The solver's table has, for every arrangement of the
// rings, how many taps it is from one the light gets through - allowing the laser to come
// from anywhere, since turning the watch is free. So if this arrangement is already zero
// taps away, the hint is where to turn the laser to; otherwise it is whichever ring's tap
// leads to an arrangement one tap nearer.
static void give_hint(void)
{
    g.hint_ring = g.hint_entry = -1;
    const int here = pack(g.rot, g.lvl.n_rings);
    if (g.dist[here] == 0) {
        for (int e = 0; e < SPOKES; e++)
            if (wins(&g.lvl, g.rot, (uint8_t)(1u << e))) {
                g.hint_entry = e;
                break;
            }
    } else {
        for (int i = 0; i < g.lvl.n_rings; i++) {
            uint8_t next[MAX_RINGS];
            memcpy(next, g.rot, sizeof(next));
            next[i] = (uint8_t)m8(next[i] + 1);
            if (i < g.lvl.n_rings - 1) next[i + 1] = (uint8_t)m8(next[i + 1] - 1);
            if (g.dist[pack(next, g.lvl.n_rings)] == g.dist[here] - 1) {
                g.hint_ring = i;
                break;
            }
        }
    }
    g.hint_t = HINT_S;
    g.hinted = true;
    g.idle_t = 0;
    g.dirty = true;
    tone1(660, 0, 60, TAT_TRIANGLE, 0.4f, 0);
    tone1(990, 0, 90, TAT_TRIANGLE, 0.4f, 70);
    T->log("hint on level %d: %s %d", g.level, g.hint_ring >= 0 ? "tap ring" : "laser to spoke",
           g.hint_ring >= 0 ? g.hint_ring : g.hint_entry);
}

static void tap_ring(int idx)
{
    for (int i = 0; i < MAX_RINGS; i++) g.vis_from[i] = g.vis_to[i];
    g.rot[idx] = (uint8_t)m8(g.rot[idx] + 1);
    g.vis_to[idx] += 1;
    if (idx < g.lvl.n_rings - 1) {
        g.rot[idx + 1] = (uint8_t)m8(g.rot[idx + 1] - 1);   /* the ring inside turns the other way */
        g.vis_to[idx + 1] -= 1;
    }
    g.tween_t = 0;
    g.taps++;
    g.hint_t = 0;   /* whatever was suggested, something has been done about it */
    sfx_click();
    retrace(true);
}

// ---------------------------------------------------------------- tilt -> entry spoke

static float wrap_pi(float a)
{
    while (a > PI) a -= TAU;
    while (a < -PI) a += TAU;
    return a;
}

static float minf(float a, float b) { return a < b ? a : b; }

static void update_tilt(const tat_input_t *in, float dt)
{
    const float ax = in->tilt.ax, ay = in->tilt.ay;
    const float g_mag = sqrtf(ax * ax + ay * ay);
    const float gz = in->tilt.gz * (PI / 180.0f) * dt;
    if (g_mag >= 0.25f) {
        const float target = atan2f(-ay, -ax);   /* screen direction of world-up */
        if (!g.have_up) {
            g.up = target;
            g.have_up = true;
        }
        // Learn the gyro's sign from gravity, for flat play later.
        if (g.had_target && fabsf(gz) > 0.004f) {
            g.sign_score += wrap_pi(target - g.last_target) * (-g.gyro_sign * gz) * 400;
            if (g.sign_score < -1.0f) {
                g.gyro_sign = -g.gyro_sign;
                g.sign_score = 0;
            }
            g.sign_score = minf(g.sign_score, 3.0f);
        }
        g.last_target = target;
        g.had_target = true;
        g.up = wrap_pi(g.up + wrap_pi(target - g.up) * minf(1.0f, dt / 0.12f));
        g.flat_t = 0;
    } else {
        g.had_target = false;
        g.flat_t += dt;
        // Lying flat there's no in-plane gravity. Hold the last angle, or follow the gyro if
        // FLAT PLAY is on (it drifts, which is why it's a setting).
        if (g.flat_play) g.up = wrap_pi(g.up - g.gyro_sign * gz);
    }
    const bool want_nudge = g.flat_t > 1.0f && !g.flat_play;
    if (want_nudge != g.nudge) {
        g.nudge = want_nudge;
        g.dirty = true;
    }

    // Snap to a spoke with 6 degrees of hysteresis so it can't chatter on a boundary.
    const float deg = g.up * 180.0f / PI + 90.0f;
    const float centre = m8(g.entry + VIEW) * 45.0f;   /* where that spoke is drawn */
    const float off = fmodf(deg - centre + 540.0f, 360.0f) - 180.0f;
    if (fabsf(off) > 22.5f + 6.0f) {
        g.entry = m8((int)lroundf(deg / 45.0f) - VIEW);
        if (g.entry == g.hint_entry) g.hint_t = 0;   /* that is where it was asked to go */
        sfx_detent();
        retrace(true);
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
        case 0:
            g.won = false;
            start_level(g.level >= g.depth ? 1 : g.level + 1);   /* step through the levels reached so far */
            break;
        case 1: T->menu_toggle_sound(); break;
        case 2:
            g.flat_play = !g.flat_play;
            save_progress();
            break;
        case 3:
            g.won = false;
            start_level(g.level);
            T->menu_close();
            break;
        }
        if (!T->menu_is_open()) g.dirty = true;   /* the board is showing again */
        return;
    }
    if (ges->swipe_left) {
        T->menu_open();
        return;
    }
    if (g.tween_t < 1) {
        g.tween_t = minf(1.0f, g.tween_t + dt / 0.26f);
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
    update_tilt(in, dt);

    if (g.hint_t > 0) {
        g.hint_t -= dt;
        g.dirty = true;   /* it pulses */
    }
    if (g.phase == PLAYING) {
        // Nothing getting any deeper for a while: the star lets it be known that it can help.
        g.idle_t += dt;
        if (g.idle_t > STUCK_S && g.hint_t <= 0) {
            g.blink_t += dt;
            g.dirty = true;
        }
    }

    if (g.phase == SOLVED) {
        if ((ges->tap && g.phase_t > 0.8f) || g.phase_t > 4.0f) {
            g.won = false;
            start_level(g.level + 1);
        }
        return;
    }
    if (in->clicked & TAT_BTN_B) {   /* PWR: start the level over */
        g.won = false;
        start_level(g.level);
        return;
    }
    if (ges->tap) {
        const int64_t now = T->now_us();
        if (now - g.last_tap_us < 120000) return;   /* debounce */
        g.last_tap_us = now;
        // The star is the hint button; the rings are the rings. Radii in the spec's units.
        const Geo *G = &GEO[g.lvl.n_rings];
        const float r = hypotf(ges->x - 233.0f, ges->y - 233.0f) / SCALE / zoom();
        if (r < G->star + 3) {
            give_hint();
            return;
        }
        if (r > G->rad[0] + 10) return;   /* the rim isn't a button */
        int hit = -1;
        float best = 8.0f;   /* 16 device px either side of the band's centre */
        for (int i = 0; i < g.lvl.n_rings; i++)
            if (fabsf(r - G->rad[i]) <= best) {
                best = fabsf(r - G->rad[i]);
                hit = i;
            }
        if (hit >= 0) tap_ring(hit);
    }
}

// ---------------------------------------------------------------- drawing

static float vis_rot(int i)
{
    // Mechanical, not smooth: three discrete jumps over the tween.
    const float step = minf(1.0f, floorf(g.tween_t * 3.0f + 1.0f) / 3.0f);
    return g.vis_from[i] + (g.vis_to[i] - g.vis_from[i]) * (g.tween_t >= 1 ? 1.0f : step);
}

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

static void octagon(float R, uint8_t col)
{
    const int ri = (int)ceilf(R);
    for (int dy = -ri; dy <= ri; dy++)
        for (int dx = -ri; dx <= ri; dx++)
            if (abs(dx) <= R && abs(dy) <= R && abs(dx) + abs(dy) <= R * 1.414f)
                T->canvas_pixel(g.cv, (int)C + dx, (int)C + dy, col);
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

static void draw_star(void)
{
    const Geo *G = &GEO[g.lvl.n_rings];
    const bool awake = g.won;
    const float k = zoom(), R = G->star * k;
    // Offering a hint, the star stirs: it brightens and dims.
    const bool stir = !awake && g.phase == PLAYING && g.idle_t > STUCK_S && g.hint_t <= 0 && ((int)(g.blink_t * 2.0f) & 1);
    if (awake) octagon(R + 9 * k, g.c_halo);
    octagon(R, awake ? g.c_awake : stir ? g.c_hint : g.c_asleep);
    /* The door: a bright notch on the star's edge, at board spoke 4 - which VIEW puts at the
       top, under the laser. */
    const Pt d = polar(G->star, DOOR);
    const int dw = (int)(6 * k), dh = (int)(4 * k);
    T->canvas_fill_rect(g.cv, (int)d.x - dw, (int)d.y - dh, dw * 2 + 1, dh * 2, awake ? g.c_door_lit : g.c_door);
    /* Face: shut eyes and a small mouth asleep; open eyes and a smile awake. */
    const int u0 = (int)lroundf(R / 7);
    const int u = u0 < 1 ? 1 : u0;
    const int cx = (int)C, cy = (int)C;
    const uint8_t f = awake ? g.c_face_lit : g.c_disc;
    if (awake) {
        T->canvas_fill_rect(g.cv, cx - 4 * u, cy - 2 * u, 2 * u, 2 * u, f);
        T->canvas_fill_rect(g.cv, cx + 2 * u, cy - 2 * u, 2 * u, 2 * u, f);
        T->canvas_fill_rect(g.cv, cx - 3 * u, cy + u, 6 * u, u, f);
        T->canvas_fill_rect(g.cv, cx - 4 * u, cy, u, u, f);
        T->canvas_fill_rect(g.cv, cx + 3 * u, cy, u, u, f);
        for (int s = 0; s < 8; s++) {
            const Pt p = polar(G->star + 7, (float)s);
            T->canvas_fill_rect(g.cv, (int)p.x - 2, (int)p.y - 2, 5, 5, g.c_rays);
        }
    } else {
        T->canvas_fill_rect(g.cv, cx - 4 * u, cy - u, 2 * u, u, f);
        T->canvas_fill_rect(g.cv, cx + 2 * u, cy - u, 2 * u, u, f);
        T->canvas_fill_rect(g.cv, cx - u, cy + 2 * u, 2 * u, u, f);
    }
}

static void banner(const char *top, const char *mid, const char *bottom, uint8_t col)
{
    const tat_banner_t b = {
        .top = top, .mid = mid, .bottom = bottom,
        .top_color = col, .mid_color = g.c_text, .bottom_color = g.c_awake,
        .panel = g.c_panel, .border = col,
        .top_scale = 2, .bars = true, .bottom_bold = true,
    };
    T->canvas_banner(g.cv, (int)C, 96, CW - 40, &b);
}

static void draw_menu(void)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%d / %d", g.level, g.depth);
    const tat_menu_row_t rows[] = {
        {"LEVEL", buf, 0},
        T->menu_sound_row(),
        {"FLAT PLAY", g.flat_play ? "GYRO" : "OFF", T->ui_color(g.flat_play ? TAT_UI_VALUE : TAT_UI_DIM)},
        {"RESET LEVEL", "GO", T->ui_color(TAT_UI_ACCENT)},
    };
    T->menu_draw(rows, 4, "PAUSED");
}

static void star_draw(void)
{
    if (!g.cv) return;
    if (T->menu_is_open()) {
        draw_menu();
        return;
    }
    // Turn-based: nothing moves unless something changed, so don't redraw (or send a frame).
    if (!g.dirty) return;
    g.dirty = false;

    draw_board();
    draw_star();
    for (int i = 0; i < g.n_segs; i++) thick_line(&g.segs[i], 5, g.c_glow);
    for (int i = 0; i < g.n_segs; i++) thick_line(&g.segs[i], 3, g.c_beam);
    for (int i = 0; i < g.n_segs; i++) thick_line(&g.segs[i], 1, g.c_white);
    for (int i = 0; i < g.n_stops; i++) {
        T->canvas_fill_rect(g.cv, (int)g.stops[i].x - 2, (int)g.stops[i].y - 2, 5, 5, g.c_stop);
        T->canvas_fill_rect(g.cv, (int)g.stops[i].x - 7, (int)g.stops[i].y - 1, 3, 3, g.c_stop);
        T->canvas_fill_rect(g.cv, (int)g.stops[i].x + 5, (int)g.stops[i].y - 1, 3, 3, g.c_stop);
    }
    /* Where the star would like the laser, if that is the hint: a marker on the rim. */
    if (g.hint_t > 0 && g.hint_entry >= 0 && ((int)(g.hint_t * 5.0f) & 1) == 0) {
        const float ha = spoke_angle((float)g.hint_entry);
        spoke_rect(ha, 104, 116, 9.0f, g.c_hint);
        spoke_rect(ha, 100, 104, 4.0f, g.c_hint);
    }
    /* The emitter sits on the rim wherever world-up is, pointing in. */
    const float ea = spoke_angle((float)g.entry);
    spoke_rect(ea, 105, 116, 8.0f, g.c_emit);
    spoke_rect(ea, 109, 113, 5.5f, g.c_emit_hi);
    spoke_rect(ea, EMIT_PX, 105, 3.8f, g.c_beam);
    spoke_rect(ea, EMIT_PX - 3, EMIT_PX, 2.0f, g.c_white);

    // HUD: one line, in the sliver of screen under the board. Level, and taps against the
    // best there is. While a hint is up it says what the hint means instead.
    char buf[32];
    const char *line = buf;
    uint8_t line_col = g.taps > g.lvl.best ? g.c_orange : g.c_dim;
    snprintf(buf, sizeof(buf), "L%d  %d/%d", g.level, g.taps, g.lvl.best);
    if (g.nudge) line = "TILT ME", line_col = g.c_cyan;
    else if (g.hint_t > 0) line = g.hint_ring >= 0 ? "TAP THIS RING" : "TURN THE LASER", line_col = g.c_hint;
    else if (g.phase == PLAYING && g.idle_t > STUCK_S) line = "STUCK? TAP STAR", line_col = g.c_hint;
    T->canvas_fill_rect(g.cv, (int)C - 47, 216, 95, 11, g.c_void);
    T->canvas_text_centered(g.cv, (int)C, 221, line, line_col, 1, true);

    if (g.phase == TUTORIAL) {
        T->canvas_fill_rect(g.cv, 22, 46, CW - 44, 142, g.c_panel);
        T->canvas_rect(g.cv, 22, 46, CW - 44, 142, g.c_beam);
        T->canvas_rect(g.cv, 23, 47, CW - 46, 140, g.c_beam);
        T->canvas_text_centered(g.cv, (int)C, 60, "WAKE THE STAR", g.c_beam, 2, true);
        T->canvas_text_centered(g.cv, (int)C, 84, "GET THE LIGHT IN ITS DOOR", g.c_text, 1, true);
        T->canvas_text(g.cv, 34, 102, "TURN", g.c_cyan, 1, true);
        T->canvas_text(g.cv, 70, 102, "YOUR WRIST: THE LASER", g.c_text, 1, false);
        T->canvas_text(g.cv, 70, 112, "MOVES ROUND THE RIM", g.c_text, 1, false);
        T->canvas_text(g.cv, 34, 128, "TAP", g.c_beam, 1, true);
        T->canvas_text(g.cv, 70, 128, "A RING TO TURN IT. THE", g.c_text, 1, false);
        T->canvas_text(g.cv, 70, 138, "ONE INSIDE TURNS BACK", g.c_text, 1, false);
        T->canvas_text(g.cv, 34, 154, "STUCK?", g.c_hint, 1, true);
        T->canvas_text(g.cv, 82, 154, "TAP THE STAR", g.c_text, 1, false);
        T->canvas_text_centered(g.cv, (int)C, 175, "TAP TO START", g.c_beam, 1, true);
    } else if (g.phase == SOLVED && g.phase_t > 0.25f) {
        char mid[64], stars[8] = "";
        if (g.taps <= g.lvl.best && !g.hinted) snprintf(mid, sizeof(mid), "PERFECT - %d TAP%s", g.taps, g.taps == 1 ? "" : "S");
        else if (g.taps <= g.lvl.best) snprintf(mid, sizeof(mid), "%d TAP%s, WITH A HINT", g.taps, g.taps == 1 ? "" : "S");
        else snprintf(mid, sizeof(mid), "DONE IN %d - BEST IS %d", g.taps, g.lvl.best);
        for (int i = 0; i < g.stars_earned; i++) strcat(stars, "* ");
        banner(CHEERS[g.level % 6], mid, stars, g.c_awake);
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
    g.up = -PI / 2;
    g.gyro_sign = 1;
    g.entry = m8(-VIEW);   /* the laser starts at the top of the screen, until the tilt says otherwise */
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

    // The solver is only ever run between levels, but it is run often enough while hunting
    // for a board that allocating it per level would be silly. Without it the game still
    // plays - make_level() falls back to the verified campaign.
    g.dist = (int8_t *)T->alloc(MAX_STATES * sizeof(int8_t));
    g.queue = (uint16_t *)T->alloc(MAX_STATES * sizeof(uint16_t));
    if (!g.dist || !g.queue) T->log("no memory for the solver; campaign levels only");

    static const uint32_t PLATE[4] = {0x2E3550, 0x343C5C, 0x3A4368, 0x404A74};
    static const uint32_t TOOTH[4] = {0x414B70, 0x48537C, 0x4F5B88, 0x566394};
    g.c_void = pal(0x0E0E1C);
    g.c_disc = pal(0x16162A);
    g.c_bezel = pal(0x1E2438);
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
    g.c_asleep = pal(0x3C4266);
    g.c_awake = pal(0xFFD93D);
    g.c_halo = pal(0x4A4226);
    g.c_door = pal(0x6E7BA8);
    g.c_door_lit = pal(0xFFF3A0);
    g.c_face_lit = pal(0x8A5A00);
    g.c_text = pal(0xEAF0FF);
    g.c_dim = pal(0x8A97C0);
    g.c_stop = pal(0x6E7BA9);
    g.c_emit = pal(0x3A4260);
    g.c_emit_hi = pal(0x8A97C1);
    g.c_panel = pal(0x07070F);
    g.c_rays = pal(0xFFF3A1);
    g.c_orange = pal(0xFF9A4D);
    g.c_pip_edge = pal(0x3A4261);
    g.c_cyan = pal(0x7FE8FE);
    g.c_hint = pal(0xFFD93D);   /* what the star points at when it is asked */

    self_test();
    load_progress();
    start_level(g.level);
}

static void star_enter(void) { g.dirty = true; }

static void star_redraw(void) { g.dirty = true; }

static void star_unload(void)
{
    T->free(g.dist);
    T->free(g.queue);
    g.dist = NULL;
    g.queue = NULL;
    if (g.cv) T->canvas_destroy(g.cv);
    g.cv = NULL;
}

const tat_game_t tat_game = {
    .magic = TAT_GAME_MAGIC,
    .api_major = TAT_API_MAJOR,
    .api_minor = TAT_API_MINOR,
    // "sleepystar" rather than "star": the id is also the save namespace, and this is
    // where the player's depth, best times and whether they have seen the tutorial have
    // always lived. Renaming it would quietly put everyone back on level 1.
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
