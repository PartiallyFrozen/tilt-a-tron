// SKY JUMP - tilt to steer Hopper up an endless tower of platforms.
//
// Bounce on springs, dodge the ledges that give way, shoot or stomp the monsters, and
// climb from a sunny morning all the way up into the stars.
//
//   tilt        - steer left / right
//   tap         - start, then hold to shoot straight up
//   swipe left  - pause menu (tilt sensitivity, sound, new game)
//   BOOT        - back to the home menu (console-wide)
//
// Written against tat_api.h alone: it includes nothing else from the console and calls
// nothing by name, which is what will let it become an installable file. The PNGs it
// draws with arrive through api->asset(), so it neither knows nor cares whether they were
// linked into the firmware or unpacked from a package.
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "tat/tat_api.h"

static const tat_api_t *T;

// ---------------------------------------------------------------- the world

// Everything below is in canvas pixels: a 156 x 156 picture the console scales 3x, so
// every sprite pixel is a chunky 3x3 block on the round screen.
#define CANVAS_SCALE 3
#define PI 3.14159265f

// World y points UP, and 0 is the ground.
#define GRAVITY 930.0f      /* px/s^2 */
#define JUMP_V 390.0f       /* a normal bounce reaches 82 px, about half the screen */
#define SPRING_V 700.0f     /* a spring reaches 263 px */
#define BULLET_V 370.0f
#define PLAYER_HALF 8.0f    /* half of Hopper's body, for landing and collisions */
#define PLAYER_H 22.0f
#define PLAT_W 30.0f
#define MONSTER_W 18.0f
#define MONSTER_H 16.0f
#define PLAYER_LINE 79      /* Hopper never rises past this screen row; the world scrolls */
#define PX_PER_M 4.0f       /* the score is height in "metres" */
#define SKY_STEPS 32        /* palette entries for the sky gradient */

// Caps instead of growing lists. A game is handed a fixed budget and has to live inside
// it: nothing here can be so busy that the numbers below are the thing you notice.
#define MAX_PLATS 40
#define MAX_MONSTERS 8
#define MAX_BULLETS 12
#define MAX_PUFFS 64
#define MAX_CLOUDS 6
#define MAX_STARS 70

static const float TILT_GAIN[3] = {320.0f, 470.0f, 650.0f};   /* px/s at full tilt */

typedef enum { NORMAL, MOVING, CRUMBLE, SPRING, GROUND } PType;

typedef struct {
    float x, y;                   /* centre x, top y, in world coordinates */
    float w;                      /* landing width */
    uint8_t type;
    float base_x, phase, speed;   /* MOVING swings about base_x */
    bool broken;                  /* CRUMBLE, after someone stepped on it */
    float fall_t;
} Plat;

typedef struct {
    float x, y;                   /* centre x, bottom y */
    float base_x, phase;
} Monster;

typedef struct {
    float x, y;
} Bullet;

typedef struct {
    float x, y, vx, vy, t;
    uint8_t c;
} Puff;

typedef struct {
    float x, y;                   /* y lives in the slow parallax layer */
    uint8_t kind;
} Cloud;

typedef struct {
    uint8_t x, y, size;
} Star;

typedef enum { READY, PLAYING, DEAD, GAME_OVER } Phase;

static struct {
    tat_canvas_t *cv;
    int W, H;                     /* the canvas, asked for rather than assumed */

    tat_sheet_t *hopper, *monster, *ledges, *spring, *shot, *clouds_sheet;

    /* palette */
    uint8_t sky0;                 /* first of SKY_STEPS gradient entries */
    uint8_t cloud_c, cloud_shade;
    uint8_t c_white, c_black, c_star, c_star2, c_dim, c_accent, c_danger;
    uint8_t c_panel, c_box, c_grass, c_tuft, c_dirt, c_speck, c_orange;
    uint8_t c_purple, c_stone;

    /* the run */
    Phase phase;
    float phase_t;
    int score, best;              /* metres */
    bool got_best;

    /* Hopper */
    float px, py;                 /* feet */
    float vx, vy;
    float prev_y;
    int facing;
    float squash;                 /* 1 right after a bounce, decays */
    float fire_t;
    float blink_t;
    float cam;                    /* world y at the bottom edge of the screen */
    float top_y;                  /* highest feet position so far */

