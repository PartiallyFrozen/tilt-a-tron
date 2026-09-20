// RADAR - sink their fleet before they sink yours, on a radar screen that is the whole watch.
//
// The old pencil-and-paper sea battle: two fleets of five ships hidden on two grids, and the
// players take shots at each other's water. What is new is the screen it is played on. The
// grid is a straight 13 x 13 lattice with the corners cut off by the round panel - 137 cells
// of ocean - and a radar sweep goes round it.
//
//   touch       - a pin goes where your finger is, with cross-hairs out to the rim, because
//                 a cell is two millimetres across and the finger is on top of it. Slide to
//                 move the pin
//   let go      - the pin is dropped. The sweep comes round to it, and only when the line
//                 reaches the pin do you learn what was there. A hit drops another
//   swipe left  - pause menu
//
// A hit marks the cell that was struck and nothing else: no hull outline, no ship class. You
// learn what a ship was when it sinks. Then it is their turn, and the same radar runs in
// amber over your own water, so the two sides are never confused.
//
// Placing the fleet: drag a ship to where it should go, tap it (or twist the watch) to turn
// it, and let go. PWR places whatever is left for you.
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

#define N 13              /* the lattice is N x N, indices -HALF..HALF each way */
#define HALF 6
#define PITCH 15          /* canvas pixels between cells */
#define OCEAN_R2 42       /* a cell is water if i*i + j*j is no more than this: 137 cells */
#define CELLS (N * N)

#define R_PLAY 102        /* the sweep and the grid lines reach this far */
#define R_RING 106
#define R_TICK 114

#define SHIPS 5
#define HULL 17           /* cells of ship in a fleet */

#define TAU 6.2831853f
#define SWEEP_S 7.0f      /* one turn of the radar */
#define TRACK_S 2.0f       /* a turn at the speed it comes round to a dropped pin, or to theirs */
#define WEDGE 0.30f       /* of a turn: how long the glow behind the line is */
#define GLOW 8

static const int SHIP_LEN[SHIPS] = {5, 4, 3, 3, 2};
static const char *const SHIP_NAME[SHIPS] = {"CARRIER", "CRUISER", "DESTROYER", "SUBMARINE", "PATROL BOAT"};

enum { TITLE, DEPLOY, READY, HUNT, TRACK, CONTACT, INCOMING, STRUCK, RESULT };
enum { WATER = 0, SHOT_MISS = 1, SHOT_HIT = 2 };

typedef struct {
    int8_t i, j;      // the end nearest the top left
    bool vertical;
    bool placed;
    int hits;
} ship_t;

typedef struct {
    ship_t ship[SHIPS];
    int8_t at[CELLS];      // which ship is on a cell, or -1
    uint8_t shot[CELLS];   // what has been fired at it
    int afloat, hull;
} fleet_t;

// What the static picture is made of. The sweep's glow is only ever put on BG_NONE.
enum { BG_OUT, BG_NONE, BG_GRID, BG_RING, BG_DOT, BG_TICK, BG_MAJOR };

static struct {
    tat_canvas_t *cv;
    uint8_t *bg;        // BG_* per canvas pixel
    uint16_t *ang;      // and its angle, 0..65535 clockwise from east

    uint8_t c_bg, c_grid, c_ring, c_dot, c_tick, c_major;
    uint8_t c_green, c_pale, c_dim, c_white, c_amber, c_red;
    uint8_t c_miss, c_hit_bed, c_cand, c_cand_bed, c_hull_bed, c_sunk_bed, c_amber_bed;
    uint8_t c_glow[2][GLOW];   // [theirs]
    uint8_t bg_color[8];

    fleet_t mine, theirs;
    int phase;
    float phase_t;

    float sweep;          // radians, clockwise from east, the way the tables count
    int shots, hits;
    int armed;            // the cell the pin is on, or -1
    float travel;         // how far the line still has to turn to reach it
    bool aiming;
    bool first_touch;     // the how-to banner goes once a finger has been down

    // placing the fleet
    int hand;             // the ship being placed
    float twist;          // degrees turned since the last flip
    float touch_t;
    int touch_x0, touch_y0;
    bool dragged;

    // a shot being resolved, either way
    int shot_cell;
    int shot_result;      // SHOT_MISS, SHOT_HIT
    int shot_sunk;        // the ship it finished, or -1

    // the other side
    float their_sweep;
    int their_target;
    float their_travel;   // how far their line still has to go
    int8_t ai_queue[CELLS];
    int ai_queued;

    bool won;
    int wins, best, played;
    bool dirty;
} g;

// ---------------------------------------------------------------------------- the lattice

static int cell_of(int i, int j) { return (j + HALF) * N + (i + HALF); }
static int cell_i(int c) { return c % N - HALF; }
static int cell_j(int c) { return c / N - HALF; }
static int cell_x(int c) { return CC + cell_i(c) * PITCH; }
static int cell_y(int c) { return CC + cell_j(c) * PITCH; }
static bool is_ocean(int i, int j)
{
    return i >= -HALF && i <= HALF && j >= -HALF && j <= HALF && i * i + j * j <= OCEAN_R2;
}

static int rnd(int n) { return (int)(T->random() % (uint32_t)n); }

// ---------------------------------------------------------------------------- fleets

static void fleet_clear(fleet_t *f)
{
    memset(f, 0, sizeof(*f));
    memset(f->at, -1, sizeof(f->at));
    f->afloat = SHIPS;
    f->hull = HULL;
}

static bool ship_fits(const fleet_t *f, int s, int i, int j, bool vertical)
{
    for (int k = 0; k < SHIP_LEN[s]; k++) {
        const int ci = i + (vertical ? 0 : k), cj = j + (vertical ? k : 0);
        if (!is_ocean(ci, cj)) return false;
        const int on = f->at[cell_of(ci, cj)];
        if (on >= 0 && on != s) return false;
    }
    return true;
}

