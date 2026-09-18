#include "games/jump.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "audio/audio.h"
#include "console/ui.h"
#include "engine/gestures.h"
#include "esp_log.h"
#include "esp_random.h"
#include "nvs.h"

using namespace wc;

namespace games {

namespace {

const char *TAG = "jump";

constexpr int W = Gfx::W, H = Gfx::H;
constexpr float PI = 3.14159265f;

// Everything is pixel art drawn at 3x, so one sprite pixel is a 3x3 block on screen.
constexpr int SCALE = 3;

// World units are screen pixels; world y points UP (0 = the ground).
constexpr float GRAVITY = 2800.0f;    // px/s^2
constexpr float JUMP_V = 1170.0f;     // a normal bounce reaches 244 px
constexpr float SPRING_V = 2100.0f;   // a spring reaches 790 px
constexpr float BULLET_V = 1100.0f;
constexpr float PLAYER_HALF = 24.0f;  // half of Hopper's body width, for landing / collisions
constexpr float PLAYER_H = 66.0f;
constexpr float PLAT_W = 90.0f, PLAT_H = 21.0f;
constexpr float MONSTER_W = 54.0f, MONSTER_H = 48.0f;
constexpr int PLAYER_LINE = 236;      // Hopper never rises above this screen y; the world scrolls instead
constexpr float TILT_GAIN[3] = {950.0f, 1400.0f, 1950.0f};   // px/s at full tilt: LOW / MED / HIGH
constexpr float PX_PER_M = 12.0f;     // score is height in "metres"

uint32_t rnd(uint32_t n) { return esp_random() % n; }
float frand() { return (esp_random() & 0xFFFF) / 65535.0f; }
float clampf(float v, float a, float b) { return std::max(a, std::min(b, v)); }
float lerp(float a, float b, float t) { return a + (b - a) * t; }
Color mix(uint8_t r0, uint8_t g0, uint8_t b0, uint8_t r1, uint8_t g1, uint8_t b1, float t)
{
    return rgb(uint8_t(lerp(r0, r1, t)), uint8_t(lerp(g0, g1, t)), uint8_t(lerp(b0, b1, t)));
}

// ------------------------------------------------------------------ sprites
// One letter per pixel, '.' is transparent. Colours come from the palette below.
struct Sprite {
    int w, h;
    const char *const *rows;
};

// Hopper, facing right: orange, big eyes, a red aviator cap and little brown boots.
constexpr const char *kHopperRows[] = {
    "......RRRRRRR.......",
    "....RRrrrrrrrRR.....",
    "...RRrrrrrrrrrrRR...",
    "..DRRRRRRRRRRRRRRD..",
    ".DOOOOOOOOOOOOOOOOD.",
    "DOOOWWWWWOOWWWWWOOOD",
    "DOOWWWWWWWWWWWWWWOOD",
    "DOOWWWWKKWWWWWKKWOOD",
    "DOOWWWWKKWWWWWKKWOOD",
    "DOOOWWWWWOOWWWWWOOOD",
    "DOOOOOOOOOOOOOOOOOOD",
    "DOOOOOoooooooooOOOOD",
    "DOOOOooooooooooooOOD",
    "DOOOOooooKKKKoooOOOD",
    "DOOOOoooooooooooOOOD",
    ".DOOOOooooooooooOOD.",
    ".DOOOOOOOOOOOOOOOOD.",
    "..DOOOOOOOOOOOOOOD..",
    "...DDOOOOOOOOOODD...",
    ".....DDDDDDDDDD.....",
    "....FFFF....FFFF....",
    "...FFFFF....FFFFF...",
};
constexpr Sprite kHopper{20, 22, kHopperRows};

// Purple monster: horns, angry brows, a mouth full of teeth, stubby legs.
constexpr const char *kMonsterRows[] = {
    ".DD............DD.",
    ".DPD..........DPD.",
    "..DPD........DPD..",
    "..DDPPDDDDDDPPDD..",
    ".DPKKKKPPPPKKKKPD.",
    "DPPWWWWPPPPWWWWPPD",
    "DPWWWWWWPPWWWWWWPD",
    "DPWWKKWWPPWWKKWWPD",
    "DPPWWWWPPPPWWWWPPD",
    "DPPPPPPPPPPPPPPPPD",
    "DPPDDDDDDDDDDDDPPD",
    "DPPDWDWDWDWDWDWDPD",
    ".DPPDDDDDDDDDDDPD.",
    ".DPPPPPPPPPPPPPPD.",
    "..DDPPDDDDDDPPDD..",
    "...DD........DD...",
};
constexpr Sprite kMonster{18, 16, kMonsterRows};

// Ledges are 30 x 7: a grassy one, a sliding ice one and a cracked stone one.
constexpr const char *kGrassRows[] = {
    "..g..g.....gg...g....g..g.g...",
    ".GGGGGGGGGGGGGGGGGGGGGGGGGGGG.",
    "GGGGGGGGGGGGGGGGGGGGGGGGGGGGGG",
    "DBBBBBBBBBBBBBBBBBBBBBBBBBBBBD",
    "DBbBBBBbBBBBBbBBBBbBBBBBbBBbBD",
    "DBBBBBBBBBBBBBBBBBBBBBBBBBBBBD",
    ".DDDDDDDDDDDDDDDDDDDDDDDDDDDD.",
};
constexpr const char *kIceRows[] = {
    "..............................",
    ".WWWWWWWWWWWWWWWWWWWWWWWWWWWW.",
    "WWwwwwwwwwwwwwwwwwwwwwwwwwwwWW",
    "IwwIIIIIIIIIIIIIIIIIIIIIIIwwII",
    "IIIIIIIiIIIIIiIIIIIIiIIIIIIIII",
    "DIIIIIIIIIIIIIIIIIIIIIIIIIIIID",
    ".DDDDDDDDDDDDDDDDDDDDDDDDDDDD.",
};
constexpr const char *kStoneRows[] = {
    "..............................",
    ".SSSSSSSSSSSSSSSSSSSSSSSSSSSS.",
    "SSSSSSKSSSSSSSSSSSSSSKSSSSSSSS",
    "SssssKssssssKKKsssssKsssssssss",
    "SssssKsssssKsssKssssKKsssssssS",
    "DssssssssssKssssssssssKsssssSD",
    ".DDDDDDDDDDDDDDDDDDDDDDDDDDDD.",
};
constexpr Sprite kGrass{30, 7, kGrassRows}, kIce{30, 7, kIceRows}, kStone{30, 7, kStoneRows};

// A coil spring with a red pad, sitting on a ledge.
constexpr const char *kSpringRows[] = {
    ".rrrrrrrrrr.",
    "RRRRRRRRRRRR",
    "..DssssssD..",
    "..DsDDDDsD..",
    "..DssssssD..",
    "..DsDDDDsD..",
    "..DssssssD..",
};
constexpr Sprite kSpring{12, 7, kSpringRows};

// Bullet: a little star.
constexpr const char *kShotRows[] = {
    ".YY.",
    "YWWY",
    "YWWY",
    ".YY.",
};
constexpr Sprite kShot{4, 4, kShotRows};

// Clouds, drawn in "cloud" colours from the palette (C = fluffy, c = shaded).
constexpr const char *kCloudARows[] = {
    ".......CCCC.........",
    ".....CCCCCCC...CCC..",
    "...CCCCCCCCCCCCCCCC.",
    "..CCCCCCCCCCCCCCCCCC",
    ".CCCCCCCCCCCCCCCCCCC",
    "CCCCCCCCCCCCCCCCCCCC",
    "cCCCCCCCCCCCCCCCCCCc",
    ".cccccccccccccccccc.",
};
constexpr const char *kCloudBRows[] = {
    "....CCC.......",
    "..CCCCCCC.CC..",
    ".CCCCCCCCCCCC.",
    "CCCCCCCCCCCCCC",
    "cCCCCCCCCCCCCc",
    ".cccccccccccc.",
};
constexpr Sprite kCloudA{20, 8, kCloudARows}, kCloudB{14, 6, kCloudBRows};

// Palette shared by all the sprites above.
Color palette(char ch, Color cloud_c, Color cloud_shade)
{
    switch (ch) {
    case 'D': return rgb(40, 24, 20);       // outline
    case 'O': return rgb(255, 185, 40);     // Hopper body
    case 'o': return rgb(255, 228, 130);    // belly
    case 'R': return rgb(220, 50, 50);      // cap
    case 'r': return rgb(255, 120, 110);    // cap highlight
    case 'W': return rgb(255, 255, 255);
    case 'K': return rgb(20, 16, 24);
    case 'F': return rgb(110, 65, 25);      // boots
    case 'P': return rgb(175, 65, 210);     // monster
    case 'G': return rgb(80, 200, 95);      // grass
    case 'g': return rgb(170, 245, 150);    // grass tufts
    case 'B': return rgb(150, 100, 55);     // dirt
    case 'b': return rgb(110, 70, 40);      // dirt specks
    case 'I': return rgb(90, 160, 240);     // ice
    case 'i': return rgb(60, 120, 200);
    case 'w': return rgb(200, 235, 255);
    case 'S': return rgb(165, 165, 175);    // stone
    case 's': return rgb(130, 130, 140);
    case 'Y': return rgb(255, 220, 50);
    case 'C': return cloud_c;
    case 'c': return cloud_shade;
    default: return 0;
    }
}

enum PType : uint8_t { NORMAL, MOVING, CRUMBLE, SPRING, GROUND };

struct Plat {
    float x, y;        // centre x, top y (world)
    float w;           // landing width
    PType type;
    float base_x, phase, speed;   // MOVING: swings around base_x
    bool broken;       // CRUMBLE after someone stepped on it
    float fall_t;
};
struct Monster {
    float x, y;        // centre x, bottom y (world)
    float base_x, phase;
    bool alive;
};
struct Bullet {
    float x, y;
};
struct Puff {
    float x, y, vx, vy, t;
    Color c;
};
struct Cloud {
    float x, y;        // y in the slow parallax layer
    uint8_t kind;
};
struct Star {
    int16_t x, y;
    uint8_t size;
};

enum Phase { READY, PLAYING, DEAD, GAME_OVER };

namespace sfx {
using wc::audio::Tone;
using wc::audio::Wave;

void boing(float pitch01)
{
    const float f = 260 + 140 * pitch01;
    wc::audio::play({.f0 = f, .f1 = f * 2.1f, .ms = 70, .wave = Wave::Triangle, .volume = 0.6f});
}
void spring()
{
    const Tone t[] = {{.f0 = 300, .f1 = 1500, .ms = 140, .wave = Wave::Square, .volume = 0.55f},
                      {.f0 = 900, .f1 = 1800, .ms = 90, .wave = Wave::Triangle, .volume = 0.5f, .delay_ms = 60}};
    wc::audio::play(t, 2);
}
void crumble()
{
    const Tone t[] = {{.f0 = 200, .f1 = 90, .ms = 90, .wave = Wave::Noise, .volume = 0.6f},
                      {.f0 = 180, .f1 = 60, .ms = 120, .wave = Wave::Triangle, .volume = 0.4f}};
    wc::audio::play(t, 2);
}
void shoot() { wc::audio::play({.f0 = 1400, .f1 = 700, .ms = 45, .wave = Wave::Square, .volume = 0.35f}); }
void stomp()
{
    const Tone t[] = {{.f0 = 160, .f1 = 60, .ms = 110, .wave = Wave::Square, .volume = 0.7f},
                      {.f0 = 500, .f1 = 1000, .ms = 90, .wave = Wave::Triangle, .volume = 0.5f, .delay_ms = 90}};
    wc::audio::play(t, 2);
}
void pop()
{
    const Tone t[] = {{.f0 = 700, .f1 = 1400, .ms = 60, .wave = Wave::Square, .volume = 0.5f},
                      {.f0 = 300, .f1 = 100, .ms = 80, .wave = Wave::Noise, .volume = 0.45f, .delay_ms = 40}};
    wc::audio::play(t, 2);
}
void hurt()
{
    const Tone t[] = {{.f0 = 400, .f1 = 150, .ms = 200, .wave = Wave::Square, .volume = 0.7f},
                      {.f0 = 120, .f1 = 60, .ms = 200, .wave = Wave::Noise, .volume = 0.4f}};
    wc::audio::play(t, 2);
}
void fall() { wc::audio::play({.f0 = 900, .f1 = 80, .ms = 600, .wave = Wave::Triangle, .volume = 0.6f}); }
void start() { wc::audio::play({.f0 = 500, .f1 = 900, .ms = 100, .wave = Wave::Triangle, .volume = 0.6f}); }
void newBest()
{
    const Tone t[] = {{.f0 = 660, .ms = 90, .wave = Wave::Square, .volume = 0.6f, .delay_ms = 0},
                      {.f0 = 880, .ms = 90, .wave = Wave::Square, .volume = 0.6f, .delay_ms = 100},
                      {.f0 = 1100, .ms = 90, .wave = Wave::Square, .volume = 0.6f, .delay_ms = 200},
                      {.f0 = 1320, .ms = 260, .wave = Wave::Square, .volume = 0.7f, .delay_ms = 300}};
    wc::audio::play(t, 4);
}
void gameOver()
{
    const Tone t[] = {{.f0 = 392, .ms = 160, .wave = Wave::Square, .volume = 0.6f, .delay_ms = 0},
                      {.f0 = 330, .ms = 160, .wave = Wave::Square, .volume = 0.6f, .delay_ms = 160},
                      {.f0 = 262, .ms = 320, .wave = Wave::Square, .volume = 0.6f, .delay_ms = 320}};
    wc::audio::play(t, 3);
}
}  // namespace sfx

}  // namespace

struct Jump::State {
    // ---- run
    Phase phase = READY;
    float phase_t = 0;
    int score = 0, best = 0;   // metres
    bool got_best = false;