    /* the tower */
    Plat plats[MAX_PLATS];
    int n_plats;
    Monster monsters[MAX_MONSTERS];
    int n_monsters;
    Bullet bullets[MAX_BULLETS];
    int n_bullets;
    Puff puffs[MAX_PUFFS];
    int n_puffs;
    Cloud clouds[MAX_CLOUDS];
    Star stars[MAX_STARS];
    float gen_y;                  /* the next platform goes about here */
    float next_monster_y;
    float t;                      /* run time, for animation */

    /* settings */
    int tilt_sens;                /* index into TILT_GAIN */

    /* housekeeping */
    int frames;
    int64_t fps_t0;
} g;

// ---------------------------------------------------------------- odds and ends

static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
static float lerp(float a, float b, float t) { return a + (b - a) * t; }
static uint32_t rnd(uint32_t n) { return T->random() % n; }
static float frand(void) { return (T->random() & 0xFFFF) / 65535.0f; }

static tat_color_t mix(uint8_t r0, uint8_t g0, uint8_t b0, uint8_t r1, uint8_t g1, uint8_t b1, float t)
{
    return T->rgb((uint8_t)lerp(r0, r1, t), (uint8_t)lerp(g0, g1, t), (uint8_t)lerp(b0, b1, t));
}

static float screen_y(float wy) { return g.H - (wy - g.cam); }

// ---------------------------------------------------------------- sound

static void tone1(float f0, float f1, uint16_t ms, uint8_t wave, float vol, uint16_t delay)
{
    const tat_tone_t t = {f0, f1, ms, wave, vol, delay};
    T->tone(&t);
}

static void sfx_boing(float pitch01)
{
    const float f = 260 + 140 * pitch01;
    tone1(f, f * 2.1f, 70, TAT_TRIANGLE, 0.6f, 0);
}
static void sfx_spring(void)
{
    tone1(300, 1500, 140, TAT_SQUARE, 0.55f, 0);
    tone1(900, 1800, 90, TAT_TRIANGLE, 0.5f, 60);
}
static void sfx_crumble(void)
{
    tone1(200, 90, 90, TAT_NOISE, 0.6f, 0);
    tone1(180, 60, 120, TAT_TRIANGLE, 0.4f, 0);
}
static void sfx_shoot(void) { tone1(1400, 700, 45, TAT_SQUARE, 0.35f, 0); }
static void sfx_stomp(void)
{
    tone1(160, 60, 110, TAT_SQUARE, 0.7f, 0);
    tone1(500, 1000, 90, TAT_TRIANGLE, 0.5f, 90);
}
static void sfx_pop(void)
{
    tone1(700, 1400, 60, TAT_SQUARE, 0.5f, 0);
    tone1(300, 100, 80, TAT_NOISE, 0.45f, 40);
}
static void sfx_hurt(void)
{
    tone1(400, 150, 200, TAT_SQUARE, 0.7f, 0);
    tone1(120, 60, 200, TAT_NOISE, 0.4f, 0);
}
static void sfx_fall(void) { tone1(900, 80, 600, TAT_TRIANGLE, 0.6f, 0); }
static void sfx_start(void) { tone1(500, 900, 100, TAT_TRIANGLE, 0.6f, 0); }
static void sfx_new_best(void)
{
    tone1(660, 0, 90, TAT_SQUARE, 0.6f, 0);
    tone1(880, 0, 90, TAT_SQUARE, 0.6f, 100);
    tone1(1100, 0, 90, TAT_SQUARE, 0.6f, 200);
    tone1(1320, 0, 260, TAT_SQUARE, 0.7f, 300);
}
static void sfx_game_over(void)
{
    tone1(392, 0, 160, TAT_SQUARE, 0.6f, 0);
    tone1(330, 0, 160, TAT_SQUARE, 0.6f, 160);
    tone1(262, 0, 320, TAT_SQUARE, 0.6f, 320);
}

// ---------------------------------------------------------------- building the tower

static float difficulty(void) { return clampf(g.top_y / PX_PER_M / 1800.0f, 0.0f, 1.0f); }