static void ship_lift(fleet_t *f, int s)
{
    for (int c = 0; c < CELLS; c++)
        if (f->at[c] == s) f->at[c] = -1;
    f->ship[s].placed = false;
}

static void ship_put(fleet_t *f, int s, int i, int j, bool vertical)
{
    ship_lift(f, s);
    f->ship[s] = (ship_t){(int8_t)i, (int8_t)j, vertical, true, 0};
    for (int k = 0; k < SHIP_LEN[s]; k++) f->at[cell_of(i + (vertical ? 0 : k), j + (vertical ? k : 0))] = (int8_t)s;
}

static void ship_scatter(fleet_t *f, int s)
{
    for (;;) {
        const int i = rnd(N) - HALF, j = rnd(N) - HALF;
        const bool v = rnd(2) != 0;
        if (!ship_fits(f, s, i, j, v)) continue;
        ship_put(f, s, i, j, v);
        return;
    }
}

// Fires on a cell that has not been fired on. Says what happened through g.shot_*.
static void fire(fleet_t *f, int c)
{
    const int s = f->at[c];
    g.shot_cell = c;
    g.shot_sunk = -1;
    if (s < 0) {
        f->shot[c] = SHOT_MISS;
        g.shot_result = SHOT_MISS;
        return;
    }
    f->shot[c] = SHOT_HIT;
    g.shot_result = SHOT_HIT;
    f->hull--;
    if (++f->ship[s].hits == SHIP_LEN[s]) {
        f->afloat--;
        g.shot_sunk = s;
    }
}

// ---------------------------------------------------------------------------- sound

static void tone(float f0, float f1, int ms, int wave, float vol, int delay)
{
    const tat_tone_t t = {f0, f1, (uint16_t)ms, (uint8_t)wave, vol, (uint16_t)delay};
    T->tone(&t);
}

static void sfx_ping(void) { tone(1320, 1280, 70, TAT_TRIANGLE, 0.25f, 0); }
static void sfx_fire(void) { tone(520, 180, 120, TAT_SQUARE, 0.4f, 0); }
static void sfx_miss(void) { tone(300, 120, 260, TAT_TRIANGLE, 0.5f, 120); }
static void sfx_hit(void)
{
    tone(200, 60, 320, TAT_NOISE, 0.8f, 100);
    tone(880, 1320, 140, TAT_SQUARE, 0.5f, 120);
}
static void sfx_sunk(void)
{
    tone(180, 40, 600, TAT_NOISE, 0.9f, 100);
    tone(660, 660, 110, TAT_SQUARE, 0.5f, 150);
    tone(880, 880, 110, TAT_SQUARE, 0.5f, 280);
    tone(1320, 1320, 220, TAT_SQUARE, 0.5f, 410);
}
static void sfx_struck(void) { tone(160, 50, 420, TAT_NOISE, 0.9f, 0); }
static void sfx_place(void) { tone(440, 660, 80, TAT_TRIANGLE, 0.5f, 0); }
static void sfx_turn(void) { tone(700, 900, 50, TAT_TRIANGLE, 0.4f, 0); }
static void sfx_no(void) { tone(200, 150, 140, TAT_SQUARE, 0.4f, 0); }
static void sfx_end(bool won)
{
    const float up[4] = {523, 659, 784, 1047}, down[4] = {392, 330, 262, 196};
    for (int k = 0; k < 4; k++) tone(won ? up[k] : down[k], 0, k == 3 ? 360 : 140, TAT_SQUARE, 0.55f, 500 + k * 150);
}

// ---------------------------------------------------------------------------- phases

static void set_phase(int p)
{
    g.phase = p;
    g.phase_t = 0;
    g.dirty = true;
}

static void begin_deploy(void)
{
    fleet_clear(&g.mine);
    fleet_clear(&g.theirs);
    for (int s = 0; s < SHIPS; s++) ship_scatter(&g.theirs, s);
    g.hand = 0;
    g.twist = 0;
    g.dragged = false;
    g.mine.ship[0] = (ship_t){-2, 0, false, false, 0};   // in the hand: shown, not yet put down
    g.shots = g.hits = 0;
    g.sweep = -TAU / 4;
    g.their_sweep = TAU / 4;
    g.armed = -1;
    g.aiming = false;
    g.first_touch = false;
    g.ai_queued = 0;
    set_phase(DEPLOY);
}

static void next_in_hand(void)
{
    while (g.hand < SHIPS && g.mine.ship[g.hand].placed) g.hand++;
    if (g.hand >= SHIPS) {
        set_phase(READY);
        return;
    }
    // Somewhere it fits, near the middle, for the player to move.
    for (int tries = 0;; tries++) {
        const int span = tries < 40 ? 3 : HALF;
        const int i = rnd(2 * span + 1) - span, j = rnd(2 * span + 1) - span;
        if (!ship_fits(&g.mine, g.hand, i, j, false)) continue;
        g.mine.ship[g.hand] = (ship_t){(int8_t)i, (int8_t)j, false, false, 0};
        break;
    }
    g.dirty = true;
}

static void finish_game(bool won)
{
    g.won = won;
    g.played++;
    T->save_set("played", g.played);
    if (won) {
        g.wins++;
        T->save_set("wins", g.wins);
        if (g.best == 0 || g.shots < g.best) {
            g.best = g.shots;
            T->save_set("best", g.best);
        }
    }
    T->log("battle over: %s, %d shots, %d hits", won ? "won" : "lost", g.shots, g.hits);
    sfx_end(won);
    set_phase(RESULT);
}

// ---------------------------------------------------------------------------- their side

