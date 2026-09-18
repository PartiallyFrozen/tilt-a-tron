#include "console/icons.h"

#include <cmath>
#include <initializer_list>

namespace console::icons {

using wc::Color;
using wc::rgb;

namespace {

constexpr float PI = 3.14159265f;
constexpr float TAU = 2 * PI;

float wrapAngle(float a)
{
    a = std::fmod(a + PI, TAU);
    if (a < 0) a += TAU;
    return a - PI;
}

// Calls shade(nd, a) for every pixel inside the icon circle. nd = distance / r
// (0..1), a = angle 0..2π clockwise from +x.
template <typename F>
void forEach(Color *buf, int r, F shade)
{
    const int size = 2 * r;
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            const float dx = x + 0.5f - r, dy = y + 0.5f - r;
            const float d = std::sqrt(dx * dx + dy * dy);
            Color &px = buf[y * size + x];
            if (d > r) {
                px = 0;
                continue;
            }
            float a = std::atan2(dy, dx);
            if (a < 0) a += TAU;
            px = shade(d / r, a, dx / r, dy / r);
        }
    }
}

}  // namespace

void breakout(Color *buf, int r)
{
    static constexpr Color BG = rgb(18, 20, 30), EDGE = rgb(70, 74, 92), CORE = rgb(44, 48, 62);
    struct Band {
        float r0, r1;
        int n;
        Color c;
    };
    static constexpr Band bands[] = {
        {0.24f, 0.36f, 9, rgb(0xff, 0x4f, 0x9a)},
        {0.40f, 0.52f, 13, rgb(0xff, 0x8a, 0x3d)},
        {0.56f, 0.68f, 17, rgb(0xff, 0xd2, 0x3f)},
    };
    constexpr float ball_x = 0.27f, ball_y = 0.69f, ball_r = 0.055f;

    forEach(buf, r, [&](float nd, float a, float x, float y) -> Color {
        if (nd > 0.955f) return EDGE;
        const float bx = x - ball_x, by = y - ball_y;
        if (bx * bx + by * by < ball_r * ball_r) return wc::colors::white;
        if (nd > 0.79f && nd < 0.87f && std::fabs(wrapAngle(a - PI / 2)) < 0.42f) return wc::colors::white;
        if (nd < 0.17f) return CORE;
        for (int i = 0; i < 3; i++) {
            const Band &b = bands[i];
            if (nd < b.r0 || nd > b.r1) continue;
            const float pos = a / TAU * b.n;
            const int idx = int(pos);
            const float frac = pos - idx;
            if (frac < 0.07f || frac > 0.93f) return BG;
            if (i == 2 && (idx * 7 + 3) % 5 == 0) return BG;   // a few already broken
            return b.c;
        }
        return BG;
    });
}

// A round labyrinth: wooden rings with gaps, a hole, and a steel ball.
void maze(Color *buf, int r)
{
    static constexpr Color FLOOR = rgb(201, 158, 98), WALL = rgb(104, 68, 34), EDGE = rgb(70, 74, 92),
                           BALL = rgb(206, 211, 219);
    struct Gap {
        float nd, a0, a1;
    };
    static constexpr Gap gaps[] = {{0.30f, 0.6f, 1.3f}, {0.52f, 3.4f, 4.0f}, {0.74f, 5.2f, 5.7f}, {0.74f, 1.9f, 2.3f}};
    forEach(buf, r, [&](float nd, float a, float x, float y) -> Color {
        if (nd > 0.955f) return EDGE;
        const float bx = x - 0.40f, by = y + 0.44f;
        if (bx * bx + by * by < 0.010f) return BALL;
        const float hx = x + 0.42f, hy = y - 0.12f;
        if (hx * hx + hy * hy < 0.012f) return rgb(0, 0, 0);
        for (float ring : {0.30f, 0.52f, 0.74f}) {
            if (std::fabs(nd - ring) > 0.035f) continue;
            bool gap = false;
            for (const Gap &g : gaps) gap |= g.nd == ring && a > g.a0 && a < g.a1;
            if (!gap) return WALL;
        }
        // A couple of spokes between the rings.
        if (nd > 0.30f && nd < 0.52f && std::fabs(wrapAngle(a - 2.6f)) < 0.07f) return WALL;
        if (nd > 0.52f && nd < 0.74f && std::fabs(wrapAngle(a - 0.4f)) < 0.05f) return WALL;
        return FLOOR;
    });
}

// A road running to the horizon with kerbs, and a red car seen from behind.
void racer(Color *buf, int r)
{
    static constexpr Color SKY = rgb(90, 150, 220), GRASS = rgb(38, 140, 52), ROAD = rgb(96, 96, 102),
                           EDGE = rgb(70, 74, 92), RED = rgb(225, 30, 40), TIRE = rgb(24, 24, 28);
    forEach(buf, r, [&](float nd, float, float x, float y) -> Color {
        if (nd > 0.955f) return EDGE;
        if (y < -0.15f) return SKY;
        // Car: wing, body and two fat tyres.
        if (y > 0.30f && y < 0.40f && std::fabs(x) < 0.34f) return TIRE;                       // rear wing
        if (y > 0.40f && y < 0.72f && std::fabs(x) < 0.16f) return RED;                         // body
        if (y > 0.46f && y < 0.76f && std::fabs(x) > 0.20f && std::fabs(x) < 0.36f) return TIRE; // tyres
        const float depth = (y + 0.15f) / 1.1f;          // 0 at the horizon, 1 at the bottom
        const float half = 0.06f + depth * 0.62f;
        const float ax = std::fabs(x);
        if (ax < half) {
            const bool dash = int(depth * depth * 9) % 2 == 0;
            return (ax < 0.012f + depth * 0.02f && dash) ? rgb(240, 240, 240) : ROAD;
        }
        if (ax < half * 1.16f) return int(depth * depth * 12) % 2 ? RED : rgb(240, 240, 240);
        return GRASS;
    });
}