    // ---- Hopper
    float px = 0, py = 0;      // feet
    float vx = 0, vy = 0;
    float prev_y = 0;
    int facing = 1;
    float squash = 0;          // 1 right after a bounce, decays
    float fire_t = 0;
    float cam = 0;             // world y at the bottom edge of the screen
    float top_y = 0;           // highest feet position so far

    // ---- world
    std::vector<Plat> plats;
    std::vector<Monster> monsters;
    std::vector<Bullet> bullets;
    std::vector<Puff> puffs;
    Cloud clouds[6];
    Star stars[70];
    float gen_y = 0;           // next platform goes about here
    float next_monster_y = 0;
    float t = 0;               // run time, for animation

    // ---- settings
    int tilt_sens = 1;         // index into TILT_GAIN

    // ---- menu / input
    Gestures ges;
    bool menu = false, menu_dirty = false;

    // ------------------------------------------------------------------ persistence
    void load()
    {
        nvs_handle_t h;
        if (nvs_open("jump", NVS_READONLY, &h) != ESP_OK) return;
        int32_t v;
        if (nvs_get_i32(h, "best", &v) == ESP_OK) best = v;
        uint8_t s;
        if (nvs_get_u8(h, "tilt", &s) == ESP_OK && s < 3) tilt_sens = s;
        nvs_close(h);
    }
    void save()
    {
        nvs_handle_t h;
        if (nvs_open("jump", NVS_READWRITE, &h) != ESP_OK) return;
        nvs_set_i32(h, "best", best);
        nvs_set_u8(h, "tilt", uint8_t(tilt_sens));
        nvs_commit(h);
        nvs_close(h);
    }