static void ai_enqueue(int i, int j)
{
    if (!is_ocean(i, j)) return;
    const int c = cell_of(i, j);
    if (g.mine.shot[c] != WATER) return;
    for (int k = 0; k < g.ai_queued; k++)
        if (g.ai_queue[k] == c) return;
    g.ai_queue[g.ai_queued++] = (int8_t)c;
}

// After a hit they work outward from it, along the line of any hits they already have; with
// nothing to go on they fire at every other cell, which no ship can hide between.
static void ai_learn(void)
{
    if (g.shot_result != SHOT_HIT) return;
    if (g.shot_sunk >= 0) {
        // That one is dealt with. Anything still wounded keeps its leads.
        g.ai_queued = 0;
        for (int c = 0; c < CELLS; c++) {
            if (g.mine.shot[c] != SHOT_HIT) continue;
            const int s = g.mine.at[c];
            if (g.mine.ship[s].hits == SHIP_LEN[s]) continue;
            ai_enqueue(cell_i(c) + 1, cell_j(c)), ai_enqueue(cell_i(c) - 1, cell_j(c));
            ai_enqueue(cell_i(c), cell_j(c) + 1), ai_enqueue(cell_i(c), cell_j(c) - 1);
        }
        return;
    }
    const int i = cell_i(g.shot_cell), j = cell_j(g.shot_cell);
    // Two hits in a row say which way the ship lies: those ends go to the front.
    const int before = g.ai_queued;
    ai_enqueue(i + 1, j), ai_enqueue(i - 1, j), ai_enqueue(i, j + 1), ai_enqueue(i, j - 1);
    for (int k = before; k < g.ai_queued; k++) {
        const int c = g.ai_queue[k], di = cell_i(c) - i, dj = cell_j(c) - j;
        const int bi = i - di, bj = j - dj;   // the cell on the other side of the hit
        if (!is_ocean(bi, bj) || g.mine.shot[cell_of(bi, bj)] != SHOT_HIT) continue;
        const int8_t lead = g.ai_queue[k];
        memmove(g.ai_queue + 1, g.ai_queue, (size_t)k);
        g.ai_queue[0] = lead;
    }
}

static int ai_pick(void)
{
    while (g.ai_queued > 0) {
        const int c = g.ai_queue[0];
        memmove(g.ai_queue, g.ai_queue + 1, (size_t)--g.ai_queued);
        if (g.mine.shot[c] == WATER) return c;
    }
    int pool[CELLS], n = 0;
    for (int pass = 0; pass < 2 && n == 0; pass++)
        for (int c = 0; c < CELLS; c++) {
            const int i = cell_i(c), j = cell_j(c);
            if (!is_ocean(i, j) || g.mine.shot[c] != WATER) continue;
            if (pass == 0 && ((i + j) & 1)) continue;
            pool[n++] = c;
        }
    return n ? pool[rnd(n)] : -1;
}

static float bearing_of(int c) { return atan2f((float)cell_j(c), (float)cell_i(c)); }

// How far a line at `from` turns to reach a cell: the long way round if the cell is close, so
// that the line is always seen coming.
static float turn_to(float from, int c)
{
    float turn = fmodf(bearing_of(c) - from, TAU);
    if (turn < 0) turn += TAU;
    if (turn < TAU * 0.3f) turn += TAU;
    return turn;
}

static void begin_incoming(void)
{
    g.their_target = ai_pick();
    if (g.their_target < 0) {
        set_phase(HUNT);
        return;
    }
    g.their_travel = turn_to(g.their_sweep, g.their_target);
    set_phase(INCOMING);
}

// ---------------------------------------------------------------------------- hunting

// The cell of their water nearest a touch that has not been fired on, or -1 off the board.
static int cell_near(int tx, int ty)
{
    const float x = (float)tx / SCALE - CC, y = (float)ty / SCALE - CC;
    int best = -1;
    float best_d = 1e9f;
    for (int c = 0; c < CELLS; c++) {
        const int i = cell_i(c), j = cell_j(c);
        if (!is_ocean(i, j) || g.theirs.shot[c] != WATER) continue;
        const float dx = x - i * PITCH, dy = y - j * PITCH, d = dx * dx + dy * dy;
        if (d < best_d) best_d = d, best = c;
    }
    return best_d <= (PITCH * 1.5f) * (PITCH * 1.5f) ? best : -1;
}

static void update_hunt(float dt, const tat_input_t *in)
{
    g.sweep = fmodf(g.sweep + TAU / SWEEP_S * dt, TAU);
    if (in->touch.down) {
        g.first_touch = true;
        const int was = g.armed;
        g.armed = cell_near(in->touch.x, in->touch.y);
        if (g.armed != was && g.armed >= 0) sfx_ping();
        g.aiming = true;
    } else if (g.aiming) {
        g.aiming = false;
        if (g.armed < 0) return;   // let go off the board: nothing dropped
        g.shots++;
        sfx_fire();
        g.travel = turn_to(g.sweep, g.armed);
        set_phase(TRACK);
    }
}

// The pin is down and the line is on its way. What was there is found out when it arrives.
static void update_track(float dt)
{
    const float step = TAU / TRACK_S * dt;
    g.sweep = fmodf(g.sweep + step, TAU);
    g.travel -= step;
    if (g.travel > 0) return;
    const int c = g.armed;
    g.armed = -1;
    g.sweep = fmodf(bearing_of(c) + TAU, TAU);
    fire(&g.theirs, c);
    if (g.shot_result == SHOT_HIT) g.hits++;
    if (g.shot_sunk >= 0) sfx_sunk();
    else if (g.shot_result == SHOT_HIT) sfx_hit();
    else sfx_miss();
    set_phase(CONTACT);
}

// ---------------------------------------------------------------------------- placing