static void add_plat(float x, float y, PType type)
{
    if (g.n_plats >= MAX_PLATS) return;
    Plat *p = &g.plats[g.n_plats++];
    memset(p, 0, sizeof(*p));
    p->x = p->base_x = clampf(x, 20, g.W - 20);
    p->y = y;
    p->w = PLAT_W;
    p->type = (uint8_t)type;
    p->phase = frand() * 2 * PI;
    p->speed = 1.2f + frand() * 1.2f + difficulty() * 1.5f;
}

// Keep the tower going up to a screen-height above the camera.
static void generate(void)
{
    const float d = difficulty();
    while (g.gen_y < g.cam + g.H + 40) {
        // Stop short of full rather than dropping a platform on the floor: gen_y has not
        // moved, so the tower picks up again as soon as the cull below makes room.
        if (g.n_plats + 2 > MAX_PLATS) break;

        const float gap = lerp(17, 28, frand()) + d * lerp(10, 40, frand());
        g.gen_y += gap;
        const float r = frand();
        PType type = NORMAL;
        if (r < 0.06f + 0.02f * d) type = SPRING;
        else if (r < 0.12f + 0.30f * d) type = MOVING;
        add_plat(20 + frand() * (g.W - 40), g.gen_y, type);

        // Crumbling ledges are extras, never the only way up.
        if (frand() < 0.10f + 0.35f * d) add_plat(20 + frand() * (g.W - 40), g.gen_y + gap * 0.45f, CRUMBLE);

        if (g.gen_y > g.next_monster_y && g.top_y / PX_PER_M > 120 && g.n_monsters < MAX_MONSTERS) {
            Monster *m = &g.monsters[g.n_monsters++];
            m->x = m->base_x = 30 + frand() * (g.W - 60);
            m->y = g.gen_y + gap * 0.5f + 8;
            m->phase = frand() * 2 * PI;
            g.next_monster_y = g.gen_y + lerp(470, 230, d) + frand() * 200;
        }
    }
}

static void new_game(void)
{
    g.n_plats = g.n_monsters = g.n_bullets = g.n_puffs = 0;
    g.px = g.W / 2.0f;
    g.py = 0;
    g.prev_y = 0;
    g.vx = g.vy = 0;
    g.facing = 1;
    g.squash = 0;
    g.cam = -(g.H - 127);   /* the ground sits at canvas y 127 */
    g.top_y = 0;
    g.score = 0;
    g.got_best = false;
    g.t = 0;

    // Solid ground to start on, then the tower.
    Plat *ground = &g.plats[g.n_plats++];
    memset(ground, 0, sizeof(*ground));
    ground->x = ground->base_x = g.W / 2.0f;
    ground->y = 0;
    ground->w = g.W + 60.0f;
    ground->type = GROUND;

    g.gen_y = 0;
    g.next_monster_y = 830;
    generate();

    for (int i = 0; i < MAX_CLOUDS; i++) {
        g.clouds[i].x = frand() * g.W;
        g.clouds[i].y = g.cam * 0.35f + frand() * g.H;
        g.clouds[i].kind = (uint8_t)rnd(2);
    }
    for (int i = 0; i < MAX_STARS; i++) {
        g.stars[i].x = (uint8_t)rnd(g.W);
        g.stars[i].y = (uint8_t)rnd(g.H);
        g.stars[i].size = (uint8_t)(1 + (rnd(4) == 0));
    }
    g.phase = READY;
    g.phase_t = 0;
}

static void begin_play(void)
{
    g.phase = PLAYING;
    g.phase_t = 0;
    g.vy = JUMP_V;
    g.squash = 1;
    sfx_start();
}

// ---------------------------------------------------------------- simulation

static void spawn_puffs(float x, float y, int n, uint8_t c, float speed)
{
    for (int i = 0; i < n && g.n_puffs < MAX_PUFFS; i++) {
        const float a = frand() * 2 * PI, s = speed * (0.4f + frand() * 0.8f);
        Puff *q = &g.puffs[g.n_puffs++];
        q->x = x;
        q->y = y;
        q->vx = cosf(a) * s;
        q->vy = sinf(a) * s + speed * 0.3f;
        q->t = 0;
        q->c = c;
    }
}