    // ------------------------------------------------------------------ world building
    float difficulty() const { return clampf(top_y / PX_PER_M / 1800.0f, 0.0f, 1.0f); }

    float screenY(float wy) const { return H - (wy - cam); }

    void addPlat(float x, float y, PType type)
    {
        Plat p{};
        p.x = p.base_x = clampf(x, 60, W - 60);
        p.y = y;
        p.w = PLAT_W;
        p.type = type;
        p.phase = frand() * 2 * PI;
        p.speed = 1.2f + frand() * 1.2f + difficulty() * 1.5f;
        plats.push_back(p);
    }

    // Keep the tower going up to a screen-height above the camera.
    void generate()
    {
        const float d = difficulty();
        while (gen_y < cam + H + 120) {
            const float gap = lerp(52, 84, frand()) + d * lerp(30, 120, frand());
            gen_y += gap;
            const float r = frand();
            PType type = NORMAL;
            if (r < 0.06f + 0.02f * d) type = SPRING;
            else if (r < 0.12f + 0.30f * d) type = MOVING;
            addPlat(60 + frand() * (W - 120), gen_y, type);

            // Crumbling ledges are extras, never the only way up.
            if (frand() < 0.10f + 0.35f * d) {
                const float cy = gen_y + gap * 0.45f;
                addPlat(60 + frand() * (W - 120), cy, CRUMBLE);
            }
            if (gen_y > next_monster_y && top_y / PX_PER_M > 120) {
                Monster m{};
                m.x = m.base_x = 90 + frand() * (W - 180);
                m.y = gen_y + gap * 0.5f + 24;
                m.phase = frand() * 2 * PI;
                m.alive = true;
                monsters.push_back(m);
                next_monster_y = gen_y + lerp(1400, 700, d) + frand() * 600;
            }
        }
    }