// Whether the ship in the hand is somewhere it could stay. It is drawn wherever it is
// dragged, water or not, so that it follows the finger; only putting it down is refused.
static bool hand_ok(void)
{
    const ship_t *h = &g.mine.ship[g.hand];
    return ship_fits(&g.mine, g.hand, h->i, h->j, h->vertical);
}

// The hand's ship is kept out of `at` until it is put down, so that it can be dragged over
// the others.
static void hand_set(int i, int j, bool vertical)
{
    ship_t *h = &g.mine.ship[g.hand];
    const int len = SHIP_LEN[g.hand];
    const int max_i = HALF - (vertical ? 0 : len - 1), max_j = HALF - (vertical ? len - 1 : 0);
    if (i < -HALF) i = -HALF;
    if (j < -HALF) j = -HALF;
    if (i > max_i) i = max_i;
    if (j > max_j) j = max_j;
    if (h->i == i && h->j == j && h->vertical == vertical) return;
    h->i = (int8_t)i, h->j = (int8_t)j, h->vertical = vertical;
    g.dirty = true;
}

static void hand_turn(void)
{
    ship_t *h = &g.mine.ship[g.hand];
    // About its middle, so that it turns where it is and not about one end.
    const int mid = SHIP_LEN[g.hand] / 2;
    if (h->vertical) hand_set(h->i - mid, h->j + mid, false);
    else hand_set(h->i + mid, h->j - mid, true);
    sfx_turn();
}

static void hand_commit(void)
{
    ship_t *h = &g.mine.ship[g.hand];
    if (!hand_ok()) {
        sfx_no();
        return;
    }
    ship_put(&g.mine, g.hand, h->i, h->j, h->vertical);
    sfx_place();
    next_in_hand();
}

static void update_deploy(float dt, const tat_input_t *in)
{
    if (in->clicked & TAT_BTN_B) {
        // The rest, anywhere. The ship in the hand goes down where it is if it can.
        if (hand_ok()) hand_commit();
        for (int s = 0; s < SHIPS; s++)
            if (!g.mine.ship[s].placed) ship_scatter(&g.mine, s);
        g.hand = SHIPS;
        sfx_place();
        set_phase(READY);
        return;
    }

    // A twist of the watch turns the ship: a quarter turn's worth of intent is enough.
    g.twist += in->tilt.gz * dt;
    g.twist *= 1.0f - 1.5f * dt;   // and it has to be a twist, not a slow drift
    if (fabsf(g.twist) > 28.0f) {
        g.twist = 0;
        hand_turn();
    }

    const ship_t *h = &g.mine.ship[g.hand];
    if (in->touch.pressed) {
        g.touch_t = 0;
        g.touch_x0 = in->touch.x, g.touch_y0 = in->touch.y;
        g.dragged = false;
    }
    if (in->touch.down) {
        g.touch_t += dt;
        const int mx = in->touch.x - g.touch_x0, my = in->touch.y - g.touch_y0;
        if (mx * mx + my * my > 18 * 18) g.dragged = true;
        if (g.dragged) {
            // The ship rides above the finger, where it can be seen, centred on it.
            const float fx = (float)in->touch.x / SCALE - CC, fy = (float)in->touch.y / SCALE - CC - 16;
            const float half = (SHIP_LEN[g.hand] - 1) * 0.5f;
            const int i = (int)lroundf(fx / PITCH - (h->vertical ? 0 : half));
            const int j = (int)lroundf(fy / PITCH - (h->vertical ? half : 0));
            hand_set(i, j, h->vertical);
        }
    }
    if (in->touch.released) {
        if (g.dragged) hand_commit();
        else if (g.touch_t < 0.4f) hand_turn();
    }
}

// ---------------------------------------------------------------------------- update

static void rd_update(float dt)
{
    if (!g.cv || !g.bg || !g.ang) return;
    if (dt > 1.0f / 20) dt = 1.0f / 20;

    const tat_input_t *in = T->input();
    const tat_gestures_t *ges = T->gestures();

    if (T->menu_is_open()) {
        switch (T->menu_update()) {
        case 0: T->menu_toggle_sound(); break;
        case 1:
            begin_deploy();
            T->menu_close();
            break;
        }
        if (!T->menu_is_open()) g.dirty = true;
        return;
    }
    if (ges->swipe_left && g.phase != DEPLOY) {
        g.aiming = false;   // the swipe was not a shot
        g.armed = -1;
        T->menu_open();
        return;
    }

    g.phase_t += dt;
    switch (g.phase) {
    case TITLE:
        g.sweep = fmodf(g.sweep + TAU / SWEEP_S * dt, TAU);
        if (ges->tap) begin_deploy();
        break;

    case DEPLOY:
        if (ges->swipe_left && !g.dragged) {
            T->menu_open();
            break;
        }
        update_deploy(dt, in);
        break;

    case READY:
        if (g.phase_t > 1.1f) set_phase(HUNT);
        break;

    case HUNT:
        update_hunt(dt, in);
        break;

    case TRACK:
        update_track(dt);
        break;

    case CONTACT:
        if (g.phase_t < (g.shot_sunk >= 0 ? 2.0f : 1.2f)) break;
        if (g.theirs.afloat == 0) finish_game(true);
        else if (g.shot_result == SHOT_HIT) set_phase(HUNT);   // a hit shoots again
        else begin_incoming();
        break;

    case INCOMING: {
        const float step = TAU / TRACK_S * dt;
        g.their_sweep = fmodf(g.their_sweep + step, TAU);
        g.their_travel -= step;
        if (g.their_travel > 0) break;
        g.their_sweep = fmodf(bearing_of(g.their_target) + TAU, TAU);
        fire(&g.mine, g.their_target);
        ai_learn();
        if (g.shot_result == SHOT_HIT) sfx_struck();
        else sfx_miss();
        set_phase(STRUCK);
        break;
    }

    case STRUCK:
        if (g.phase_t < (g.shot_sunk >= 0 ? 1.8f : g.shot_result == SHOT_HIT ? 1.0f : 0.7f)) break;
        if (g.mine.afloat == 0) finish_game(false);
        else if (g.shot_result == SHOT_HIT) begin_incoming();
        else set_phase(HUNT);
        break;

    case RESULT:
        if (g.phase_t > 1.5f && ges->tap) begin_deploy();
        break;
    }
}

