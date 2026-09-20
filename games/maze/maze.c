// MARBLE MAZE - tilt the watch to roll a steel ball through a wooden labyrinth.
//
// Every level carves a fresh maze into the round board. Roll the ball from the green ring
// to the checkered flag without dropping it into one of the holes that wait at the dead
// ends. Three balls, and the maze gets bigger and holier the further you get.
//
//   tilt        - roll the ball
//   tap         - start the level, or play again after game over
//   swipe left  - pause menu (level, sound, new game)
//   BOOT        - back to the home menu (console-wide)
//
// Written against tat_api.h alone: it includes nothing else from the console and calls
// nothing by name, which is what will let it become an installable file. The two PNGs it
// draws with arrive through api->asset(), so it neither knows nor cares whether they were
// linked into the firmware or unpacked from a package.
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "tat/tat_api.h"

static const tat_api_t *T;

// ---------------------------------------------------------------- the board

// The board lives on a 233 x 233 pixel canvas that the presenter doubles onto the screen,
// so all positions and speeds below are canvas pixels.
#define SCALE 2
#define BOARD_R 100.0f   /* the maze lives inside this circle; HUD sits outside it */

#define GRAVITY 750.0f      /* px/s^2 per g of tilt */
#define FRICTION 1.1f       /* 1/s */
#define MAX_SPEED 215.0f
#define RESTITUTION 0.28f

// Fixed arrays instead of the vectors the C++ version grew, so the whole game fits in one
// budget known at build time. Every bound below comes from the grid never being larger
// than MAX_N: generate() clamps n to 13.
#define MAX_N 13
#define MAX_CELLS (MAX_N * MAX_N)   /* 169 cells, and with them the carve and search scratch:
                                       each cell is marked before it is pushed, so neither the
                                       depth-first stack nor the breadth-first queue can ever
                                       hold a cell twice */
// Each valid cell contributes at most two walls of its own - the one above it and the one
// to its left - and the board's outside edge adds the rest. The valid region is a disc, so
// every column has exactly one cell with nothing below it and every row exactly one with
// nothing to its right: that is the 2 * MAX_N.
#define MAX_WALLS (2 * MAX_CELLS + 2 * MAX_N)
// A hole needs a valid dead-end cell that is neither the start nor the finish, so there can
// never be more of them than there are cells. A tighter bound would need an argument about
// how many dead ends a perfect maze can have, and 169 of these costs 1.3 KB.
#define MAX_HOLES MAX_CELLS

typedef struct {
    float x0, y0, x1, y1;
} Wall;

typedef struct {
    float x, y;
} Hole;

typedef enum { READY, PLAYING, FALLING, CLEARED, GAME_OVER } Phase;

static struct {
    tat_canvas_t *cv;
    int W, H;   /* the canvas, asked for rather than assumed */

    /* the run */
    int level, lives, best;
    Phase phase;
    float phase_t;

    /* the maze */
    int n;                 /* grid is n x n, clipped to the round board */
    float cs;              /* cell size in pixels */
    float ox, oy;          /* top-left of the grid */
    float cx, cy;          /* the middle of the canvas, where the board is centred */
    float wall_t;          /* wall thickness */
    uint8_t valid[MAX_CELLS];    /* cell is on the board */
    uint8_t open_e[MAX_CELLS];   /* passage to the cell on the right */
    uint8_t open_s[MAX_CELLS];   /* passage to the cell below */
    Wall walls[MAX_WALLS];
    int n_walls;
    Hole holes[MAX_HOLES];
    int n_holes;
    float hole_r;
    int start_cell, finish_cell;

    // Scratch for the carve and the searches. It lives here rather than on the stack
    // because a game's task stack is small and these are 169 entries each; a cell index
    // and a passage distance both fit in an int16_t.
    uint8_t seen[MAX_CELLS];
    int16_t stack[MAX_CELLS];
    int16_t queue[MAX_CELLS];
    int16_t dist[MAX_CELLS];

    /* the ball */
    float bx, by, vx, vy, br;
    float tilt_nx, tilt_ny;   /* "level": how the watch was held at the tap */
    float fall_x, fall_y;
    float roll_t;
    float still_t;            /* seconds since the ball last really moved */