    void newGame()
    {
        plats.clear();
        monsters.clear();
        bullets.clear();
        puffs.clear();
        px = Gfx::CX;
        py = 0;
        prev_y = 0;
        vx = vy = 0;
        facing = 1;
        squash = 0;
        cam = -(H - 380);   // the ground sits at screen y 380
        top_y = 0;
        score = 0;
        got_best = false;
        t = 0;
        // Solid ground to start on, then the tower.
        plats.push_back(Plat{float(Gfx::CX), 0, float(W) + 200, GROUND, float(Gfx::CX), 0, 0, false, 0});
        gen_y = 0;
        next_monster_y = 2500;
        generate();
        for (Cloud &c : clouds) {
            c.x = frand() * W;
            c.y = cam * 0.35f + frand() * H;
            c.kind = uint8_t(rnd(2));
        }
        for (Star &s : stars) {
            s.x = int16_t(rnd(W));
            s.y = int16_t(rnd(H));
            s.size = uint8_t(2 + (rnd(4) == 0));
        }
        phase = READY;
        phase_t = 0;
    }

    void beginPlay()
    {
        phase = PLAYING;
        phase_t = 0;
        vy = JUMP_V;
        squash = 1;
        sfx::start();
    }

    // ------------------------------------------------------------------ simulation
    void spawnPuffs(float x, float y, int n, Color c, float speed)
    {
        for (int i = 0; i < n; i++) {
            const float a = frand() * 2 * PI, s = speed * (0.4f + frand() * 0.8f);
            puffs.push_back(Puff{x, y, std::cos(a) * s, std::sin(a) * s + speed * 0.3f, 0, c});
        }
    }