// ---------------------------------------------------------------------------- drawing

static void ring(int cx, int cy, int r, uint8_t c, bool dashed)
{
    int x = r, y = 0, err = 1 - r, n = 0;
    while (x >= y) {
        if (!dashed || (n / 2) % 2 == 0) {
            T->canvas_pixel(g.cv, cx + x, cy + y, c), T->canvas_pixel(g.cv, cx - x, cy + y, c);
            T->canvas_pixel(g.cv, cx + x, cy - y, c), T->canvas_pixel(g.cv, cx - x, cy - y, c);
            T->canvas_pixel(g.cv, cx + y, cy + x, c), T->canvas_pixel(g.cv, cx - y, cy + x, c);
            T->canvas_pixel(g.cv, cx + y, cy - x, c), T->canvas_pixel(g.cv, cx - y, cy - x, c);
        }
        n++, y++;
        if (err < 0) err += 2 * y + 1;
        else x--, err += 2 * (y - x) + 1;
    }
}

static void mark_miss(int c, uint8_t col)
{
    ring(cell_x(c), cell_y(c), 3, col, false);
    T->canvas_pixel(g.cv, cell_x(c), cell_y(c), col);
}

static void mark_hit(int c, uint8_t col, uint8_t bed)
{
    const int x = cell_x(c), y = cell_y(c);
    T->canvas_fill_circle(g.cv, x, y, 4, bed);
    for (int k = -3; k <= 3; k++) {
        T->canvas_pixel(g.cv, x + k, y + k, col), T->canvas_pixel(g.cv, x + k + 1, y + k, col);
        T->canvas_pixel(g.cv, x + k, y - k, col), T->canvas_pixel(g.cv, x + k + 1, y - k, col);
    }
}

// A hull: a capsule along its cells, with a rib between each.
static void hull(const ship_t *s, int len, uint8_t col, uint8_t bed)
{
    const int x0 = CC + s->i * PITCH - 5, y0 = CC + s->j * PITCH - 5;
    const int w = s->vertical ? 11 : (len - 1) * PITCH + 11, h = s->vertical ? (len - 1) * PITCH + 11 : 11;
    T->canvas_fill_rect(g.cv, x0 + 1, y0 + 1, w - 2, h - 2, bed);
    T->canvas_fill_rect(g.cv, x0 + 3, y0, w - 6, 1, col), T->canvas_fill_rect(g.cv, x0 + 3, y0 + h - 1, w - 6, 1, col);
    T->canvas_fill_rect(g.cv, x0, y0 + 3, 1, h - 6, col), T->canvas_fill_rect(g.cv, x0 + w - 1, y0 + 3, 1, h - 6, col);
    // the corners, cut
    const int cx[4] = {x0, x0 + w - 1, x0, x0 + w - 1}, cy[4] = {y0, y0, y0 + h - 1, y0 + h - 1};
    for (int k = 0; k < 4; k++) {
        const int sx = k & 1 ? -1 : 1, sy = k & 2 ? -1 : 1;
        T->canvas_pixel(g.cv, cx[k], cy[k], g.c_bg), T->canvas_pixel(g.cv, cx[k] + sx, cy[k], g.c_bg);
        T->canvas_pixel(g.cv, cx[k], cy[k] + sy, g.c_bg);
        T->canvas_pixel(g.cv, cx[k] + sx, cy[k] + sy, col);
        T->canvas_pixel(g.cv, cx[k] + 2 * sx, cy[k], col), T->canvas_pixel(g.cv, cx[k], cy[k] + 2 * sy, col);
    }
    for (int k = 1; k < len; k++) {
        if (s->vertical) T->canvas_fill_rect(g.cv, x0 + 3, y0 + k * PITCH - 2, 5, 1, col);
        else T->canvas_fill_rect(g.cv, x0 + k * PITCH - 2, y0 + 3, 1, 5, col);
    }
}

// The static picture, with the glow of a sweep laid into the empty water behind its line.
static void paint_scope(float sweep, bool theirs, bool lit)
{
    uint8_t *px = T->canvas_pixels(g.cv);
    const uint16_t at = (uint16_t)(sweep / TAU * 65536.0f);
    const uint32_t wedge = (uint32_t)(WEDGE * 65536.0f);
    const uint8_t *glow = g.c_glow[theirs ? 1 : 0];
    for (int k = 0; k < CW * CW; k++) {
        const uint8_t what = g.bg[k];
        if (what != BG_NONE || !lit) {
            px[k] = g.bg_color[what];
            continue;
        }
        const uint32_t behind = (uint16_t)(at - g.ang[k]);
        px[k] = behind < wedge ? glow[behind * GLOW / wedge] : g.c_bg;
    }
}

static void sweep_line(float sweep, uint8_t col)
{
    const int x = CC + (int)lroundf(cosf(sweep) * R_PLAY), y = CC + (int)lroundf(sinf(sweep) * R_PLAY);
    T->canvas_line(g.cv, CC, CC, x, y, col);
    T->canvas_fill_circle(g.cv, x, y, 2, col);
}

static void label(int y, const char *s, uint8_t col)
{
    const int w = T->text_width(s, 1, true) + 6;
    T->canvas_fill_rect(g.cv, CC - w / 2, y - 5, w, 10, g.c_bg);
    T->canvas_text_centered(g.cv, CC, y, s, col, 1, true);
}

