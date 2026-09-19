// ECHO - watch the pads light up, then touch them in the same order. One more each round.
//
// The oldest electronic memory game there is, on a round screen. The watch shows a
// pattern; then it is your turn; you touch the pads to play it back. Get it right and the
// pattern grows by one. Get to round 20 and you have won. Touch a wrong pad, or take too
// long, and you have lost.
//
//   tap a pad   - play it
//   swipe left  - pause menu
//
// That is all of it, on purpose. An earlier version pressed pads by tipping the watch
// toward them, with lives and a strict mode on top; it was clever and it was no fun. A
// memory game is about the pattern, and anything between the player's finger and the pad
// is in the way.
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

#define PADS 4
#define WIN_AT 20          /* rounds to win, as on the machines this comes from */
#define TURN_LIMIT_S 8.0f  /* how long a player may think about one press */

// The ring the pads are cut from, in canvas pixels.
#define R_HUB 38
#define R_IN 44
#define R_OUT 113
#define GAP 3.2f           /* half the width of the dark cross between pads */

enum { UP, RIGHT, DOWN, LEFT };
enum { READY, SHOW, INPUT, LOST, WON };

// What every canvas pixel is, worked out once: the picture never changes shape, only
// colour, so a frame is one pass over this map with a fourteen-entry table.
enum { RG_NONE, RG_PAD /* + pad*3: body, shade, shine */, RG_HUB = RG_PAD + PADS * 3, RG_HUB_RING, RG_COUNT };

static struct {
    tat_canvas_t *cv;
    uint8_t *map;

    uint8_t c_bg, c_hub, c_hub_ring, c_text, c_dim, c_panel, c_accent, c_danger, c_go;
    uint8_t c_pad[PADS][2][3];   // [pad][lit][body, shade, shine]

    uint8_t run[WIN_AT];
    int len;          // how many pads this round plays
    int at;           // where playback, or the player, has got to
    int best;

    int phase;
    float phase_t;
    int lit;          // the pad showing lit, or -1
    float lit_t;      // how much longer it stays lit
    float idle_t;     // how long the player has taken over this press
    bool dirty;
} g;

// ---------------------------------------------------------------- sound

static const float NOTE[PADS] = {523.25f, 392.00f, 329.63f, 261.63f};   // C5 G4 E4 C4: up is high

static void tone1(float f0, float f1, int ms, int wave, float vol, int delay)
{
    const tat_tone_t t = {.f0 = f0, .f1 = f1, .ms = (uint16_t)ms, .wave = (uint8_t)wave, .volume = vol,
                          .delay_ms = (uint16_t)delay};
    T->tone(&t);
}
static void sfx_pad(int pad, int ms) { tone1(NOTE[pad], 0, ms, TAT_TRIANGLE, 0.7f, 0); }
static void sfx_round(void)
{
    tone1(659, 0, 70, TAT_TRIANGLE, 0.45f, 0);
    tone1(880, 0, 110, TAT_TRIANGLE, 0.45f, 80);
}
static void sfx_won(void)
{
    static const float n[] = {523, 659, 784, 1047, 784, 1047};
    for (int i = 0; i < 6; i++) tone1(n[i], 0, i == 5 ? 320 : 110, TAT_SQUARE, 0.5f, i * 120);
}
static void sfx_lost(void)
{
    tone1(110, 82, 420, TAT_SQUARE, 0.6f, 0);
    tone1(60, 0, 200, TAT_NOISE, 0.35f, 0);
    tone1(233, 0, 360, TAT_SQUARE, 0.5f, 460);
}

// ---------------------------------------------------------------- the rounds

// Playback quickens as the pattern grows, the way the old machines did, but never so far
// that the pads stop reading as separate.
static float step_s(void)
{
    const float s = 0.62f - 0.016f * (float)g.len;
    return s < 0.30f ? 0.30f : s;
}

static void light(int pad, float seconds)
{
    g.lit = pad;
    g.lit_t = seconds;
    g.dirty = true;
}

static void set_phase(int phase)
{
    g.phase = phase;
    g.phase_t = 0;
    g.dirty = true;
}