    /* drawing */
    tat_sheet_t *ball, *flag;
    uint8_t *board;           /* the static scene, copied in under the ball each frame */
    bool redraw;
    uint8_t c_floor, c_floor2, c_plank, c_grain, c_wall, c_wall_lit, c_wall_dark;
    uint8_t c_shadow, c_hole, c_rim, c_rim_lit, c_start, c_white, c_black;
    uint8_t c_dim, c_panel, c_box, c_go, c_danger, c_label, c_steel;
} g;

// ---------------------------------------------------------------- odds and ends

static uint32_t rnd(uint32_t n) { return T->random() % n; }
static float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
static float minf(float a, float b) { return a < b ? a : b; }
static int imin(int a, int b) { return a < b ? a : b; }

static int cell_idx(int c, int r) { return r * g.n + c; }
static float cell_x(int i) { return g.ox + (i % g.n + 0.5f) * g.cs; }
static float cell_y(int i) { return g.oy + (i / g.n + 0.5f) * g.cs; }

// ---------------------------------------------------------------- sound

static void tone1(float f0, float f1, uint16_t ms, uint8_t wave, float vol, uint16_t delay)
{
    const tat_tone_t t = {f0, f1, ms, wave, vol, delay};
    T->tone(&t);
}

static void sfx_start(void) { tone1(520, 780, 90, TAT_TRIANGLE, 0.7f, 0); }

static void sfx_thunk(float strength)
{
    tone1(150, 80, 45, TAT_TRIANGLE, clampf(0.25f + strength * 0.7f, 0.25f, 0.95f), 0);
}

static void sfx_roll(float speed01) { tone1(90, 70, 28, TAT_NOISE, 0.05f + 0.10f * speed01, 0); }

static void sfx_fall(void)
{
    tone1(640, 70, 380, TAT_SQUARE, 0.7f, 0);
    tone1(110, 50, 130, TAT_NOISE, 0.5f, 360);
}

static void sfx_cleared(void)
{
    tone1(523, 0, 90, TAT_SQUARE, 0.7f, 0);
    tone1(659, 0, 90, TAT_SQUARE, 0.7f, 90);
    tone1(784, 0, 90, TAT_SQUARE, 0.7f, 180);
    tone1(1047, 0, 220, TAT_SQUARE, 0.8f, 270);
}

static void sfx_game_over(void)
{
    tone1(392, 0, 160, TAT_SQUARE, 0.7f, 0);
    tone1(330, 0, 160, TAT_SQUARE, 0.7f, 160);
    tone1(262, 0, 320, TAT_SQUARE, 0.7f, 320);
}

// ---------------------------------------------------------------- the maze

static bool is_open(int a, int b)
{
    if (b == a + 1) return g.open_e[a];
    if (b == a - 1) return g.open_e[b];
    if (b == a + g.n) return g.open_s[a];
    if (b == a - g.n) return g.open_s[b];
    return false;
}

static void neighbours(int i, int out[4], int *count)
{
    *count = 0;
    const int c = i % g.n, r = i / g.n;
    if (c + 1 < g.n && g.valid[i + 1]) out[(*count)++] = i + 1;
    if (c > 0 && g.valid[i - 1]) out[(*count)++] = i - 1;
    if (r + 1 < g.n && g.valid[i + g.n]) out[(*count)++] = i + g.n;
    if (r > 0 && g.valid[i - g.n]) out[(*count)++] = i - g.n;
}

// Farthest cell from `from` through the passages (breadth-first). The C++ version could
// also hand back the whole distance map through an optional out-parameter; nothing ever
// asked for it, so the parameter is gone and `dist` is scratch again.
static int farthest(int from)
{
    const int cells = g.n * g.n;
    for (int i = 0; i < cells; i++) g.dist[i] = -1;
    g.dist[from] = 0;
    g.queue[0] = (int16_t)from;
    int tail = 1;
    int last = from;
    for (int q = 0; q < tail; q++) {
        const int i = g.queue[q];
        last = i;
        int nb[4], cnt;
        neighbours(i, nb, &cnt);
        for (int k = 0; k < cnt; k++) {
            if (g.dist[nb[k]] >= 0 || !is_open(i, nb[k])) continue;
            g.dist[nb[k]] = (int16_t)(g.dist[i] + 1);
            g.queue[tail++] = (int16_t)nb[k];
        }
    }
    return last;
}