static void die(void)
{
    g.phase = DEAD;
    g.phase_t = 0;
    g.vy = 170;   /* a little hop, then the long fall */
    sfx_hurt();
    spawn_puffs(g.px, g.py + PLAYER_H / 2, 10, g.c_orange, 90);
}

static void save_settings(void)
{
    T->save_set("best", g.best);
    T->save_set("tilt", g.tilt_sens);
}

static void step(const tat_input_t *in, float dt)
{
    g.t += dt;

    // Steering: tilt the watch left or right. Full speed at about 25 degrees.
    const float gain = TILT_GAIN[g.tilt_sens];
    const float target = clampf(in->tilt.ax / 0.42f, -1.0f, 1.0f) * gain;
    const float k = dt * 14 < 1.0f ? dt * 14 : 1.0f;
    g.vx += (target - g.vx) * k;
    if (fabsf(g.vx) > 20) g.facing = g.vx > 0 ? 1 : -1;
    g.px += g.vx * dt;
    if (g.px < -PLAYER_HALF) g.px += g.W + 2 * PLAYER_HALF;
    if (g.px > g.W + PLAYER_HALF) g.px -= g.W + 2 * PLAYER_HALF;

    g.prev_y = g.py;
    g.vy -= GRAVITY * dt;
    g.py += g.vy * dt;

    // Shooting: hold to keep firing.
    g.fire_t -= dt;
    if (g.phase == PLAYING && in->touch.down && g.fire_t <= 0 && g.n_bullets < MAX_BULLETS) {
        g.bullets[g.n_bullets].x = g.px;
        g.bullets[g.n_bullets].y = g.py + PLAYER_H * 0.8f;
        g.n_bullets++;
        g.fire_t = 0.22f;
        sfx_shoot();
    }

    // Platforms move; crumbled ones fall away.
    for (int i = 0; i < g.n_plats; i++) {
        Plat *p = &g.plats[i];
        if (p->type == MOVING) p->x = clampf(p->base_x + sinf(p->phase + g.t * p->speed) * 28, 17, g.W - 17);
        if (p->broken) {
            p->fall_t += dt;
            p->y -= 170 * p->fall_t * dt;
        }
    }
    for (int i = 0; i < g.n_monsters; i++)
        g.monsters[i].x = g.monsters[i].base_x + sinf(g.monsters[i].phase + g.t * 1.3f) * 13;

    // Landing: only while falling, and only through a platform's top.
    if (g.phase == PLAYING && g.vy < 0) {
        for (int i = 0; i < g.n_plats; i++) {
            Plat *p = &g.plats[i];
            if (p->broken) continue;
            if (g.prev_y < p->y || g.py > p->y) continue;
            if (fabsf(g.px - p->x) > p->w / 2 + PLAYER_HALF * 0.6f) continue;
            if (p->type == CRUMBLE) {
                p->broken = true;
                p->fall_t = 0;
                sfx_crumble();
                spawn_puffs(p->x, p->y, 8, g.c_stone, 60);
                continue;   /* no bounce: it gives way */
            }
            g.py = p->y;
            g.vy = p->type == SPRING ? SPRING_V : JUMP_V;
            g.squash = 1;
            if (p->type == SPRING) {
                sfx_spring();
                spawn_puffs(p->x, p->y, 6, g.c_white, 70);
            } else {
                sfx_boing(clampf((p->y - g.top_y) / 70.0f + 0.5f, 0, 1));
            }
            break;
        }

        for (int i = 0; i < g.n_monsters; i++) {
            Monster *m = &g.monsters[i];
            if (fabsf(g.px - m->x) > MONSTER_W / 2 + PLAYER_HALF * 0.6f) continue;
            const float m_top = m->y + MONSTER_H;
            if (g.prev_y >= m_top - 4 && g.py <= m_top && g.py >= m->y) {
                g.py = m_top;
                g.vy = JUMP_V * 1.15f;
                g.squash = 1;
                sfx_stomp();
                spawn_puffs(m->x, m->y + MONSTER_H / 2, 14, g.c_purple, 100);
                g.monsters[i] = g.monsters[--g.n_monsters];
                break;
            }
        }
    }

    // Running into one from the side or below.
    if (g.phase == PLAYING) {
        for (int i = 0; i < g.n_monsters; i++) {
            const Monster *m = &g.monsters[i];
            if (fabsf(g.px - m->x) < MONSTER_W / 2 + PLAYER_HALF * 0.7f && g.py + PLAYER_H * 0.8f > m->y + 3 &&
                g.py < m->y + MONSTER_H - 3) {
                die();
                break;
            }
        }
    }

    // Bullets.
    for (int i = 0; i < g.n_bullets;) {
        Bullet *b = &g.bullets[i];
        b->y += BULLET_V * dt;
        bool gone = screen_y(b->y) < -4;
        for (int j = 0; j < g.n_monsters; j++) {
            Monster *m = &g.monsters[j];
            if (fabsf(b->x - m->x) < MONSTER_W / 2 && b->y > m->y && b->y < m->y + MONSTER_H) {
                gone = true;
                sfx_pop();
                spawn_puffs(m->x, m->y + MONSTER_H / 2, 14, g.c_purple, 100);
                g.monsters[j] = g.monsters[--g.n_monsters];
                break;
            }
        }
        if (gone) g.bullets[i] = g.bullets[--g.n_bullets];
        else i++;
    }

    // Particles.
    for (int i = 0; i < g.n_puffs;) {
        Puff *q = &g.puffs[i];
        q->t += dt;
        q->vy -= 300 * dt;
        q->x += q->vx * dt;
        q->y += q->vy * dt;
        if (q->t > 0.6f) g.puffs[i] = g.puffs[--g.n_puffs];
        else i++;
    }

    g.squash = g.squash - dt * 7 > 0 ? g.squash - dt * 7 : 0;
    g.blink_t -= dt;
    if (g.blink_t < -3.0f - frand() * 3) g.blink_t = 0.12f;

    // The camera follows Hopper up, never down.
    if (g.phase == PLAYING) {
        const float limit = g.py - (g.H - PLAYER_LINE);
        if (limit > g.cam) g.cam = limit;
        if (g.py > g.top_y) g.top_y = g.py;
        g.score = (int)(g.top_y / PX_PER_M);
        if (g.score > g.best) {
            if (!g.got_best && g.best > 0) sfx_new_best();
            g.got_best = true;
            g.best = g.score;
        }
        generate();
    }

    // Tidy up whatever scrolled off the bottom.
    for (int i = 0; i < g.n_plats;) {
        if (g.plats[i].y < g.cam - 30 || g.plats[i].fall_t > 1.0f) g.plats[i] = g.plats[--g.n_plats];
        else i++;
    }
    for (int i = 0; i < g.n_monsters;) {
        if (g.monsters[i].y + MONSTER_H < g.cam - 15) g.monsters[i] = g.monsters[--g.n_monsters];
        else i++;
    }

    // Fell off the bottom of the screen.
    if (g.py < g.cam - 30) {
        if (g.phase == PLAYING) sfx_fall();
        g.phase = GAME_OVER;
        g.phase_t = 0;
        if (g.got_best) save_settings();
        sfx_game_over();
    }
}