// Where the word about a shot goes: across the middle, unless that is where the shot landed.
static int verdict_y(int c)
{
    const int y = cell_y(c);
    return y < 84 || y > 150 ? 98 : y < CC ? 146 : 52;
}

static void band(int y, int h) { T->canvas_fill_rect(g.cv, 0, y, CW, h, g.c_bg); }
static void text(int y, const char *s, uint8_t col, int scale) { T->canvas_text_centered(g.cv, CC, y, s, col, scale, true); }

static void draw_fleet(const fleet_t *f, bool sunk_only, uint8_t col, uint8_t bed)
{
    for (int s = 0; s < SHIPS; s++) {
        const bool sunk = f->ship[s].hits == SHIP_LEN[s];
        if (!f->ship[s].placed || (sunk_only && !sunk)) continue;
        hull(&f->ship[s], SHIP_LEN[s], sunk ? g.c_red : col, sunk ? g.c_sunk_bed : bed);
    }
}

static void draw_shots(const fleet_t *f, uint8_t hit_col, uint8_t hit_bed)
{
    for (int c = 0; c < CELLS; c++) {
        if (f->shot[c] == SHOT_MISS) mark_miss(c, g.c_miss);
        else if (f->shot[c] == SHOT_HIT) mark_hit(c, hit_col, hit_bed);
    }
}

// A strike blooming out of a cell: rings that grow and thin for the first half second.
static void bloom(int c, float t, uint8_t col)
{
    if (t > 0.7f) return;
    const int r = 4 + (int)(t * 30);
    ring(cell_x(c), cell_y(c), r, col, false);
    if (r > 9) ring(cell_x(c), cell_y(c), r - 5, col, true);
}