static void add_wall(float x0, float y0, float x1, float y1)
{
    if (g.n_walls >= MAX_WALLS) return;
    Wall *w = &g.walls[g.n_walls++];
    w->x0 = x0;
    w->y0 = y0;
    w->x1 = x1;
    w->y1 = y1;
}

static void build_walls(void)
{
    g.n_walls = 0;
    const float h = g.wall_t / 2;
    for (int r = 0; r < g.n; r++) {
        for (int c = 0; c < g.n; c++) {
            const int i = cell_idx(c, r);
            if (!g.valid[i]) continue;
            const float x = g.ox + c * g.cs, y = g.oy + r * g.cs;
            // Top and left edges: a wall unless there's a passage to that neighbour.
            const bool up = r > 0 && g.valid[i - g.n] && g.open_s[i - g.n];
            const bool left = c > 0 && g.valid[i - 1] && g.open_e[i - 1];
            if (!up) add_wall(x - h, y - h, x + g.cs + h, y + h);
            if (!left) add_wall(x - h, y - h, x + h, y + g.cs + h);
            // Bottom and right only where the board ends (otherwise the neighbour draws it).
            const bool has_down = r + 1 < g.n && g.valid[i + g.n];
            const bool has_right = c + 1 < g.n && g.valid[i + 1];
            if (!has_down) add_wall(x - h, y + g.cs - h, x + g.cs + h, y + g.cs + h);
            if (!has_right) add_wall(x + g.cs - h, y - h, x + g.cs + h, y + g.cs + h);
        }
    }
}

static void generate(void)
{
    // Bigger, tighter mazes as the levels climb.
    g.n = imin(7 + (g.level - 1) / 2 * 2, MAX_N);
    g.cs = floorf(2 * BOARD_R / g.n);
    g.ox = g.cx - g.n * g.cs / 2;
    g.oy = g.cy - g.n * g.cs / 2;
    g.wall_t = floorf(g.cs / 8);
    if (g.wall_t < 2.0f) g.wall_t = 2.0f;
    g.br = g.cs * 0.26f;
    g.hole_r = g.cs * 0.30f;

    const int cells = g.n * g.n;
    memset(g.valid, 0, (size_t)cells);
    memset(g.open_e, 0, (size_t)cells);
    memset(g.open_s, 0, (size_t)cells);
    int first = -1;
    for (int r = 0; r < g.n; r++) {
        for (int c = 0; c < g.n; c++) {
            const float dx = g.ox + (c + 0.5f) * g.cs - g.cx, dy = g.oy + (r + 0.5f) * g.cs - g.cy;
            if (sqrtf(dx * dx + dy * dy) <= BOARD_R - g.cs * 0.30f) {
                g.valid[cell_idx(c, r)] = 1;
                if (first < 0) first = cell_idx(c, r);
            }
        }
    }

    // Depth-first carve: a perfect maze (exactly one route between any two cells).
    memset(g.seen, 0, (size_t)cells);
    int sp = 0;
    g.stack[sp++] = (int16_t)first;
    g.seen[first] = 1;
    while (sp > 0) {
        const int i = g.stack[sp - 1];
        int nb[4], cnt;
        neighbours(i, nb, &cnt);
        int fresh[4], fc = 0;
        for (int k = 0; k < cnt; k++)
            if (!g.seen[nb[k]]) fresh[fc++] = nb[k];
        if (fc == 0) {
            sp--;
            continue;
        }
        const int j = fresh[rnd((uint32_t)fc)];
        if (j == i + 1) g.open_e[i] = 1;
        else if (j == i - 1) g.open_e[j] = 1;
        else if (j == i + g.n) g.open_s[i] = 1;
        else g.open_s[j] = 1;
        g.seen[j] = 1;
        g.stack[sp++] = (int16_t)j;
    }

    // Start and finish at the two ends of the longest route through the maze.
    g.start_cell = farthest(first);
    g.finish_cell = farthest(g.start_cell);

    // Holes wait at the end of wrong turns. More of them as levels climb.
    g.n_holes = 0;
    const float chance = minf(0.45f + 0.07f * g.level, 0.9f);
    for (int i = 0; i < cells; i++) {
        if (!g.valid[i] || i == g.start_cell || i == g.finish_cell) continue;
        int nb[4], cnt, exits = 0;
        neighbours(i, nb, &cnt);
        for (int k = 0; k < cnt; k++) exits += is_open(i, nb[k]);
        if (exits == 1 && rnd(1000) < (uint32_t)(chance * 1000) && g.n_holes < MAX_HOLES) {
            g.holes[g.n_holes].x = cell_x(i);
            g.holes[g.n_holes].y = cell_y(i);
            g.n_holes++;
        }
    }

    build_walls();
    T->log("level %d: %dx%d, %d walls, %d holes", g.level, g.n, g.n, g.n_walls, g.n_holes);
}