static void begin_show(float pause_s)
{
    g.at = 0;
    g.lit = -1;
    set_phase(SHOW);
    g.phase_t = -pause_s;   // a breath before the pattern plays
}

static void new_game(void)
{
    g.len = 1;
    for (int i = 0; i < WIN_AT; i++) g.run[i] = (uint8_t)(T->random() % PADS);
    g.lit = -1;
    set_phase(READY);
}

static void lose(void)
{
    T->log("lost on round %d, best %d", g.len, g.best);
    sfx_lost();
    light(g.run[g.at], 1.2f);   // the one it should have been
    set_phase(LOST);
}

static void press(int pad)
{
    g.idle_t = 0;
    if (pad != g.run[g.at]) {
        lose();
        return;
    }
    sfx_pad(pad, 220);
    light(pad, 0.22f);
    if (++g.at < g.len) return;

    // The whole pattern, right. The score is rounds completed.
    T->log("round %d done", g.len);
    if (g.len > g.best) {
        g.best = g.len;
        T->save_set("best", g.best);
    }
    if (g.len == WIN_AT) {
        sfx_won();
        set_phase(WON);
        return;
    }
    sfx_round();
    g.len++;
    begin_show(0.7f);
}

// Which pad a point on the screen belongs to, or -1.
static int pad_at(int sx, int sy)
{
    const int x = sx / SCALE, y = sy / SCALE;
    if (x < 0 || y < 0 || x >= CW || y >= CW) return -1;
    const int r = g.map[y * CW + x];
    return (r >= RG_PAD && r < RG_HUB) ? (r - RG_PAD) / 3 : -1;
}

// ---------------------------------------------------------------- the picture