static void rd_draw(void)
{
    if (!g.cv || !g.bg || !g.ang) return;
    char line[40];

    if (T->menu_is_open()) {
        const tat_menu_row_t rows[] = {
            T->menu_sound_row(),
            {"NEW BATTLE", "GO", T->ui_color(TAT_UI_ACCENT)},
        };
        T->menu_draw(rows, 2, "PAUSED");
        return;
    }
    // Everything that moves is a sweep; the rest is drawn when it changes.
    const bool moving = g.phase == TITLE || g.phase == HUNT || g.phase == TRACK || g.phase == INCOMING || g.phase == CONTACT ||
                        g.phase == STRUCK;
    if (!moving && !g.dirty) return;
    g.dirty = false;

    switch (g.phase) {
    case TITLE:
        paint_scope(g.sweep, false, true);
        sweep_line(g.sweep, g.c_green);
        band(84, 66);
        text(100, "RADAR", g.c_white, 4);
        text(124, "SEA BATTLE", g.c_green, 2);
        text(141, "5 SHIPS  137 CELLS", g.c_dim, 1);
        text(172, "TAP TO DEPLOY", g.c_pale, 1);
        if (g.wins > 0) {
            snprintf(line, sizeof(line), "WINS %d  BEST %d", g.wins, g.best);
            text(196, line, g.c_dim, 1);
        }
        break;

    case DEPLOY:
    case READY: {
        paint_scope(0, false, false);
        for (int s = 0; s < SHIPS; s++)
            if (g.mine.ship[s].placed) hull(&g.mine.ship[s], SHIP_LEN[s], g.c_green, g.c_hull_bed);
        if (g.phase == DEPLOY) {
            const bool ok = hand_ok();
            hull(&g.mine.ship[g.hand], SHIP_LEN[g.hand], ok ? g.c_amber : g.c_red, ok ? g.c_amber_bed : g.c_sunk_bed);
            snprintf(line, sizeof(line), "%s %d", SHIP_NAME[g.hand], SHIP_LEN[g.hand]);
            label(12, "DEPLOY FLEET", g.c_dim);
            label(23, line, g.c_amber);
            label(211, "DRAG  TAP TO TURN", g.c_dim);
            label(221, "PWR AUTO", g.c_dim);
        } else {
            band(100, 34);
            text(111, "FLEET READY", g.c_green, 2);
            text(126, "FIND THEIRS", g.c_pale, 1);
        }
        // The roster: a bar per ship, as long as it is.
        int x = CC - 46;
        for (int s = 0; s < SHIPS && g.phase == DEPLOY; s++) {
            const int w = SHIP_LEN[s] * 4;
            const uint8_t col = g.mine.ship[s].placed ? g.c_green : s == g.hand ? g.c_amber : g.c_tick;
            T->canvas_fill_rect(g.cv, x, 200, w, 3, col);
            x += w + 5;
        }
        break;
    }

    case HUNT:
    case TRACK:
    case CONTACT: {
        paint_scope(g.sweep, false, true);
        draw_fleet(&g.theirs, true, g.c_red, g.c_sunk_bed);
        sweep_line(g.sweep, g.c_green);
        draw_shots(&g.theirs, g.c_red, g.c_hit_bed);
        if (g.armed >= 0) {
            const int x = cell_x(g.armed), y = cell_y(g.armed);
            if (g.phase == HUNT) {
                // Cross-hairs out to the rim: the finger is on top of the pin itself.
                const int reach_x = (int)sqrtf((float)(R_PLAY * R_PLAY - (y - CC) * (y - CC)));
                const int reach_y = (int)sqrtf((float)(R_PLAY * R_PLAY - (x - CC) * (x - CC)));
                T->canvas_fill_rect(g.cv, CC - reach_x, y, 2 * reach_x + 1, 1, g.c_cand);
                T->canvas_fill_rect(g.cv, x, CC - reach_y, 1, 2 * reach_y + 1, g.c_cand);
                T->canvas_fill_rect(g.cv, x - 6, y - 6, 13, 13, g.c_cand_bed);
                T->canvas_rect(g.cv, x - 6, y - 6, 13, 13, g.c_amber);
            }
            ring(x, y, 6, g.c_amber, true);
            T->canvas_fill_circle(g.cv, x, y, 2, g.c_amber);
        }
        snprintf(line, sizeof(line), "SWEEP %02d", g.shots + (g.phase == HUNT ? 1 : 0));
        label(12, line, g.c_dim);
        snprintf(line, sizeof(line), "%d/5 AFLOAT", g.theirs.afloat);
        label(221, line, g.c_dim);

        if (g.phase == CONTACT) {
            const bool hit = g.shot_result == SHOT_HIT;
            bloom(g.shot_cell, g.phase_t, hit ? g.c_red : g.c_miss);
            if (g.phase_t > 0.25f) {
                const int y = verdict_y(g.shot_cell);
                band(y, 38);
                text(y + 12, g.shot_sunk >= 0 ? "SUNK" : hit ? "HIT" : "MISS", hit ? g.c_red : g.c_pale, 3);
                text(y + 30, g.shot_sunk >= 0 ? SHIP_NAME[g.shot_sunk] : hit ? "CLASS UNKNOWN  DROP ANOTHER" : "OPEN WATER",
                     g.c_pale, 1);
            }
        } else if (g.phase == HUNT && !g.first_touch && g.played < 3) {
            band(150, 34);
            text(158, "TOUCH TO PLACE A PIN", g.c_amber, 1);
            text(168, "SLIDE TO MOVE IT", g.c_pale, 1);
            text(178, "LET GO TO DROP IT", g.c_pale, 1);
        }
        break;
    }

    case INCOMING:
    case STRUCK: {
        paint_scope(g.their_sweep, true, true);
        draw_fleet(&g.mine, false, g.c_green, g.c_hull_bed);
        sweep_line(g.their_sweep, g.c_amber);
        draw_shots(&g.mine, g.c_amber, g.c_amber_bed);
        label(12, "INCOMING", g.c_amber);
        snprintf(line, sizeof(line), "HULL %d/%d", g.mine.hull, HULL);
        label(221, line, g.c_dim);
        if (g.phase == INCOMING) {
            ring(cell_x(g.their_target), cell_y(g.their_target), 6, g.c_amber, true);
            T->canvas_fill_circle(g.cv, cell_x(g.their_target), cell_y(g.their_target), 2, g.c_amber);
        } else {
            const bool hit = g.shot_result == SHOT_HIT;
            bloom(g.shot_cell, g.phase_t, hit ? g.c_amber : g.c_miss);
            if (g.phase_t > 0.25f && (hit || g.shot_sunk >= 0)) {
                const int y = verdict_y(g.shot_cell);
                band(y, 38);
                text(y + 12, g.shot_sunk >= 0 ? "SHIP LOST" : "STRUCK", g.c_amber, 3);
                text(y + 30, g.shot_sunk >= 0 ? SHIP_NAME[g.shot_sunk] : "THEY FIRE AGAIN", g.c_pale, 1);
            }
        }
        break;
    }

    case RESULT: {
        paint_scope(0, false, false);
        // Their water, with everything in it shown at last.
        draw_fleet(&g.theirs, false, g.won ? g.c_red : g.c_green, g.won ? g.c_sunk_bed : g.c_hull_bed);
        draw_shots(&g.theirs, g.c_red, g.c_hit_bed);
        band(74, 86);
        text(86, g.won ? "ALL CONTACTS SUNK" : "YOUR FLEET IS LOST", g.c_dim, 1);
        text(104, g.won ? "VICTORY" : "DEFEAT", g.won ? g.c_green : g.c_red, 3);
        const char *stat_name[3] = {"SWEEPS", "ACCURACY", g.won ? "HULL LEFT" : "THEIR HULL"};
        int value[3] = {g.shots, g.shots ? g.hits * 100 / g.shots : 0, g.won ? g.mine.hull : g.theirs.hull};
        for (int k = 0; k < 3; k++) {
            const int x = CC + (k - 1) * 62;
            T->canvas_text_centered(g.cv, x, 126, stat_name[k], g.c_dim, 1, false);
            snprintf(line, sizeof(line), k == 1 ? "%d%%" : "%d", value[k]);
            T->canvas_text_centered(g.cv, x, 142, line, g.c_white, 2, true);
        }
        if (g.phase_t > 1.5f) label(180, "TAP TO REDEPLOY", g.c_pale);
        if (g.won && g.best == g.shots) label(196, "FEWEST SHOTS YET", g.c_amber);
        break;
    }
    }
    if (g.phase == RESULT && g.phase_t <= 1.6f) g.dirty = true;   // for the line that arrives late

    T->canvas_present(g.cv);
}

// ---------------------------------------------------------------------------- the scope

// The part of the picture that never changes, and each pixel's angle, worked out once.
static void build_scope(void)
{
    const uint16_t *pol_a = T->polar_angles(), *pol_r = T->polar_radii();
    for (int y = 0; y < CW; y++)
        for (int x = 0; x < CW; x++) {
            const int k = y * CW + x, idx = (SCALE * y) * TAT_SCREEN + SCALE * x;
            const float r = pol_r[idx] / 16.0f / SCALE;
            const int dx = x - CC, dy = y - CC;
            g.ang[k] = pol_a[idx];
            uint8_t what = r <= R_PLAY ? BG_NONE : BG_OUT;
            if (what == BG_NONE) {
                const bool gx = (dx % PITCH + PITCH) % PITCH == 0, gy = (dy % PITCH + PITCH) % PITCH == 0;
                if (gx || gy) what = BG_GRID;
                if (fabsf(r - 35) < 0.5f || fabsf(r - 65) < 0.5f || fabsf(r - 98) < 0.5f) what = BG_RING;
                if (gx && gy && is_ocean(dx / PITCH, dy / PITCH)) what = BG_DOT;
            } else if (fabsf(r - R_RING) < 0.5f) {
                what = BG_TICK;
            } else if (r >= R_RING + 2 && r <= R_TICK) {
                // Seventy-two ticks round the rim, every ninth one heavier: 5 and 45 degrees.
                const float deg = g.ang[k] * (360.0f / 65536.0f), in_tick = fmodf(deg + 0.6f, 5.0f);
                const float in_major = fmodf(deg + 1.2f, 45.0f);
                if (in_major < 2.4f) what = BG_MAJOR;
                else if (in_tick < 1.2f && r >= R_RING + 5) what = BG_TICK;
            }
            g.bg[k] = what;
        }
}