// ---------------------------------------------------------------- flow

static void save_best(void) { T->save_set("best", g.best); }

static void place_ball(void)
{
    g.bx = cell_x(g.start_cell);
    g.by = cell_y(g.start_cell);
    g.vx = g.vy = 0;
}

static void new_level(void)
{
    generate();
    place_ball();
    g.phase = READY;
    g.phase_t = 0;
    g.redraw = true;
}

static void new_game(void)
{
    g.level = 1;
    g.lives = 3;
    new_level();
}

static void begin_play(void)
{
    // Level is real level (flat), so tipping toward you always rolls toward you.
    // Capturing "how it's held now" made the ball ignore the natural viewing tilt.
    // The pause menu's LEVEL row can still set a custom level.
    g.phase = PLAYING;
    g.phase_t = 0;
    g.still_t = 0;
    g.redraw = true;
    sfx_start();
}

// ---------------------------------------------------------------- physics

static bool collide(void)
{
    bool hit = false;
    for (int i = 0; i < g.n_walls; i++) {
        const Wall *w = &g.walls[i];
        const float px = clampf(g.bx, w->x0, w->x1), py = clampf(g.by, w->y0, w->y1);
        float dx = g.bx - px, dy = g.by - py;
        const float d2 = dx * dx + dy * dy;
        if (d2 >= g.br * g.br) continue;
        float d = sqrtf(d2);
        if (d < 0.001f) {   /* center inside the wall: push out along the way we came */
            dx = -g.vx;
            dy = -g.vy;
            d = sqrtf(dx * dx + dy * dy);
            if (d < 0.001f) d = 0.001f;
        }
        const float nx = dx / d, ny = dy / d;
        g.bx = px + nx * g.br;
        g.by = py + ny * g.br;
        const float vn = g.vx * nx + g.vy * ny;
        if (vn < 0) {
            g.vx -= (1 + RESTITUTION) * vn * nx;
            g.vy -= (1 + RESTITUTION) * vn * ny;
            if (-vn > 35) sfx_thunk(clampf(-vn / MAX_SPEED, 0, 1));
        }
        hit = true;
    }
    return hit;
}