    void die()
    {
        phase = DEAD;
        phase_t = 0;
        vy = 500;   // a little hop, then the long fall
        sfx::hurt();
        spawnPuffs(px, py + PLAYER_H / 2, 10, rgb(255, 185, 40), 260);
    }

    void step(const InputState &in, float dt)
    {
        t += dt;

        // Steering: tilt the watch left or right. Full speed at about 25 degrees.
        const float gain = TILT_GAIN[tilt_sens];
        const float target = clampf(in.tilt.ax / 0.42f, -1.0f, 1.0f) * gain;
        vx += (target - vx) * std::min(1.0f, dt * 14);
        if (std::fabs(vx) > 60) facing = vx > 0 ? 1 : -1;
        px += vx * dt;
        if (px < -PLAYER_HALF) px += W + 2 * PLAYER_HALF;
        if (px > W + PLAYER_HALF) px -= W + 2 * PLAYER_HALF;

        prev_y = py;
        vy -= GRAVITY * dt;
        py += vy * dt;

        // Shooting: hold to keep firing.
        fire_t -= dt;
        if (phase == PLAYING && in.touch.down && fire_t <= 0) {
            bullets.push_back(Bullet{px, py + PLAYER_H * 0.8f});
            fire_t = 0.22f;
            sfx::shoot();
        }

        // Platforms move, crumbled ones fall away.
        for (Plat &p : plats) {
            if (p.type == MOVING) {
                p.x = p.base_x + std::sin(p.phase + t * p.speed) * 85;
                p.x = clampf(p.x, 50, W - 50);
            }
            if (p.broken) {
                p.fall_t += dt;
                p.y -= 500 * p.fall_t * dt;
            }
        }
        for (Monster &m : monsters) m.x = m.base_x + std::sin(m.phase + t * 1.3f) * 40;

        // Landing (only when falling, only on the way through a platform's top).
        if (phase == PLAYING && vy < 0) {
            for (Plat &p : plats) {
                if (p.broken) continue;
                if (prev_y < p.y || py > p.y) continue;
                if (std::fabs(px - p.x) > p.w / 2 + PLAYER_HALF * 0.6f) continue;
                if (p.type == CRUMBLE) {
                    p.broken = true;
                    p.fall_t = 0;
                    sfx::crumble();
                    spawnPuffs(p.x, p.y, 8, rgb(130, 130, 140), 180);
                    continue;   // no bounce: it gives way
                }
                py = p.y;
                vy = p.type == SPRING ? SPRING_V : JUMP_V;
                squash = 1;
                if (p.type == SPRING) {
                    sfx::spring();
                    spawnPuffs(p.x, p.y, 6, colors::white, 200);
                } else {
                    sfx::boing(clampf((p.y - top_y) / 200.0f + 0.5f, 0, 1));
                }
                break;
            }

            for (Monster &m : monsters) {
                if (!m.alive) continue;
                if (std::fabs(px - m.x) > MONSTER_W / 2 + PLAYER_HALF * 0.6f) continue;
                const float m_top = m.y + MONSTER_H;
                if (prev_y >= m_top - 10 && py <= m_top && py >= m.y) {
                    m.alive = false;
                    py = m_top;
                    vy = JUMP_V * 1.15f;
                    squash = 1;
                    sfx::stomp();
                    spawnPuffs(m.x, m.y + MONSTER_H / 2, 14, rgb(175, 65, 210), 300);
                    break;
                }
            }
        }
        // Running into a monster from the side or below.
        if (phase == PLAYING) {
            for (Monster &m : monsters) {
                if (!m.alive) continue;
                if (std::fabs(px - m.x) < MONSTER_W / 2 + PLAYER_HALF * 0.7f && py + PLAYER_H * 0.8f > m.y + 8 &&
                    py < m.y + MONSTER_H - 8) {
                    die();
                    break;
                }
            }
        }

        // Bullets.
        for (size_t i = 0; i < bullets.size();) {
            Bullet &b = bullets[i];
            b.y += BULLET_V * dt;
            bool gone = screenY(b.y) < -10;
            for (Monster &m : monsters) {
                if (!m.alive) continue;
                if (std::fabs(b.x - m.x) < MONSTER_W / 2 && b.y > m.y && b.y < m.y + MONSTER_H) {
                    m.alive = false;
                    gone = true;
                    sfx::pop();
                    spawnPuffs(m.x, m.y + MONSTER_H / 2, 14, rgb(175, 65, 210), 300);
                    break;
                }
            }
            if (gone) bullets.erase(bullets.begin() + i);
            else i++;
        }

        // Particles.
        for (size_t i = 0; i < puffs.size();) {
            Puff &q = puffs[i];
            q.t += dt;
            q.vy -= 900 * dt;
            q.x += q.vx * dt;
            q.y += q.vy * dt;
            if (q.t > 0.6f) puffs.erase(puffs.begin() + i);
            else i++;
        }

        squash = std::max(0.0f, squash - dt * 7);

        // Camera follows Hopper up, never down.
        if (phase == PLAYING) {
            const float limit = py - (H - PLAYER_LINE);
            if (limit > cam) cam = limit;
            if (py > top_y) top_y = py;
            score = int(top_y / PX_PER_M);
            if (score > best) {
                if (!got_best && best > 0) sfx::newBest();
                got_best = true;
                best = score;
            }
            generate();
        }

        // Tidy up what scrolled off the bottom.
        plats.erase(std::remove_if(plats.begin(), plats.end(),
                                   [&](const Plat &p) { return p.y < cam - 80 || p.fall_t > 1.0f; }),
                    plats.end());
        monsters.erase(std::remove_if(monsters.begin(), monsters.end(),
                                      [&](const Monster &m) { return m.y + MONSTER_H < cam - 40 || !m.alive; }),
                       monsters.end());

        // Fell off the bottom of the screen.
        if (py < cam - 80) {
            if (phase == PLAYING) sfx::fall();
            phase = GAME_OVER;
            phase_t = 0;
            if (got_best) save();
            sfx::gameOver();
        }
    }