void jump(Color *buf, int r)
{
    static constexpr Color SKY = rgb(110, 180, 245), EDGE = rgb(70, 74, 92), CLOUD = rgb(245, 250, 255),
                           PLAT = rgb(70, 195, 90), PLAT_TOP = rgb(160, 240, 150), BODY = rgb(255, 190, 50),
                           BELLY = rgb(255, 225, 130), WHITE = rgb(255, 255, 255), BLACK = rgb(0, 0, 0);
    forEach(buf, r, [&](float nd, float, float x, float y) -> Color {
        if (nd > 0.955f) return EDGE;
        // Hopper: a round body with two eyes, mid-bounce above the ledge.
        const float bx = x, by = y + 0.08f;
        const float body = bx * bx / (0.36f * 0.36f) + by * by / (0.40f * 0.40f);
        if (body < 1.0f) {
            for (int i = -1; i <= 1; i += 2) {
                const float ex = bx - i * 0.14f, ey = by + 0.10f;
                if ((ex - 0.03f) * (ex - 0.03f) + (ey - 0.02f) * (ey - 0.02f) < 0.045f * 0.045f) return BLACK;
                if (ex * ex + ey * ey < 0.085f * 0.085f) return WHITE;
            }
            if (bx * bx / (0.22f * 0.22f) + (by - 0.12f) * (by - 0.12f) / (0.20f * 0.20f) < 1.0f) return BELLY;
            return BODY;
        }
        if (y > 0.36f && y < 0.40f && std::fabs(x) > 0.05f && std::fabs(x) < 0.32f) return rgb(120, 70, 20);   // feet
        if (y > 0.50f && y < 0.66f && std::fabs(x) < 0.52f) return y < 0.54f ? PLAT_TOP : PLAT;              // ledge
        // A cloud drifting past.
        const float cx = x + 0.45f, cy = y + 0.55f;
        if (cx * cx + cy * cy < 0.16f * 0.16f || (cx + 0.16f) * (cx + 0.16f) + (cy + 0.05f) * (cy + 0.05f) < 0.12f * 0.12f ||
            (cx - 0.17f) * (cx - 0.17f) + (cy + 0.04f) * (cy + 0.04f) < 0.13f * 0.13f)
            return CLOUD;
        return SKY;
    });
}

void tiltatris(Color *buf, int r)
{
    // Fallback only (the real icon is a PNG): coloured wedge rings around a dark core.
    static constexpr Color BG = rgb(12, 12, 20), EDGE = rgb(70, 74, 92), CORE = rgb(16, 19, 28);
    static constexpr Color RING[3] = {rgb(255, 150, 40), rgb(80, 220, 240), rgb(190, 90, 240)};
    forEach(buf, r, [&](float nd, float a, float, float) -> Color {
        if (nd > 0.955f) return EDGE;
        if (nd < 0.22f) return CORE;
        if (nd < 0.85f) {
            const int ring = int((nd - 0.22f) / 0.21f);
            const float frac = std::fmod(a / TAU * 12 + ring * 0.3f, 1.0f);
            if (frac < 0.08f || (ring == 2 && int(a / TAU * 12) % 5 == 0)) return BG;
            return RING[ring % 3];
        }
        return BG;
    });
}

void clock(Color *buf, int r)
{
    // Fallback only (the real icon is a PNG): a gold pocket watch with a cream dial.
    static constexpr Color EDGE = rgb(70, 74, 92), GOLD = rgb(214, 170, 60), GOLD_D = rgb(140, 100, 30),
                           CREAM = rgb(244, 234, 205), INK = rgb(40, 30, 24), RED = rgb(220, 40, 40);
    forEach(buf, r, [&](float nd, float a, float x, float y) -> Color {
        if (nd > 0.955f) return EDGE;
        if (nd > 0.86f) return GOLD;
        if (nd > 0.80f) return GOLD_D;
        // Hands: hour at 10 o'clock, minute at 2 o'clock.
        const float ah = -TAU / 6, am = TAU / 6;
        const float hx = std::sin(ah), hy = -std::cos(ah), mx = std::sin(am), my = -std::cos(am);
        const float ph = x * hx + y * hy, dh = std::fabs(x * hy - y * hx);
        const float pm = x * mx + y * my, dm = std::fabs(x * my - y * mx);
        if (ph > -0.08f && ph < 0.45f && dh < 0.045f) return INK;
        if (pm > -0.08f && pm < 0.62f && dm < 0.03f) return INK;
        if (nd < 0.05f) return RED;
        if (nd > 0.70f && std::fmod(a + 0.02f, TAU / 12) < 0.05f) return INK;
        return CREAM;
    });
}

void settings(Color *buf, int r)
{
    static constexpr Color BG = rgb(26, 30, 40), EDGE = rgb(70, 74, 92), GEAR = rgb(205, 210, 222),
                           SHADE = rgb(150, 156, 170);
    forEach(buf, r, [&](float nd, float a, float, float) -> Color {
        if (nd > 0.955f) return EDGE;
        if (nd < 0.19f) return BG;                                   // axle hole
        if (nd < 0.27f) return SHADE;                                // hub ring
        if (nd < 0.47f) return GEAR;                                 // body
        if (nd < 0.63f && std::cos(8 * a) > 0.15f) return GEAR;      // teeth
        return BG;
    });
}

}  // namespace console::icons