static void step_ball(const tat_input_t *in, float dt)
{
    const float ax = (in->tilt.ax - g.tilt_nx) * GRAVITY, ay = (in->tilt.ay - g.tilt_ny) * GRAVITY;
    g.vx += ax * dt;
    g.vy += ay * dt;

    // The floor dips toward a hole: get close and it starts to pull you in.
    for (int i = 0; i < g.n_holes; i++) {
        const float dx = g.holes[i].x - g.bx, dy = g.holes[i].y - g.by, d = sqrtf(dx * dx + dy * dy);
        if (d < g.hole_r * 1.5f && d > 0.5f) {
            const float pull = 450.0f * (1.0f - d / (g.hole_r * 1.5f));
            g.vx += dx / d * pull * dt;
            g.vy += dy / d * pull * dt;
        }
    }

    float damp = 1.0f - FRICTION * dt;
    if (damp < 0.0f) damp = 0.0f;
    g.vx *= damp;
    g.vy *= damp;
    float speed = sqrtf(g.vx * g.vx + g.vy * g.vy);
    if (speed > MAX_SPEED) {
        g.vx *= MAX_SPEED / speed;
        g.vy *= MAX_SPEED / speed;
        speed = MAX_SPEED;
    }

    // Small steps so a fast ball can't tunnel through a thin wall.
    int steps = (int)ceilf(speed * dt / (g.br * 0.45f));
    if (steps < 1) steps = 1;
    for (int s = 0; s < steps; s++) {
        g.bx += g.vx * dt / steps;
        g.by += g.vy * dt / steps;
        collide();
    }

    // Rolling rumble, faster ticks the faster it goes.
    g.still_t = speed > 6 ? 0 : g.still_t + dt;
    g.roll_t -= dt;
    if (speed > 22 && g.roll_t <= 0) {
        sfx_roll(clampf(speed / MAX_SPEED, 0, 1));
        g.roll_t = 0.16f - 0.10f * clampf(speed / MAX_SPEED, 0, 1);
    }

    for (int i = 0; i < g.n_holes; i++) {
        const float dx = g.holes[i].x - g.bx, dy = g.holes[i].y - g.by;
        if (dx * dx + dy * dy < g.hole_r * g.hole_r * 0.55f) {
            g.fall_x = g.holes[i].x;
            g.fall_y = g.holes[i].y;
            g.phase = FALLING;
            g.phase_t = 0;
            g.lives--;
            sfx_fall();
            return;
        }
    }

    const float fx = cell_x(g.finish_cell) - g.bx, fy = cell_y(g.finish_cell) - g.by;
    if (fx * fx + fy * fy < (g.cs * 0.30f) * (g.cs * 0.30f)) {
        g.phase = CLEARED;
        g.phase_t = 0;
        g.redraw = true;
        sfx_cleared();
        if (g.level + 1 > g.best) {
            g.best = g.level + 1;
            save_best();
        }
    }
}

// ---------------------------------------------------------------- menu

// Hold the watch how you like, then tap: that becomes level. Tap again to go back to flat.
static void level_tap(void)
{
    if (g.tilt_nx == 0 && g.tilt_ny == 0) {
        const tat_input_t *in = T->input();
        g.tilt_nx = in->tilt.ax;
        g.tilt_ny = in->tilt.ay;
    } else {
        g.tilt_nx = g.tilt_ny = 0;
    }
    g.vx = g.vy = 0;
}

// ---------------------------------------------------------------- drawing

static void fill_disc(float cx, float cy, float r, uint8_t col)
{
    const int y0 = (int)floorf(cy - r), y1 = (int)ceilf(cy + r);
    for (int y = y0; y <= y1; y++) {
        const float dy = y + 0.5f - cy;
        if (dy * dy > r * r) continue;
        const float half = sqrtf(r * r - dy * dy);
        const int xa = (int)ceilf(cx - half - 0.5f), xb = (int)floorf(cx + half - 0.5f);
        if (xb >= xa) T->canvas_fill_rect(g.cv, xa, y, xb - xa + 1, 1, col);
    }
}