    void update(Engine &e, float dt)
    {
        const InputState &in = e.input();
        ges.update(in.touch);
        phase_t += dt;

        if (menu) {
            if (ges.tap) menuTap(e, ges.x, ges.y);
            if (ges.swipe_right || (in.clicked & BTN_B)) closeMenu();
            return;
        }
        if (ges.swipe_left && (phase == PLAYING || phase == READY)) {
            menu = true;
            menu_dirty = true;
            return;
        }

        switch (phase) {
        case READY:
            if (ges.tap) beginPlay();
            break;
        case PLAYING:
        case DEAD:
            step(in, dt);
            break;
        case GAME_OVER:
            if (ges.tap && phase_t > 0.7f) newGame();
            break;
        }
    }

    // ------------------------------------------------------------------ menu
    void closeMenu() { menu = false; }

    void menuTap(Engine &e, int x, int y)
    {
        namespace ui = console::ui;
        if (ui::rowRect(0).hit(x, y)) {
            tilt_sens = (tilt_sens + 1) % 3;
            save();
            menu_dirty = true;
        } else if (ui::rowRect(1).hit(x, y)) {
            wc::audio::setVolume(wc::audio::volume() == 0 ? 2 : 0);
            if (wc::audio::volume() > 0) sfx::start();
            menu_dirty = true;
        } else if (ui::rowRect(2).hit(x, y)) {
            newGame();
            closeMenu();
        } else if (ui::buttonRect(0, 2).hit(x, y)) {
            closeMenu();
        } else if (ui::buttonRect(1, 2).hit(x, y)) {
            closeMenu();
            e.goHome();
        }
    }

    void drawMenu(Gfx &g)
    {
        namespace ui = console::ui;
        static const char *kSens[] = {"LOW", "MED", "HIGH"};
        ui::clearScreen(g);
        ui::title(g, "PAUSED");
        ui::row(g, 0, "TILT", kSens[tilt_sens]);
        ui::row(g, 1, "SOUND", wc::audio::volume() ? "ON" : "OFF", wc::audio::volume() ? ui::GO : ui::DIM);
        ui::row(g, 2, "NEW GAME", "GO", ui::ACCENT);
        char buf[16];
        snprintf(buf, sizeof(buf), "%d M", best);
        ui::row(g, 3, "BEST", buf, ui::LABEL);
        ui::button(g, ui::buttonRect(0, 2), "RESUME");
        ui::outlineButton(g, ui::buttonRect(1, 2), "HOME");
    }

    // ------------------------------------------------------------------ drawing
    Color cloud_c = colors::white, cloud_shade = rgb(200, 215, 240);