static void build_map(void)
{
    const float c = (CW - 1) * 0.5f;
    for (int y = 0; y < CW; y++) {
        for (int x = 0; x < CW; x++) {
            const float dx = (float)x - c, dy = (float)y - c;
            const float d = sqrtf(dx * dx + dy * dy);
            uint8_t r = RG_NONE;
            if (d <= R_HUB) {
                r = d > R_HUB - 2.5f ? RG_HUB_RING : RG_HUB;
            } else if (d >= R_IN && d <= R_OUT) {
                // The pads are the four quarters between the diagonals, with a dark cross
                // of constant width where they meet.
                const float ax = fabsf(dx), ay = fabsf(dy);
                if (fabsf(ax - ay) * 0.7071f > GAP) {
                    const int pad = ay > ax ? (dy < 0 ? UP : DOWN) : (dx > 0 ? RIGHT : LEFT);
                    // A bevel: a shine along the inner edge, a shade along the outer.
                    const int part = d > R_OUT - 4.0f ? 1 : d < R_IN + 2.5f ? 2 : 0;
                    r = (uint8_t)(RG_PAD + pad * 3 + part);
                }
            }
            g.map[y * CW + x] = r;
        }
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
    const int lines = 1 + (mid != NULL) + (bottom != NULL);
    T->canvas_banner(g.cv, CC, CC - 3 - lines * 6, CW - 50, &b);
}

static void draw_pads(void)
{
    uint8_t color[RG_COUNT];
    color[RG_NONE] = g.c_bg;
    color[RG_HUB] = g.c_hub;
    color[RG_HUB_RING] = g.lit >= 0 ? g.c_pad[g.lit][1][0] : g.c_hub_ring;
    for (int p = 0; p < PADS; p++)
        for (int part = 0; part < 3; part++) color[RG_PAD + p * 3 + part] = g.c_pad[p][g.lit == p][part];

    uint8_t *px = T->canvas_pixels(g.cv);
    const int n = CW * CW;
    for (int i = 0; i < n; i++) px[i] = color[g.map[i]];
}

// The hub says whose go it is, in one word, and which round this is. Nothing else in the
// game needs explaining once that is on screen.
static void draw_hub(void)
{
    char buf[8];
    snprintf(buf, sizeof(buf), "%d", g.len);
    T->canvas_text_centered(g.cv, CC, CC - 6, buf, g.c_text, 4, true);
    if (g.phase == SHOW) T->canvas_text_centered(g.cv, CC, CC + 20, "WATCH", g.c_dim, 1, true);
    else if (g.phase == INPUT) T->canvas_text_centered(g.cv, CC, CC + 20, "YOUR GO", g.c_go, 1, true);
}

// ---------------------------------------------------------------- the game

static uint8_t mix(uint8_t a, float k)
{
    const float v = (float)a * k;
    return (uint8_t)(v > 255 ? 255 : v);
}

static uint8_t lift(uint8_t a) { return (uint8_t)(a + (255 - a) * 0.5f); }   // half way to white

static void ec_begin(const tat_api_t *api)
{
    T = api;
    memset(&g, 0, sizeof(g));
    g.lit = -1;
    g.cv = T->canvas_create(SCALE, 0);
    g.map = T->alloc((size_t)CW * CW);
    if (!g.cv || !g.map) {
        T->log("no memory for the pads");
        return;
    }
    build_map();

    g.c_bg = T->canvas_color(g.cv, T->rgb(8, 9, 16));
    g.c_hub = T->canvas_color(g.cv, T->rgb(18, 20, 32));
    g.c_hub_ring = T->canvas_color(g.cv, T->rgb(52, 58, 84));
    g.c_text = T->canvas_color(g.cv, T->rgb(246, 248, 255));
    g.c_dim = T->canvas_color(g.cv, T->rgb(140, 148, 180));
    g.c_panel = T->canvas_color(g.cv, T->rgb(10, 12, 22));
    g.c_accent = T->canvas_color(g.cv, T->rgb(255, 214, 61));
    g.c_danger = T->canvas_color(g.cv, T->rgb(255, 84, 92));
    g.c_go = T->canvas_color(g.cv, T->rgb(60, 220, 120));

    // Up green, right red, down blue, left yellow. Unlit they are dark enough that a lit
    // one is unmistakable out of the corner of an eye, which is how this game is played.
    static const uint8_t HUE[PADS][3] = {{60, 220, 110}, {255, 72, 84}, {70, 140, 255}, {255, 206, 60}};
    for (int p = 0; p < PADS; p++) {
        const uint8_t *h = HUE[p];
        g.c_pad[p][0][0] = T->canvas_color(g.cv, T->rgb(mix(h[0], 0.30f), mix(h[1], 0.30f), mix(h[2], 0.30f)));
        g.c_pad[p][0][1] = T->canvas_color(g.cv, T->rgb(mix(h[0], 0.18f), mix(h[1], 0.18f), mix(h[2], 0.18f)));
        g.c_pad[p][0][2] = T->canvas_color(g.cv, T->rgb(mix(h[0], 0.46f), mix(h[1], 0.46f), mix(h[2], 0.46f)));
        g.c_pad[p][1][0] = T->canvas_color(g.cv, T->rgb(h[0], h[1], h[2]));
        g.c_pad[p][1][1] = T->canvas_color(g.cv, T->rgb(mix(h[0], 0.72f), mix(h[1], 0.72f), mix(h[2], 0.72f)));
        g.c_pad[p][1][2] = T->canvas_color(g.cv, T->rgb(lift(h[0]), lift(h[1]), lift(h[2])));
    }

    T->save_get("best", &g.best, 0);
    new_game();
    T->log("ready, best %d", g.best);
}

static void ec_enter(void) { g.dirty = true; }

static void ec_update(float dt)
{
    if (!g.cv || !g.map) return;
    if (dt > 1.0f / 20) dt = 1.0f / 20;

    const tat_input_t *in = T->input();
    const tat_gestures_t *ges = T->gestures();

    if (T->menu_is_open()) {
        switch (T->menu_update()) {
        case 0: T->menu_toggle_sound(); break;
        case 1:
            new_game();
            T->menu_close();
            break;
        }
        if (!T->menu_is_open()) g.dirty = true;
        return;
    }
    if (ges->swipe_left) {
        // Pausing while the pattern plays would be a way to study it, so it plays again.
        if (g.phase == SHOW || g.phase == INPUT) begin_show(0.6f);
        T->menu_open();
        return;
    }

    g.phase_t += dt;
    if (g.lit >= 0 && (g.lit_t -= dt) <= 0) {
        g.lit = -1;
        g.dirty = true;
    }

    switch (g.phase) {
    case READY:
        if (ges->tap) begin_show(0.4f);
        break;

    case SHOW: {
        if (g.phase_t < 0) break;
        const float step = step_s();
        const int due = (int)(g.phase_t / step);
        if (due >= g.len) {
            if (g.lit < 0) {
                g.at = 0;
                g.idle_t = 0;
                set_phase(INPUT);
            }
        } else if (due >= g.at) {
            // The next pad, never "the pad that is due": a slow frame must not skip one.
            sfx_pad(g.run[g.at], (int)(step * 1000 * 0.7f));
            light(g.run[g.at], step * 0.7f);
            g.at++;
        }
        break;
    }

    case INPUT:
        g.idle_t += dt;
        if (g.idle_t > TURN_LIMIT_S) {
            lose();
            break;
        }
        // On the touch itself, not on a finished tap: the pad should light and sound the
        // moment a finger lands, the way a real button would.
        if (in->touch.pressed) {
            const int pad = pad_at(in->touch.x, in->touch.y);
            if (pad >= 0) press(pad);
        }
        break;

    case LOST:
    case WON:
        if (g.phase_t > 1.0f && ges->tap) new_game();
        break;
    }
}

static void ec_draw(void)
{
    if (!g.cv || !g.map) return;

    if (T->menu_is_open()) {
        char best[16];
        snprintf(best, sizeof(best), "%d", g.best);
        const tat_menu_row_t rows[] = {
            T->menu_sound_row(),
            {"NEW GAME", "GO", T->ui_color(TAT_UI_ACCENT)},
            {"BEST", best, T->ui_color(TAT_UI_LABEL)},
        };
        T->menu_draw(rows, 3, "PAUSED");
        return;
    }
    // The losing banner waits for the pad it should have been to be seen first, so the
    // frame on which it is due has to be drawn even though nothing else changed.
    static bool lost_banner_up;
    const bool lost_banner = g.phase == LOST && g.phase_t > 0.9f;
    if (lost_banner != lost_banner_up) g.dirty = true;
    lost_banner_up = lost_banner;

    if (!g.dirty) return;
    g.dirty = false;

    draw_pads();
    draw_hub();

    char mid[32], bottom[32];
    if (g.phase == READY) {
        banner("WATCH THE PADS", "THEN TOUCH THEM IN ORDER", "TAP TO START", g.c_go);
    } else if (lost_banner) {
        snprintf(mid, sizeof(mid), "YOU GOT TO ROUND %d", g.len);
        snprintf(bottom, sizeof(bottom), "BEST %d   TAP TO PLAY", g.best);
        banner("YOU LOSE", mid, bottom, g.c_danger);
    } else if (g.phase == WON) {
        snprintf(mid, sizeof(mid), "ALL %d ROUNDS", WIN_AT);
        banner("YOU WIN!", mid, "TAP TO PLAY AGAIN", g.c_accent);
    }

    T->canvas_present(g.cv);
}

static void ec_redraw(void) { g.dirty = true; }

static bool ec_keep_awake(void) { return (g.phase == SHOW || g.phase == INPUT) && !T->menu_is_open(); }

static void ec_unload(void)
{
    if (g.cv) T->canvas_destroy(g.cv);
    if (g.map) T->free(g.map);
    g.cv = NULL;
    g.map = NULL;
}

const tat_game_t tat_game = {
    .magic = TAT_GAME_MAGIC,
    .api_major = TAT_API_MAJOR,
    .api_minor = TAT_API_MINOR,
    .id = "echo",
    .name = "ECHO",
    .accent_r = 60, .accent_g = 220, .accent_b = 120,
    .assets = NULL,
    .asset_count = 0,
    .begin = ec_begin,
    .enter = ec_enter,
    .update = ec_update,
    .draw = ec_draw,
    .leave = NULL,
    .unload = ec_unload,
    .redraw = ec_redraw,
    .keep_awake = ec_keep_awake,
};