static void draw_board(void)
{
    T->canvas_clear(g.cv, g.c_black);

    // Wooden floor: planks with a seam every few rows and a little grain.
    const int cells = g.n * g.n;
    for (int i = 0; i < cells; i++) {
        if (!g.valid[i]) continue;
        const int x = (int)(g.ox + (i % g.n) * g.cs), y = (int)(g.oy + (i / g.n) * g.cs), w = (int)g.cs + 1;
        T->canvas_fill_rect(g.cv, x, y, w, w, ((i % g.n) + (i / g.n)) % 2 ? g.c_floor : g.c_floor2);
    }
    for (int y = (int)g.oy; y < (int)(g.oy + g.n * g.cs); y++) {
        const bool seam = (y / 7) % 1 == 0 && y % 7 == 3;
        for (int x = (int)g.ox; x < (int)(g.ox + g.n * g.cs); x++) {
            const int ci = cell_idx(imin(g.n - 1, (int)((x - g.ox) / g.cs)), imin(g.n - 1, (int)((y - g.oy) / g.cs)));
            if (!g.valid[ci]) continue;
            const unsigned h = (unsigned)(x * 2654435761u) ^ (unsigned)(y * 40503u);
            if (seam) T->canvas_pixel(g.cv, x, y, g.c_plank);
            else if ((h >> 7) % 23 == 0) T->canvas_pixel(g.cv, x, y, g.c_grain);
        }
    }

    // Start: a green ring. Finish: the checkered flag.
    fill_disc(cell_x(g.start_cell), cell_y(g.start_cell), g.cs * 0.34f, g.c_start);
    fill_disc(cell_x(g.start_cell), cell_y(g.start_cell), g.cs * 0.22f,
              ((g.start_cell % g.n) + (g.start_cell / g.n)) % 2 ? g.c_floor : g.c_floor2);
    {
        float fs = g.cs * 0.6f;
        if (fs < 6.0f) fs = 6.0f;
        T->canvas_sprite_scaled(g.cv, g.flag, 0, cell_x(g.finish_cell), cell_y(g.finish_cell) + fs / 2, fs / 12,
                                fs / 12, false);
    }

    // Holes: a dark pit with a lit far edge, so it reads as a dip in the wood.
    for (int i = 0; i < g.n_holes; i++) {
        const Hole *h = &g.holes[i];
        fill_disc(h->x, h->y, g.hole_r + 1.5f, g.c_rim);
        fill_disc(h->x + 0.8f, h->y + 0.8f, g.hole_r + 0.5f, g.c_rim_lit);
        fill_disc(h->x, h->y, g.hole_r, g.c_hole);
    }

    // Walls: a drop shadow, then the wall with a lit top/left and dark bottom/right.
    for (int i = 0; i < g.n_walls; i++) {
        const Wall *w = &g.walls[i];
        T->canvas_fill_rect(g.cv, (int)w->x0 + 1, (int)w->y0 + 2, (int)(w->x1 - w->x0), (int)(w->y1 - w->y0),
                            g.c_shadow);
    }
    for (int i = 0; i < g.n_walls; i++) {
        const Wall *w = &g.walls[i];
        const int x = (int)w->x0, y = (int)w->y0, ww = (int)(w->x1 - w->x0), hh = (int)(w->y1 - w->y0);
        T->canvas_fill_rect(g.cv, x, y, ww, hh, g.c_wall);
        T->canvas_fill_rect(g.cv, x, y, ww, 1, g.c_wall_lit);
        T->canvas_fill_rect(g.cv, x, y, 1, hh, g.c_wall_lit);
        T->canvas_fill_rect(g.cv, x, y + hh - 1, ww, 1, g.c_wall_dark);
        T->canvas_fill_rect(g.cv, x + ww - 1, y, 1, hh, g.c_wall_dark);
    }

    // HUD lives outside the board: level at the top, best at the bottom.
    char buf[24];
    snprintf(buf, sizeof(buf), "LEVEL %d", g.level);
    T->canvas_text_centered(g.cv, g.W / 2, 9, buf, g.c_white, 1, true);
    snprintf(buf, sizeof(buf), "BEST %d", g.best);
    T->canvas_text_centered(g.cv, g.W / 2, 224, buf, g.c_dim, 1, true);

    memcpy(g.board, T->canvas_pixels(g.cv), (size_t)g.W * g.H);
}

static void draw_lives(void)
{
    // Spare balls, tucked beside the level label.
    for (int i = 0; i < g.lives; i++)
        T->canvas_sprite_scaled(g.cv, g.ball, 0, (float)(g.W / 2 + 34 + i * 8), 12, 0.4f, 0.4f, false);
}

static void draw_ball(float scale)
{
    const float r = g.br * scale;
    if (r < 0.5f) return;
    fill_disc(g.bx + 1.2f, g.by + 1.6f, r, g.c_wall_dark);   /* shadow on the wood */
    T->canvas_sprite_scaled(g.cv, g.ball, 0, g.bx, g.by + r, r / 8, r / 8, false);
}