    // Draw a sprite with its bottom-centre at (x, y), each sprite pixel sx by sy
    // screen pixels (non-integer scales squash and stretch). Runs of one colour
    // become one fillRect, so this is cheap even at 3x.
    void drawSprite(Gfx &g, const Sprite &s, float x, float y, float sx, float sy, bool flip = false)
    {
        const float x0 = x - s.w * sx / 2, y0 = y - s.h * sy;
        if (y0 > H || y0 + s.h * sy < 0) return;
        int py_prev = int(y0 + 0.5f);
        for (int r = 0; r < s.h; r++) {
            const int py_next = int(y0 + (r + 1) * sy + 0.5f);
            const int rh = py_next - py_prev;
            if (rh > 0) {
                const char *row = s.rows[r];
                int c = 0;
                while (c < s.w) {
                    const char ch = row[c];
                    int e = c + 1;
                    while (e < s.w && row[e] == ch) e++;
                    if (ch != '.') {
                        const int cs = flip ? s.w - e : c, ce = flip ? s.w - c : e;
                        const int xa = int(x0 + cs * sx + 0.5f), xb = int(x0 + ce * sx + 0.5f);
                        if (xb > xa) g.fillRect(xa, py_prev, xb - xa, rh, palette(ch, cloud_c, cloud_shade));
                    }
                    c = e;
                }
            }
            py_prev = py_next;
        }
    }

    void drawSky(Gfx &g)
    {
        // Morning at the bottom of the tower, deep space at the top.
        const float alt = clampf(cam / PX_PER_M / 2600.0f, 0.0f, 1.0f);
        // Day: light at the horizon, richer up top. Space: near-black with a violet glow low down.
        for (int y = 0; y < H; y += 2) {
            const float k = float(y) / H;   // 0 = top of screen
            const Color c = mix(72 + int(78 * k), 140 + int(75 * k), 235 + int(20 * k),
                                6 + int(34 * k), 6 + int(18 * k), 24 + int(56 * k), alt);
            g.fillRect(0, y, W, 2, c);
        }
        if (alt > 0.35f) {
            const int n = int((alt - 0.35f) / 0.65f * 70);
            const int drift = int(cam * 0.08f);
            for (int i = 0; i < n; i++) {
                const Star &s = stars[i];
                const int y = ((s.y + drift) % H + H) % H;
                const bool twinkle = ((i * 7 + int(t * 3)) % 11) == 0;
                g.fillRect(s.x, y, s.size, s.size, twinkle ? rgb(255, 240, 200) : alt > 0.7f ? colors::white : rgb(200, 205, 230));
            }
        }
        if (alt < 0.85f) {
            cloud_c = mix(255, 255, 255, 150, 130, 200, alt);
            cloud_shade = mix(200, 215, 240, 110, 90, 160, alt);
            for (Cloud &c : clouds) {
                // Clouds live in a slower layer, so they drift by as you climb.
                float sy = H - (c.y - cam * 0.35f);
                if (sy > H + 60) {
                    c.y += H + 120;
                    c.x = frand() * W;
                    c.kind = uint8_t(rnd(2));
                    sy = H - (c.y - cam * 0.35f);
                }
                if (sy < -60) continue;
                drawSprite(g, c.kind ? kCloudB : kCloudA, c.x, sy, 4, 4);
            }
        }
    }

    void drawGround(Gfx &g, const Plat &p)
    {
        const int sy = int(screenY(p.y));
        if (sy > H) return;
        // Grass on top, dirt below, all the way down.
        g.fillRect(0, sy, W, 4, rgb(170, 245, 150));
        g.fillRect(0, sy + 4, W, 8, rgb(80, 200, 95));
        g.fillRect(0, sy + 12, W, std::max(0, H - sy - 12), rgb(150, 100, 55));
        for (int x = 7; x < W; x += 29) g.fillRect(x, sy + 22 + (x * 13) % 40, 6, 3, rgb(110, 70, 40));
        for (int x = 3; x < W; x += 23) g.fillRect(x, sy - 5, 3, 5, rgb(170, 245, 150));
    }

    void drawPlat(Gfx &g, const Plat &p)
    {
        if (p.type == GROUND) {
            drawGround(g, p);
            return;
        }
        const float sy = screenY(p.y);
        if (sy < -30 || sy > H + 30) return;
        const Sprite &s = p.type == MOVING ? kIce : p.type == CRUMBLE ? kStone : kGrass;
        drawSprite(g, s, p.x, sy + PLAT_H, SCALE, SCALE);
        if (p.type == SPRING) drawSprite(g, kSpring, p.x, sy + SCALE, SCALE, SCALE);
    }