// ---------------------------------------------------------------- drawing

static void draw_sky(void)
{
    // Morning at the bottom of the tower, deep space at the top. The gradient is
    // SKY_STEPS palette entries, so changing it every frame costs nothing.
    const float alt = clampf(g.cam / PX_PER_M / 2600.0f, 0.0f, 1.0f);
    for (int i = 0; i < SKY_STEPS; i++) {
        const float k = (float)i / (SKY_STEPS - 1);   /* 0 = top of the screen */
        T->canvas_set_color(g.cv, (uint8_t)(g.sky0 + i),
                            mix(72 + (int)(78 * k), 140 + (int)(75 * k), 235 + (int)(20 * k),
                                6 + (int)(34 * k), 6 + (int)(18 * k), 24 + (int)(56 * k), alt));
    }
    uint8_t *row = T->canvas_pixels(g.cv);
    for (int y = 0; y < g.H; y++, row += g.W) memset(row, g.sky0 + y * SKY_STEPS / g.H, g.W);

    if (alt > 0.35f) {
        const int n = (int)((alt - 0.35f) / 0.65f * MAX_STARS);
        const int drift = (int)(g.cam * 0.08f);
        for (int i = 0; i < n; i++) {
            const Star *s = &g.stars[i];
            const int y = ((s->y + drift) % g.H + g.H) % g.H;
            const bool twinkle = ((i * 7 + (int)(g.t * 3)) % 11) == 0;
            T->canvas_fill_rect(g.cv, s->x, y, s->size, s->size,
                                twinkle ? g.c_star2 : alt > 0.7f ? g.c_white : g.c_star);
        }
    }
    if (alt < 0.85f) {
        T->canvas_set_color(g.cv, g.cloud_c, mix(240, 248, 255, 150, 130, 200, alt));
        T->canvas_set_color(g.cv, g.cloud_shade, mix(200, 215, 240, 110, 90, 160, alt));
        for (int i = 0; i < MAX_CLOUDS; i++) {
            Cloud *cl = &g.clouds[i];
            // Clouds live in a slower layer, so they drift past as you climb.
            float sy = g.H - (cl->y - g.cam * 0.35f);
            if (sy > g.H + 20) {
                cl->y += g.H + 40;
                cl->x = frand() * g.W;
                cl->kind = (uint8_t)rnd(2);
                sy = g.H - (cl->y - g.cam * 0.35f);
            }
            if (sy < -20) continue;
            T->canvas_sprite(g.cv, g.clouds_sheet, cl->kind, (int)cl->x - 10, (int)sy - 8, false);
        }
    }
}