static tat_color_t shade(const uint8_t *rgb, float k)
{
    return T->rgb((uint8_t)(rgb[0] * k), (uint8_t)(rgb[1] * k), (uint8_t)(rgb[2] * k));
}

static void rd_begin(const tat_api_t *api)
{
    T = api;
    memset(&g, 0, sizeof(g));
    g.cv = T->canvas_create(SCALE, 0);
    g.bg = T->alloc((size_t)CW * CW);
    g.ang = T->alloc((size_t)CW * CW * sizeof(uint16_t));
    if (!g.cv || !g.bg || !g.ang) {
        T->log("no memory for the scope");
        return;
    }
    build_scope();

    static const uint8_t GREEN[3] = {52, 240, 154}, AMBER[3] = {255, 194, 75}, RED[3] = {255, 92, 70};
    g.c_bg = T->canvas_color(g.cv, T->rgb(0, 0, 0));
    g.c_grid = T->canvas_color(g.cv, T->rgb(9, 30, 22));
    g.c_ring = T->canvas_color(g.cv, T->rgb(14, 44, 32));
    g.c_dot = T->canvas_color(g.cv, T->rgb(64, 176, 124));
    g.c_tick = T->canvas_color(g.cv, T->rgb(28, 84, 64));
    g.c_major = T->canvas_color(g.cv, T->rgb(23, 169, 107));
    g.c_green = T->canvas_color(g.cv, shade(GREEN, 1));
    g.c_pale = T->canvas_color(g.cv, T->rgb(143, 227, 188));
    g.c_dim = T->canvas_color(g.cv, T->rgb(111, 184, 148));
    g.c_white = T->canvas_color(g.cv, T->rgb(233, 255, 244));
    g.c_amber = T->canvas_color(g.cv, shade(AMBER, 1));
    g.c_red = T->canvas_color(g.cv, shade(RED, 1));
    g.c_miss = T->canvas_color(g.cv, T->rgb(72, 120, 96));
    g.c_hit_bed = T->canvas_color(g.cv, shade(RED, 0.22f));
    g.c_sunk_bed = T->canvas_color(g.cv, shade(RED, 0.15f));
    g.c_cand = T->canvas_color(g.cv, shade(AMBER, 0.5f));
    g.c_cand_bed = T->canvas_color(g.cv, shade(AMBER, 0.12f));
    g.c_amber_bed = T->canvas_color(g.cv, shade(AMBER, 0.2f));
    g.c_hull_bed = T->canvas_color(g.cv, shade(GREEN, 0.17f));
    for (int k = 0; k < GLOW; k++) {
        // Brightest just behind the line, and gone by the end of the wedge.
        const float fade = 1.0f - (float)k / GLOW, level = 0.30f * fade * fade;
        g.c_glow[0][k] = T->canvas_color(g.cv, shade(GREEN, level));
        g.c_glow[1][k] = T->canvas_color(g.cv, shade(AMBER, level));
    }
    g.bg_color[BG_OUT] = g.bg_color[BG_NONE] = g.c_bg;
    g.bg_color[BG_GRID] = g.c_grid;
    g.bg_color[BG_RING] = g.c_ring;
    g.bg_color[BG_DOT] = g.c_dot;
    g.bg_color[BG_TICK] = g.c_tick;
    g.bg_color[BG_MAJOR] = g.c_major;

    T->save_get("wins", &g.wins, 0);
    T->save_get("best", &g.best, 0);
    T->save_get("played", &g.played, 0);
    fleet_clear(&g.mine);
    fleet_clear(&g.theirs);
    g.armed = -1;
    set_phase(TITLE);
    T->log("ready: %d wins, best %d shots", g.wins, g.best);
}

static void rd_enter(void)
{
    g.dirty = true;
    g.aiming = false;
    g.armed = -1;
}

static void rd_redraw(void) { g.dirty = true; }

static bool rd_keep_awake(void)
{
    return g.phase != TITLE && g.phase != RESULT && g.phase != DEPLOY && !T->menu_is_open();
}

static void rd_unload(void)
{
    if (g.cv) T->canvas_destroy(g.cv);
    if (g.bg) T->free(g.bg);
    if (g.ang) T->free(g.ang);
    g.cv = NULL;
    g.bg = NULL;
    g.ang = NULL;
}

const tat_game_t tat_game = {
    .magic = TAT_GAME_MAGIC,
    .api_major = TAT_API_MAJOR,
    .api_minor = TAT_API_MINOR,
    .id = "radar",
    .name = "RADAR",
    .accent_r = 52, .accent_g = 240, .accent_b = 154,
    .assets = NULL,
    .asset_count = 0,
    .begin = rd_begin,
    .enter = rd_enter,
    .update = rd_update,
    .draw = rd_draw,
    .leave = NULL,
    .unload = rd_unload,
    .redraw = rd_redraw,
    .keep_awake = rd_keep_awake,
};