    void drawMonster(Gfx &g, const Monster &m)
    {
        const float bob = std::sin(t * 5 + m.phase);
        const float sy = screenY(m.y);
        if (sy < -60 || sy > H + 60) return;
        // Breathes: a little wider as it squats.
        drawSprite(g, kMonster, m.x, sy, SCALE * (1 + 0.05f * bob), SCALE * (1 - 0.05f * bob), px < m.x);
    }

    void drawHopper(Gfx &g)
    {
        const float sy = screenY(py);
        if (sy < -80 || sy > H + 100) return;
        // Squash on landing, stretch when moving fast.
        const float q = squash * squash;
        float sx = 1 + 0.35f * q, sy_ = 1 - 0.30f * q;
        const float stretch = clampf(std::fabs(vy) / 2600.0f, 0, 0.18f);
        sx -= stretch * 0.5f;
        sy_ += stretch;
        const bool dead = phase == DEAD || phase == GAME_OVER;
        drawSprite(g, kHopper, px, sy, SCALE * sx, SCALE * sy_, facing < 0);
        if (dead) {
            // X eyes.
            const float ex = px, ey = sy - PLAYER_H * 0.66f;
            for (int i = -1; i <= 1; i += 2) {
                const int x = int(ex + i * 12 * sx), y = int(ey);
                g.fillRect(x - 6, y - 6, 12, 12, colors::white);
                g.line(x - 5, y - 5, x + 5, y + 5, colors::black);
                g.line(x - 5, y + 5, x + 5, y - 5, colors::black);
                g.line(x - 4, y - 5, x + 5, y + 4, colors::black);
                g.line(x - 5, y + 4, x + 4, y - 5, colors::black);
            }
        }
    }

    void drawHud(Gfx &g)
    {
        char buf[24];
        snprintf(buf, sizeof(buf), "%d M", score);
        console::ui::shadowText(g, Gfx::CX, 34, buf, colors::white, 3);
        if (best > 0) {
            snprintf(buf, sizeof(buf), "BEST %d M", best);
            console::ui::shadowText(g, Gfx::CX, 428, buf, got_best ? console::ui::ACCENT : rgb(220, 230, 245), 2);
        }
    }

    void banner(Gfx &g, const char *top, const char *bottom, Color c)
    {
        namespace ui = console::ui;
        g.fillRect(83, 196, 300, bottom ? 78 : 50, ui::PANEL);
        g.rect(83, 196, 300, bottom ? 78 : 50, ui::BOX);
        g.textCentered(Gfx::CX, 221, top, c, 3, true);
        if (bottom) g.textCentered(Gfx::CX, 254, bottom, ui::LABEL, 2, true);
    }

    void draw(Gfx &g)
    {
        if (menu) {
            if (menu_dirty) {
                drawMenu(g);
                menu_dirty = false;
            }
            return;
        }

        drawSky(g);
        for (const Plat &p : plats) drawPlat(g, p);
        for (const Monster &m : monsters) drawMonster(g, m);
        for (const Bullet &b : bullets) drawSprite(g, kShot, b.x, screenY(b.y) + 6, SCALE, SCALE);
        for (const Puff &q : puffs) {
            const int r = std::max(2, int(6 * (1 - q.t / 0.6f)));
            g.fillRect(int(q.x) - r / 2, int(screenY(q.y)) - r / 2, r, r, q.c);
        }
        drawHopper(g);
        drawHud(g);

        char buf[32];
        switch (phase) {
        case READY: banner(g, "TAP TO JUMP", "TILT TO STEER - TAP TO SHOOT", colors::white); break;
        case GAME_OVER:
            if (phase_t > 0.5f) {
                snprintf(buf, sizeof(buf), got_best ? "NEW BEST %d M!" : "%d M", score);
                banner(g, "GAME OVER", buf, got_best ? console::ui::ACCENT : console::ui::DANGER);
            }
            break;
        default: break;
        }
    }
};

Jump::Jump() : s_(new State) {}
Jump::~Jump() { delete s_; }

void Jump::begin(Engine &e)
{
    s_->load();
    s_->newGame();
    ESP_LOGI(TAG, "ready, best %d m", s_->best);
}

void Jump::enter(Engine &e)
{
    // Coming back from the home screen mid-run: pause rather than let it run blind.
    if (s_->phase == PLAYING) {
        s_->menu = true;
        s_->menu_dirty = true;
    }
    s_->menu_dirty = s_->menu;
}

bool Jump::keepAwake() const { return !s_->menu && (s_->phase == PLAYING || s_->phase == DEAD); }

void Jump::update(Engine &e, float dt) { s_->update(e, std::min(dt, 1.0f / 30)); }

void Jump::draw(Engine &e, Gfx &g) { s_->draw(g); }

}  // namespace games