static void draw_ground(const Plat *p)
{
    const int sy = (int)screen_y(p->y);
    if (sy > g.H) return;
    // Grass on top, dirt all the way down.
    T->canvas_fill_rect(g.cv, 0, sy, g.W, 1, g.c_tuft);
    T->canvas_fill_rect(g.cv, 0, sy + 1, g.W, 3, g.c_grass);
    T->canvas_fill_rect(g.cv, 0, sy + 4, g.W, g.H - sy - 4 > 0 ? g.H - sy - 4 : 0, g.c_dirt);
    for (int x = 2; x < g.W; x += 10) T->canvas_fill_rect(g.cv, x, sy + 7 + (x * 13) % 14, 2, 1, g.c_speck);
    for (int x = 1; x < g.W; x += 8) T->canvas_fill_rect(g.cv, x, sy - 2, 1, 2, g.c_tuft);
}

static void draw_plat(const Plat *p)
{
    if (p->type == GROUND) {
        draw_ground(p);
        return;
    }
    const int sy = (int)(screen_y(p->y) + 0.5f);
    if (sy < -10 || sy > g.H + 10) return;
    const int frame = p->type == MOVING ? 1 : p->type == CRUMBLE ? 2 : 0;
    const int x = (int)(p->x + 0.5f) - 15;
    T->canvas_sprite(g.cv, g.ledges, frame, x, sy, false);
    if (p->type == SPRING) T->canvas_sprite(g.cv, g.spring, 0, x + 9, sy - 6, false);
}

static void draw_monster(const Monster *m)
{
    const float bob = sinf(g.t * 5 + m->phase);
    const float sy = screen_y(m->y);
    if (sy < -20 || sy > g.H + 20) return;
    // It breathes - a little wider as it squats - and snaps its mouth now and then.
    const int frame = fmodf(g.t * 2 + m->phase, 4.0f) < 0.5f ? 1 : 0;
    T->canvas_sprite_scaled(g.cv, g.monster, frame, m->x, sy, 1 + 0.06f * bob, 1 - 0.06f * bob, g.px < m->x);
}

static void draw_hopper(void)
{
    const float sy = screen_y(g.py);
    if (sy < -30 || sy > g.H + 40) return;
    // Squash on landing, stretch when moving fast.
    const float q = g.squash * g.squash;
    float sx = 1 + 0.35f * q, syq = 1 - 0.30f * q;
    const float stretch = clampf(fabsf(g.vy) / 900.0f, 0, 0.18f);
    sx -= stretch * 0.5f;
    syq += stretch;
    const bool dead = g.phase == DEAD || g.phase == GAME_OVER;
    const int frame = dead ? 2 : g.blink_t > 0 ? 1 : 0;
    T->canvas_sprite_scaled(g.cv, g.hopper, frame, g.px, sy, sx, syq, g.facing < 0);
}