// The C++ version passed its second line straight through as the banner's middle line and
// never used the third, so this keeps the same two.
static void banner(const char *top, const char *mid, uint8_t col)
{
    const tat_banner_t b = {
        .top = top, .mid = mid, .bottom = NULL,
        .top_color = col, .mid_color = g.c_label, .bottom_color = g.c_label,
        .panel = g.c_panel, .border = g.c_box,
    };
    T->canvas_banner(g.cv, g.W / 2, 100, g.W - 84, &b);
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

static void mz_begin(const tat_api_t *api)
{
    T = api;
    memset(&g, 0, sizeof(g));

    g.cv = T->canvas_create(SCALE, 0);
    if (!g.cv) {
        T->log("no memory for the canvas");
        return;
    }
    g.W = T->canvas_width(g.cv);
    g.H = g.W;
    g.cx = g.W / 2.0f;
    g.cy = g.H / 2.0f;

    g.board = T->alloc((size_t)g.W * g.H);
    if (!g.board) {
        T->log("no memory for the board");
        return;
    }

    g.c_black = T->canvas_color(g.cv, T->rgb(0, 0, 0));
    g.c_white = T->canvas_color(g.cv, T->rgb(255, 255, 255));
    g.c_floor = T->canvas_color(g.cv, T->rgb(201, 158, 98));
    g.c_floor2 = T->canvas_color(g.cv, T->rgb(188, 145, 86));
    g.c_plank = T->canvas_color(g.cv, T->rgb(170, 128, 72));
    g.c_grain = T->canvas_color(g.cv, T->rgb(212, 172, 112));
    g.c_wall = T->canvas_color(g.cv, T->rgb(104, 68, 34));
    g.c_wall_lit = T->canvas_color(g.cv, T->rgb(150, 104, 58));
    g.c_wall_dark = T->canvas_color(g.cv, T->rgb(70, 44, 20));
    g.c_shadow = T->canvas_color(g.cv, T->rgb(120, 90, 50));
    g.c_hole = T->canvas_color(g.cv, T->rgb(8, 6, 4));
    g.c_rim = T->canvas_color(g.cv, T->rgb(60, 40, 22));
    g.c_rim_lit = T->canvas_color(g.cv, T->rgb(226, 190, 130));
    g.c_start = T->canvas_color(g.cv, T->rgb(60, 170, 90));
    g.c_dim = T->canvas_color(g.cv, T->rgb(150, 150, 150));
    g.c_panel = T->canvas_color(g.cv, T->rgb(16, 18, 26));
    g.c_box = T->canvas_color(g.cv, T->rgb(90, 90, 100));
    g.c_go = T->canvas_color(g.cv, T->rgb(40, 200, 110));
    g.c_danger = T->canvas_color(g.cv, T->rgb(255, 40, 40));
    g.c_label = T->canvas_color(g.cv, T->rgb(225, 225, 225));
    g.c_steel = T->canvas_color(g.cv, T->rgb(206, 211, 219));

    bool ok = true;
    ok &= load_sheet(&g.ball, "ball.png", 16, 16);
    ok &= load_sheet(&g.flag, "flag.png", 12, 12);
    if (!ok) {
        // Without the ball there is nothing to roll, so let go of the board: update() and
        // draw() both stand down while it is NULL.
        T->log("some sprites failed to load");
        T->free(g.board);
        g.board = NULL;
        return;
    }

    g.best = 1;   /* the first level you have never reached is 1, not 0 */
    T->save_get("best", &g.best, 0);
    new_game();
    T->log("ready, best %d", g.best);
}

static void mz_enter(void)
{
    g.redraw = true;
    // Coming back from the home screen mid-roll: pause rather than let it run blind.
    if (g.phase == PLAYING) T->menu_open();
}

static void mz_update(float dt)
{
    if (!g.board) return;
    if (dt > 1.0f / 30) dt = 1.0f / 30;

    const tat_input_t *in = T->input();
    const tat_gestures_t *ges = T->gestures();
    g.phase_t += dt;

    if (T->menu_is_open()) {
        switch (T->menu_update()) {
        case 0: level_tap(); break;
        case 1: T->menu_toggle_sound(); break;
        case 2:
            new_game();   /* back to level 1 with three balls */
            T->menu_close();
            break;
        }
        return;
    }
    if (ges->swipe_left && (g.phase == PLAYING || g.phase == READY)) {
        T->menu_open();
        return;
    }

    switch (g.phase) {
    case READY:
        if (ges->tap) begin_play();
        break;
    case PLAYING:
        step_ball(in, dt);
        break;
    case FALLING:
        // The ball slides to the middle of the hole and drops away.
        g.bx += (g.fall_x - g.bx) * minf(1.0f, dt * 14);
        g.by += (g.fall_y - g.by) * minf(1.0f, dt * 14);
        if (g.phase_t > 0.9f) {
            if (g.lives <= 0) {
                g.phase = GAME_OVER;
                g.phase_t = 0;
                g.redraw = true;
                sfx_game_over();
            } else {
                place_ball();
                g.phase = READY;
                g.phase_t = 0;
                g.redraw = true;
            }
        }
        break;
    case CLEARED:
        if (g.phase_t > 1.4f) {
            g.level++;
            new_level();
        }
        break;
    case GAME_OVER:
        if (ges->tap && g.phase_t > 0.6f) new_game();
        break;
    }
}

static void mz_draw(void)
{
    if (!g.board) return;

    if (T->menu_is_open()) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", g.best);
        const tat_menu_row_t rows[] = {
            {"LEVEL", g.tilt_nx == 0 && g.tilt_ny == 0 ? "FLAT" : "CUSTOM", T->ui_color(TAT_UI_ACCENT)},
            T->menu_sound_row(),
            {"RESTART GAME", "GO", T->ui_color(TAT_UI_ACCENT)},
            {"BEST LEVEL", buf, T->ui_color(TAT_UI_LABEL)},
        };
        T->menu_draw(rows, 4, "PAUSED");
        return;
    }

    if (g.redraw) {
        draw_board();
        g.redraw = false;
    }
    memcpy(T->canvas_pixels(g.cv), g.board, (size_t)g.W * g.H);
    draw_lives();
    draw_ball(g.phase == FALLING ? clampf(1.0f - g.phase_t / 0.45f, 0.0f, 1.0f) : 1.0f);

    char buf[32];
    switch (g.phase) {
    case READY: banner("TAP TO START", "TILT TO ROLL", g.c_white); break;
    case CLEARED: banner("LEVEL CLEAR!", NULL, g.c_go); break;
    case GAME_OVER:
        snprintf(buf, sizeof(buf), "REACHED LEVEL %d", g.level);
        banner("GAME OVER", buf, g.c_danger);
        break;
    default: break;
    }

    T->canvas_present(g.cv);
}

static bool mz_keep_awake(void)
{
    // A marble game has no button presses, so count a rolling ball as "in use",
    // but not a watch left on the table with the ball at rest.
    return !T->menu_is_open() && g.phase == PLAYING && g.still_t < 20.0f;
}

static void mz_unload(void)
{
    if (g.board) T->free(g.board);
    g.board = NULL;
    if (g.cv) T->canvas_destroy(g.cv);
    g.cv = NULL;
}

const tat_game_t tat_game = {
    .magic = TAT_GAME_MAGIC,
    .api_major = TAT_API_MAJOR,
    .api_minor = TAT_API_MINOR,
    .id = "maze",   /* also the save namespace, so the player's best level survives the port */
    .name = "MARBLE MAZE",
    .accent_r = 222, .accent_g = 178, .accent_b = 112,
    .assets = tat_assets,
    .asset_count = 2,
    .begin = mz_begin,
    .enter = mz_enter,
    .update = mz_update,
    .draw = mz_draw,
    .leave = NULL,
    .unload = mz_unload,
    .redraw = NULL,   /* draw() composites and presents a frame every time it is called */
    .keep_awake = mz_keep_awake,
};