static void draw_hud(void)
{
    char buf[24];
    snprintf(buf, sizeof(buf), "%d M", g.score);
    T->canvas_text_centered(g.cv, g.W / 2 + 1, 12, buf, g.c_black, 1, true);
    T->canvas_text_centered(g.cv, g.W / 2, 11, buf, g.c_white, 1, true);
    if (g.best > 0) {
        snprintf(buf, sizeof(buf), "BEST %d M", g.best);
        T->canvas_text_centered(g.cv, g.W / 2 + 1, 143, buf, g.c_black, 1, true);
        T->canvas_text_centered(g.cv, g.W / 2, 142, buf, g.got_best ? g.c_accent : g.c_dim, 1, true);
    }
}

static void banner(const char *top, const char *mid, const char *bottom, uint8_t col)
{
    const tat_banner_t b = {
        .top = top, .mid = mid, .bottom = bottom,
        .top_color = col, .mid_color = g.c_dim, .bottom_color = g.c_dim,
        .panel = g.c_panel, .border = g.c_box,
    };
    T->canvas_banner(g.cv, g.W / 2, 68, g.W - 44, &b);
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

static void jump_begin(const tat_api_t *api)
{
    T = api;
    memset(&g, 0, sizeof(g));

    g.cv = T->canvas_create(CANVAS_SCALE, 0);
    if (!g.cv) {
        T->log("no memory for the canvas");
        return;
    }
    g.W = T->canvas_width(g.cv);
    g.H = g.W;
    g.facing = 1;

    // Fixed colours first, so they get stable indices.
    g.c_black = T->canvas_color(g.cv, T->rgb(0, 0, 0));
    g.c_white = T->canvas_color(g.cv, T->rgb(255, 255, 255));
    g.c_star = T->canvas_color(g.cv, T->rgb(200, 205, 230));
    g.c_star2 = T->canvas_color(g.cv, T->rgb(255, 240, 200));
    g.c_dim = T->canvas_color(g.cv, T->rgb(220, 230, 245));
    g.c_accent = T->canvas_color(g.cv, T->rgb(255, 217, 61));
    g.c_danger = T->canvas_color(g.cv, T->rgb(255, 70, 70));
    g.c_panel = T->canvas_color(g.cv, T->rgb(16, 18, 26));
    g.c_box = T->canvas_color(g.cv, T->rgb(90, 90, 100));
    g.c_grass = T->canvas_color(g.cv, T->rgb(80, 200, 95));
    g.c_tuft = T->canvas_color(g.cv, T->rgb(170, 245, 150));
    g.c_dirt = T->canvas_color(g.cv, T->rgb(150, 100, 55));
    g.c_speck = T->canvas_color(g.cv, T->rgb(110, 70, 40));
    g.c_orange = T->canvas_color(g.cv, T->rgb(255, 185, 40));
    g.c_purple = T->canvas_color(g.cv, T->rgb(175, 65, 210));
    g.c_stone = T->canvas_color(g.cv, T->rgb(130, 130, 140));
    g.sky0 = T->canvas_reserve(g.cv, SKY_STEPS);

    bool ok = true;
    ok &= load_sheet(&g.hopper, "hopper.png", 20, 22);
    ok &= load_sheet(&g.monster, "monster.png", 18, 16);
    ok &= load_sheet(&g.ledges, "ledges.png", 30, 7);
    ok &= load_sheet(&g.spring, "spring.png", 12, 7);
    ok &= load_sheet(&g.shot, "shot.png", 4, 4);
    ok &= load_sheet(&g.clouds_sheet, "clouds.png", 20, 8);
    // The cloud colours are tinted per frame; these are the sheet's own entries.
    g.cloud_c = T->canvas_color(g.cv, T->rgb(240, 248, 255));
    g.cloud_shade = T->canvas_color(g.cv, T->rgb(200, 215, 240));
    if (!ok) T->log("some sprites failed to load");

    T->save_get("best", &g.best, 0);
    T->save_get("tilt", &g.tilt_sens, 3);
    new_game();
    T->log("ready, best %d m", g.best);
}

static void jump_enter(void)
{
    // Coming back from the home screen mid-run: pause rather than let it run blind.
    if (g.phase == PLAYING) T->menu_open();
    g.fps_t0 = T->now_us();
    g.frames = 0;
}

static void jump_update(float dt)
{
    if (!g.cv) return;
    if (dt > 1.0f / 30) dt = 1.0f / 30;

    const tat_input_t *in = T->input();
    const tat_gestures_t *ges = T->gestures();
    g.phase_t += dt;

    if (T->menu_is_open()) {
        switch (T->menu_update()) {
        case 0:
            g.tilt_sens = (g.tilt_sens + 1) % 3;
            save_settings();
            T->menu_invalidate();
            break;
        case 1: T->menu_toggle_sound(); break;
        case 2:
            new_game();
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
    case DEAD:
        step(in, dt);
        break;
    case GAME_OVER:
        if (ges->tap && g.phase_t > 0.7f) new_game();
        break;
    }
}

static void jump_draw(void)
{
    if (!g.cv) return;

    if (T->menu_is_open()) {
        static const char *kSens[] = {"LOW", "MED", "HIGH"};
        char buf[16];
        snprintf(buf, sizeof(buf), "%d M", g.best);
        const tat_menu_row_t rows[] = {
            {"TILT", kSens[g.tilt_sens], 0},
            T->menu_sound_row(),
            {"RESTART GAME", "GO", T->ui_color(TAT_UI_ACCENT)},
            {"BEST", buf, T->ui_color(TAT_UI_LABEL)},
        };
        T->menu_draw(rows, 4, "PAUSED");
        return;
    }

    draw_sky();
    for (int i = 0; i < g.n_plats; i++) draw_plat(&g.plats[i]);
    for (int i = 0; i < g.n_monsters; i++) draw_monster(&g.monsters[i]);
    for (int i = 0; i < g.n_bullets; i++)
        T->canvas_sprite(g.cv, g.shot, 0, (int)g.bullets[i].x - 2, (int)screen_y(g.bullets[i].y) - 2, false);
    for (int i = 0; i < g.n_puffs; i++) {
        const Puff *q = &g.puffs[i];
        int r = (int)(2.5f * (1 - q->t / 0.6f));
        if (r < 1) r = 1;
        T->canvas_fill_rect(g.cv, (int)q->x - r / 2, (int)screen_y(q->y) - r / 2, r, r, q->c);
    }
    draw_hopper();
    draw_hud();

    char buf[32];
    if (g.phase == READY) {
        banner("TAP TO JUMP", "TILT TO STEER", "TAP TO SHOOT", g.c_white);
    } else if (g.phase == GAME_OVER && g.phase_t > 0.5f) {
        snprintf(buf, sizeof(buf), g.got_best ? "NEW BEST %d M!" : "%d M", g.score);
        banner("GAME OVER", buf, NULL, g.got_best ? g.c_accent : g.c_danger);
    }

    T->canvas_present(g.cv);

    g.frames++;
    const int64_t now = T->now_us();
    if (now - g.fps_t0 > 5000000) {
        T->log("%.1f fps", g.frames * 1e6f / (float)(now - g.fps_t0));
        g.frames = 0;
        g.fps_t0 = now;
    }
}

static bool jump_keep_awake(void) { return !T->menu_is_open() && (g.phase == PLAYING || g.phase == DEAD); }

static void jump_unload(void)
{
    if (g.cv) T->canvas_destroy(g.cv);
    g.cv = NULL;
}

const tat_game_t tat_game = {
    .magic = TAT_GAME_MAGIC,
    .api_major = TAT_API_MAJOR,
    .api_minor = TAT_API_MINOR,
    .id = "jump",   /* also the save namespace, so old best scores survive the port */
    .name = "SKY JUMP",
    .accent_r = 255, .accent_g = 190, .accent_b = 50,
    .assets = tat_assets,
    .asset_count = 6,
    .begin = jump_begin,
    .enter = jump_enter,
    .update = jump_update,
    .draw = jump_draw,
    .leave = NULL,
    .unload = jump_unload,
    .redraw = NULL,   /* every frame is drawn from scratch, so there is nothing to repeat */
    .keep_awake = jump_keep_awake,
};
