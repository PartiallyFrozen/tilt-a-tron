// Sleepy Star, built from SLEEPY_STAR_SPEC.md. The board is fixed to the watch; the
// laser emitter orbits the rim to wherever real-world "up" is (8 spokes, with
// hysteresis). Geometry in the spec is in device pixels; this draws on the 2x pixel
// canvas, so every spec number appears here halved.
#include "games/star.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "audio/audio.h"
#include "console/ui.h"
#include "engine/canvas.h"
#include "engine/gestures.h"
#include "engine/polar.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"

using namespace wc;

namespace games {

namespace {

const char *TAG = "star";

constexpr int SCALE = 2, CW = (Gfx::W + SCALE - 1) / SCALE;
constexpr float C = CW / 2.0f;
constexpr float PI = 3.14159265f, TAU = 2 * PI;

// ------------------------------------------------------------------ model (spec section 4)
enum El : uint8_t { BLOCK = 0, GAP, MIRROR_L, MIRROR_R, SPLIT };
constexpr int SPOKES = 8, MAX_RINGS = 4, DOOR = 4;

struct Level {
    uint8_t n_rings;
    uint8_t ring[MAX_RINGS][SPOKES];   // ring[0] = outermost
    uint8_t start[MAX_RINGS];
    uint8_t best;                      // verified minimum taps
};

inline int m8(int n) { return ((n % 8) + 8) % 8; }

// The six machine-verified campaign levels (spec section 7).
constexpr uint8_t O = GAP, L_ = MIRROR_L, R_ = MIRROR_R, Y = SPLIT, _ = BLOCK;
struct Campaign {
    int n;
    Level lvl;
};
constexpr Campaign CAMPAIGN[] = {
    {1, {2, {{O, _, O, _, _, _, O, _}, {_, _, _, O, O, _, _, O}}, {3, 2}, 1}},
    {2, {2, {{_, _, _, _, _, O, _, O}, {_, _, _, O, O, _, _, _}}, {7, 6}, 2}},
    {4, {2, {{_, _, _, _, O, L_, L_, _}, {_, _, _, _, O, O, L_, _}}, {5, 0}, 3}},
    {7, {3, {{_, _, O, L_, _, _, _, O}, {_, L_, O, _, _, _, _, R_}, {_, L_, R_, _, R_, _, _, _}}, {3, 4, 2}, 3}},
    {11, {3, {{_, _, O, L_, _, O, _, _}, {O, _, _, _, _, L_, R_, _}, {R_, _, _, L_, Y, _, _, L_}}, {4, 3, 0}, 4}},
    {16,
     {4,
      {{L_, _, _, _, O, _, L_, O}, {_, R_, _, _, _, R_, _, L_}, {L_, _, O, R_, _, R_, _, _}, {_, O, _, _, _, R_, R_, _}},
      {0, 6, 1, 6},
      5}},
};

// Ring layout by ring count (spec section 3, halved): radii outer -> inner, teeth, star radius.
struct Geo {
    float rad[MAX_RINGS];
    int teeth[MAX_RINGS];
    float star;
};
constexpr Geo GEO[MAX_RINGS + 1] = {
    {}, {},
    {{56.5f, 35.5f}, {24, 16}, 16.0f},
    {{64.0f, 46.0f, 28.0f}, {32, 24, 16}, 14.0f},
    {{67.0f, 51.5f, 36.0f, 20.5f}, {32, 24, 16, 8}, 11.0f},
};
constexpr float BAND_HALF = 6.25f, HOLE_HALF = 6.75f, EMIT_R = 84.0f, DISC_R = 82.0f, BEZEL_R = 110.5f;

// What an element does to an inbound beam on spoke s: a bit mask of outbound spokes.
inline uint8_t outs(uint8_t el, int s)
{
    switch (el) {
    case GAP: return uint8_t(1u << s);
    case MIRROR_R: return uint8_t(1u << m8(s + 1));
    case MIRROR_L: return uint8_t(1u << m8(s - 1));
    case SPLIT: return uint8_t((1u << s) | (1u << m8(s + 1)));
    default: return 0;
    }
}

// Does any branch reach the door, entering on any spoke in `entry_mask`?
bool wins(const Level &L, const uint8_t *rot, uint8_t entry_mask)
{
    uint8_t mask = entry_mask;
    for (int i = 0; i < L.n_rings && mask; i++) {
        uint8_t next = 0;
        for (int s = 0; s < SPOKES; s++)
            if (mask & (1u << s)) next |= outs(L.ring[i][m8(s - rot[i])], s);
        mask = next;
    }
    return mask & (1u << DOOR);
}

// ------------------------------------------------------------------ solver / generator (spec section 8)
constexpr int MAX_STATES = 4096;
EXT_RAM_BSS_ATTR int8_t s_dist[MAX_STATES];    // taps from each ring configuration to the nearest winning one
EXT_RAM_BSS_ATTR uint16_t s_queue[MAX_STATES];

int pack(const uint8_t *rot, int n)
{
    int v = 0;
    for (int i = n - 1; i >= 0; i--) v = v * 8 + rot[i];
    return v;
}
void unpack(int v, uint8_t *rot, int n)
{
    for (int i = 0; i < n; i++, v /= 8) rot[i] = uint8_t(v % 8);
}

// Fills s_dist for every configuration; returns how many of them win at some wrist angle.
int solve(const Level &L)
{
    int total = 1;
    for (int i = 0; i < L.n_rings; i++) total *= 8;
    int head = 0, tail = 0, winners = 0;
    uint8_t rot[MAX_RINGS];
    for (int v = 0; v < total; v++) {
        unpack(v, rot, L.n_rings);
        if (wins(L, rot, 0xFF)) {
            s_dist[v] = 0;
            s_queue[tail++] = uint16_t(v);
            winners++;
        } else {
            s_dist[v] = -1;
        }
    }
    // Breadth-first backwards: the state before "tap ring i" has ring i one notch
    // back and the ring inside it one notch forward.
    while (head < tail) {
        const int v = s_queue[head++];
        unpack(v, rot, L.n_rings);
        for (int i = 0; i < L.n_rings; i++) {
            uint8_t p[MAX_RINGS];
            std::memcpy(p, rot, sizeof(p));
            p[i] = uint8_t(m8(p[i] - 1));
            if (i < L.n_rings - 1) p[i + 1] = uint8_t(m8(p[i + 1] + 1));
            const int pv = pack(p, L.n_rings);
            if (s_dist[pv] < 0) {
                s_dist[pv] = int8_t(s_dist[v] + 1);
                s_queue[tail++] = uint16_t(pv);
            }
        }
    }
    return winners;
}

struct Rng {
    uint32_t s;
    uint32_t next()
    {
        s ^= s << 13, s ^= s >> 17, s ^= s << 5;
        return s;
    }
    int below(int n) { return int(next() % uint32_t(n)); }
};

// Tier schedule (spec section 8): rings, elements, target taps, rarity ceiling.
void tier(int n, int &rings, bool &mirrors, bool &splits, int &taps, float &max_rarity)
{
    if (n <= 3) rings = 2, mirrors = false, splits = false, taps = n <= 1 ? 1 : 2, max_rarity = 0.30f;
    else if (n <= 6) rings = 2, mirrors = true, splits = false, taps = 3, max_rarity = 0.16f;
    else if (n <= 10) rings = 3, mirrors = true, splits = false, taps = n <= 8 ? 3 : 4, max_rarity = 0.10f;
    else if (n <= 15) rings = 3, mirrors = true, splits = true, taps = 4, max_rarity = 0.06f;
    else if (n <= 30) rings = 4, mirrors = true, splits = true, taps = n <= 22 ? 5 : 6, max_rarity = 0.04f;
    else rings = 4, mirrors = true, splits = true, taps = std::min(9, 6 + (n - 31) / 10), max_rarity = 0.02f;
}

// The same level for everybody: the generator is seeded by the level number.
void makeLevel(int n, Level &out)
{
    for (const Campaign &c : CAMPAIGN)
        if (c.n == n) {
            out = c.lvl;
            return;
        }
    int rings, taps;
    bool mirrors, splits;
    float max_rarity;
    tier(n, rings, mirrors, splits, taps, max_rarity);
    Rng rng{uint32_t(n) * 2654435761u + 12345u};
    const int64_t t0 = esp_timer_get_time();
    Level best_try{};
    int best_gap = 99;
    for (int attempt = 0; attempt < 400; attempt++) {
        Level L{};
        L.n_rings = uint8_t(rings);
        for (int i = 0; i < rings; i++) {
            const int open = 3 + rng.below(2);   // 3..4 holes per ring
            bool split_used = false;
            for (int k = 0; k < open; k++) {
                int s;
                do s = rng.below(8);
                while (L.ring[i][s] != BLOCK);
                uint8_t el = GAP;
                const int pick = rng.below(10);
                if (mirrors && pick >= 4) el = pick & 1 ? MIRROR_L : MIRROR_R;
                if (splits && !split_used && pick == 9) el = SPLIT, split_used = true;
                L.ring[i][s] = el;
            }
        }
        const int winners = solve(L);
        int total = 1;
        for (int i = 0; i < rings; i++) total *= 8;
        const float rarity = float(winners) / total;
        // After a while, loosen the rarity ceiling rather than give up on the tap target.
        const float ceiling = max_rarity * (attempt < 200 ? 1.0f : 2.0f);
        if (winners == 0 || rarity > ceiling) continue;
        // Any opening at exactly `taps` from a win will do; pick one at random.
        int count = 0;
        for (int v = 0; v < total; v++) count += s_dist[v] == taps;
        if (count == 0) {
            // Remember the nearest miss in case nothing better turns up.
            for (int d = taps - 1; d >= 1 && best_gap > taps - d; d--)
                for (int v = 0; v < total; v++)
                    if (s_dist[v] == d) {
                        best_try = L;
                        unpack(v, best_try.start, rings);
                        best_try.best = uint8_t(d);
                        best_gap = taps - d;
                        break;
                    }
            continue;
        }
        int pick = rng.below(count);
        for (int v = 0; v < total; v++)
            if (s_dist[v] == taps && pick-- == 0) {
                unpack(v, L.start, rings);
                break;
            }
        L.best = uint8_t(taps);
        out = L;
        ESP_LOGI(TAG, "level %d: %d rings, %d taps, rarity %d/%d, %d tries, %lld ms", n, rings, taps, winners, total,
                 attempt + 1, (esp_timer_get_time() - t0) / 1000);
        return;
    }
    ESP_LOGW(TAG, "level %d: settled for %d taps (wanted %d)", n, best_try.best, taps);
    out = best_gap < 99 ? best_try : CAMPAIGN[5].lvl;
}

// Acceptance tests from the spec (section 14), run once at start-up and logged.
void selfTest()
{
    int ok = 0;
    for (const Campaign &c : CAMPAIGN) {
        solve(c.lvl);
        const int d = s_dist[pack(c.lvl.start, c.lvl.n_rings)];
        const bool opens_locked = !wins(c.lvl, c.lvl.start, 0xFF);
        if (d == c.lvl.best && opens_locked) ok++;
        else ESP_LOGE(TAG, "level %d fails verification: min taps %d (spec %d), locked at start %d", c.n, d,
                      c.lvl.best, opens_locked);
    }
    ESP_LOGI(TAG, "campaign verified: %d/%d levels match their best tap count", ok, int(sizeof(CAMPAIGN) / sizeof(CAMPAIGN[0])));
}

namespace sfx {
using wc::audio::Tone;
using wc::audio::Wave;
void click() { wc::audio::play({.f0 = 800, .f1 = 500, .ms = 40, .wave = Wave::Square, .volume = 0.5f}); }
void detent() { wc::audio::play({.f0 = 1200, .ms = 25, .wave = Wave::Square, .volume = 0.22f}); }
void deeper(int depth)
{
    const float f = 520.0f + 130.0f * depth;
    wc::audio::play({.f0 = f, .f1 = f * 1.35f, .ms = 90, .wave = Wave::Triangle, .volume = 0.5f});
}
void solve()
{
    const Tone t[] = {{.f0 = 523, .ms = 130, .wave = Wave::Square, .volume = 0.6f, .delay_ms = 0},
                      {.f0 = 659, .ms = 130, .wave = Wave::Square, .volume = 0.6f, .delay_ms = 140},
                      {.f0 = 784, .ms = 130, .wave = Wave::Square, .volume = 0.6f, .delay_ms = 280},
                      {.f0 = 1047, .ms = 220, .wave = Wave::Square, .volume = 0.7f, .delay_ms = 420}};
    wc::audio::play(t, 4);
}
}  // namespace sfx

struct Seg {
    float x0, y0, x1, y1;
};
struct Pt {
    float x, y;
};
Pt polar(float r, float spoke)
{
    const float a = (spoke * 45.0f - 90.0f) * PI / 180.0f;
    return {C + r * std::cos(a), C + r * std::sin(a)};
}

enum Phase { TUTORIAL, PLAYING, SOLVED };
const char *const CHEERS[] = {"NICE!", "YES!", "GOT IT!", "LOVELY!", "BRIGHT!", "WAHOO!"};

}  // namespace

struct Star::State {
    // ---- progress (NVS "sleepystar")
    int level = 1, depth = 1;
    bool seen_tut = false, flat_play = false;

    // ---- play
    Level lvl{};
    uint8_t rot[MAX_RINGS] = {};
    int entry = 0;
    int taps = 0;
    Phase phase = PLAYING;
    float phase_t = 0;
    int reached = 0;               // how many rings the beam got through on the last trace
    bool won = false;
    Seg segs[32];
    int n_segs = 0;
    Pt stops[12];
    int n_stops = 0;

    // ---- ring tween: both rings step to their new angle in 3 jumps over 260 ms
    float vis_from[MAX_RINGS] = {}, vis_to[MAX_RINGS] = {};
    float tween_t = 1;

    // ---- tilt (spec section 5)
    float up = -PI / 2;            // filtered screen angle of world-up
    bool have_up = false;
    float flat_t = 0;
    bool nudge = false;
    float gyro_sign = 1, sign_score = 0, last_target = 0;
    bool had_target = false;

    // ---- ui
    Gestures ges;
    bool menu = false, menu_dirty = false;
    bool dirty = true;
    int64_t last_tap_us = 0;
    int stars_earned = 0;

    // ---- drawing
    Canvas canvas;
    uint8_t c_void = 0, c_disc = 0, c_bezel = 0, c_plate[4] = {}, c_tooth[4] = {}, c_lip = 0, c_mirror = 0, c_split = 0;
    uint8_t c_beam = 0, c_glow = 0, c_white = 0, c_asleep = 0, c_awake = 0, c_halo = 0, c_door = 0, c_door_lit = 0;
    uint8_t c_face_lit = 0, c_text = 0, c_dim = 0, c_stop = 0, c_emit = 0, c_emit_hi = 0, c_panel = 0, c_rays = 0;
    uint8_t c_orange = 0, c_pip_edge = 0, c_cyan = 0;

    // ------------------------------------------------------------------ persistence
    void load()
    {
        nvs_handle_t h;
        if (nvs_open("sleepystar", NVS_READONLY, &h) != ESP_OK) return;
        uint16_t d;
        uint8_t v;
        if (nvs_get_u16(h, "depth", &d) == ESP_OK && d >= 1) depth = d;
        if (nvs_get_u16(h, "level", &d) == ESP_OK && d >= 1) level = std::min<int>(d, depth);
        if (nvs_get_u8(h, "seen_tut", &v) == ESP_OK) seen_tut = v;
        if (nvs_get_u8(h, "flat", &v) == ESP_OK) flat_play = v;
        nvs_close(h);
    }
    void save()
    {
        nvs_handle_t h;
        if (nvs_open("sleepystar", NVS_READWRITE, &h) != ESP_OK) return;
        nvs_set_u16(h, "depth", uint16_t(depth));
        nvs_set_u16(h, "level", uint16_t(level));
        nvs_set_u8(h, "seen_tut", seen_tut);
        nvs_set_u8(h, "flat", flat_play);
        nvs_commit(h);
        nvs_close(h);
    }
    void saveBest(int n, int used)
    {
        nvs_handle_t h;
        if (nvs_open("sleepystar", NVS_READWRITE, &h) != ESP_OK) return;
        char key[16];
        snprintf(key, sizeof(key), "best_%d", n);
        uint8_t prev = 255;
        nvs_get_u8(h, key, &prev);
        if (used < prev) nvs_set_u8(h, key, uint8_t(std::min(used, 250)));
        nvs_commit(h);
        nvs_close(h);
    }

    bool loadAssets()
    {
        if (!canvas.init(SCALE)) return false;
        auto col = [&](uint32_t hex) { return canvas.color(rgb((hex >> 16) & 255, (hex >> 8) & 255, hex & 255)); };
        c_void = col(0x0E0E1C);
        c_disc = col(0x16162A);
        c_bezel = col(0x1E2438);
        const uint32_t plate[4] = {0x2E3550, 0x343C5C, 0x3A4368, 0x404A74};
        const uint32_t tooth[4] = {0x414B70, 0x48537C, 0x4F5B88, 0x566394};
        for (int i = 0; i < 4; i++) c_plate[i] = col(plate[i]), c_tooth[i] = col(tooth[i]);
        c_lip = col(0x4A5580);
        c_mirror = col(0xDCEBFF);
        c_split = col(0x7FE8FF);
        c_beam = col(0xFF4DD2);
        c_glow = col(0x5C2458);      // the beam's soft halo over the dark disc
        c_white = col(0xFFFFFF);
        c_asleep = col(0x3C4266);
        c_awake = col(0xFFD93D);
        c_halo = col(0x4A4226);
        c_door = col(0x6E7BA8);
        c_door_lit = col(0xFFF3A0);
        c_face_lit = col(0x8A5A00);
        c_text = col(0xEAF0FF);
        c_dim = col(0x8A97C0);
        c_stop = col(0x6E7BA9);
        c_emit = col(0x3A4260);
        c_emit_hi = col(0x8A97C1);
        c_panel = col(0x07070F);
        c_rays = col(0xFFF3A1);
        c_orange = col(0xFF9A4D);
        c_pip_edge = col(0x3A4261);
        c_cyan = col(0x7FE8FE);
        return true;
    }

    // ------------------------------------------------------------------ level flow
    void startLevel(int n)
    {
        level = std::max(1, n);
        makeLevel(level, lvl);
        std::memcpy(rot, lvl.start, sizeof(rot));
        for (int i = 0; i < MAX_RINGS; i++) vis_from[i] = vis_to[i] = rot[i];
        tween_t = 1;
        taps = 0;
        reached = 0;
        phase = (level == 1 && !seen_tut) ? TUTORIAL : PLAYING;
        phase_t = 0;
        retrace(false);
        dirty = true;
        save();
    }

    // Beam walk with geometry for drawing (spec section 4.2).
    void retrace(bool sounds)
    {
        const Geo &G = GEO[lvl.n_rings];
        n_segs = n_stops = 0;
        struct B {
            int ring;
            float from_r;
            int from_s, meet_s;
        } cur[8], nxt[8];
        int n_cur = 1, deepest = 0;
        cur[0] = {0, EMIT_R, entry, entry};
        bool win = false;
        for (int guard = 0; n_cur > 0 && guard < 16; guard++) {
            int n_nxt = 0;
            for (int i = 0; i < n_cur; i++) {
                const B b = cur[i];
                const float R = G.rad[b.ring];
                const Pt p0 = polar(b.from_r, float(b.from_s)), p1 = polar(R, float(b.meet_s));
                if (n_segs < 32) segs[n_segs++] = {p0.x, p0.y, p1.x, p1.y};
                const uint8_t el = lvl.ring[b.ring][m8(b.meet_s - rot[b.ring])];
                const uint8_t mask = outs(el, b.meet_s);
                if (!mask) {
                    if (n_stops < 12) stops[n_stops++] = p1;
                    continue;
                }
                deepest = std::max(deepest, b.ring + 1);
                for (int s = 0; s < SPOKES; s++) {
                    if (!(mask & (1u << s))) continue;
                    if (b.ring == lvl.n_rings - 1) {
                        const Pt e = polar(G.star, float(s));
                        if (n_segs < 32) segs[n_segs++] = {p1.x, p1.y, e.x, e.y};
                        if (s == DOOR) {
                            win = true;
                            if (n_segs < 32) segs[n_segs++] = {e.x, e.y, C, C};
                        } else if (n_stops < 12) {
                            stops[n_stops++] = e;
                        }
                    } else if (n_nxt < 8) {
                        nxt[n_nxt++] = {b.ring + 1, R, b.meet_s, s};
                    }
                }
            }
            std::memcpy(cur, nxt, sizeof(B) * n_nxt);
            n_cur = n_nxt;
        }
        // "Getting warmer": a rising blip whenever the light gets one ring deeper than before.
        if (sounds && deepest > reached && !win) sfx::deeper(deepest);
        reached = deepest;
        if (win && !won && phase == PLAYING) {
            phase = SOLVED;
            phase_t = 0;
            stars_earned = taps <= lvl.best ? 3 : taps <= lvl.best + 2 ? 2 : 1;
            saveBest(level, taps);
            if (level + 1 > depth) depth = level + 1;
            save();
            sfx::solve();
        }
        won = win;
        dirty = true;
    }

    void tapRing(int idx)
    {
        for (int i = 0; i < MAX_RINGS; i++) vis_from[i] = vis_to[i];
        rot[idx] = uint8_t(m8(rot[idx] + 1));
        vis_to[idx] += 1;
        if (idx < lvl.n_rings - 1) {
            rot[idx + 1] = uint8_t(m8(rot[idx + 1] - 1));   // the ring inside turns the other way
            vis_to[idx + 1] -= 1;
        }
        tween_t = 0;
        taps++;
        sfx::click();
        retrace(true);
    }

    // ------------------------------------------------------------------ tilt -> entry spoke
    static float wrapPi(float a)
    {
        while (a > PI) a -= TAU;
        while (a < -PI) a += TAU;
        return a;
    }

    void updateTilt(const InputState &in, float dt)
    {
        const float ax = in.tilt.ax, ay = in.tilt.ay;
        const float g_mag = std::sqrt(ax * ax + ay * ay);
        const float gz = in.tilt.gz * (PI / 180.0f) * dt;
        if (g_mag >= 0.25f) {
            const float target = std::atan2(-ay, -ax);   // screen direction of world-up
            if (!have_up) up = target, have_up = true;
            // Learn the gyro's sign from gravity, for flat play later.
            if (had_target && std::fabs(gz) > 0.004f) {
                sign_score += wrapPi(target - last_target) * (-gyro_sign * gz) * 400;
                if (sign_score < -1.0f) gyro_sign = -gyro_sign, sign_score = 0;
                sign_score = std::min(sign_score, 3.0f);
            }
            last_target = target;
            had_target = true;
            up = wrapPi(up + wrapPi(target - up) * std::min(1.0f, dt / 0.12f));
            flat_t = 0;
        } else {
            had_target = false;
            flat_t += dt;
            // Lying flat there's no in-plane gravity. Hold the last angle, or follow the
            // gyro if FLAT PLAY is on (it drifts, which is why it's a setting).
            if (flat_play) up = wrapPi(up - gyro_sign * gz);
        }
        const bool want_nudge = flat_t > 1.0f && !flat_play;
        if (want_nudge != nudge) nudge = want_nudge, dirty = true;

        // Snap to a spoke with 6 degrees of hysteresis so it can't chatter on a boundary.
        const float deg = up * 180.0f / PI + 90.0f;
        const float centre = entry * 45.0f;
        float off = std::fmod(deg - centre + 540.0f, 360.0f) - 180.0f;
        if (std::fabs(off) > 22.5f + 6.0f) {
            entry = m8(int(std::lround(deg / 45.0f)));
            sfx::detent();
            retrace(true);
        }
    }

    // ------------------------------------------------------------------ update
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
        if (ges.swipe_left) {
            menu = true;
            menu_dirty = true;
            return;
        }
        if (tween_t < 1) {
            tween_t = std::min(1.0f, tween_t + dt / 0.26f);
            dirty = true;
        }
        if (phase == SOLVED && phase_t < 0.6f) dirty = true;   // banner pop

        if (phase == TUTORIAL) {
            if (ges.tap) {
                seen_tut = true;
                save();
                phase = PLAYING;
                dirty = true;
            }
            return;
        }
        updateTilt(in, dt);

        if (phase == SOLVED) {
            if ((ges.tap && phase_t > 0.8f) || phase_t > 4.0f) {
                won = false;
                startLevel(level + 1);
            }
            return;
        }
        if (in.clicked & BTN_B) {   // PWR: start the level over
            won = false;
            startLevel(level);
            return;
        }
        if (ges.tap) {
            const int64_t now = e.nowUs();
            if (now - last_tap_us < 120000) return;   // debounce
            last_tap_us = now;
            // The reset button first, then the rings (spec section 6); positions in device px.
            if (std::abs(ges.x - 51) <= 26 && std::abs(ges.y - 233) <= 25) {
                won = false;
                startLevel(level);
                return;
            }
            const Geo &G = GEO[lvl.n_rings];
            const float r = std::hypot(ges.x - 233.0f, ges.y - 233.0f) / SCALE;
            if (r < G.star + 3 || r > G.rad[0] + 10) return;   // the star isn't a button; nor is the bezel
            int hit = -1;
            float best = 8.0f;   // 16 device px either side of the band's centre
            for (int i = 0; i < lvl.n_rings; i++)
                if (std::fabs(r - G.rad[i]) <= best) best = std::fabs(r - G.rad[i]), hit = i;
            if (hit >= 0) tapRing(hit);
        }
    }

    // ------------------------------------------------------------------ menu
    void closeMenu()
    {
        menu = false;
        dirty = true;
    }

    void menuTap(Engine &e, int x, int y)
    {
        namespace ui = console::ui;
        if (ui::rowRect(0).hit(x, y)) {
            won = false;
            startLevel(level >= depth ? 1 : level + 1);   // step through the levels reached so far
            menu_dirty = true;
        } else if (ui::rowRect(1).hit(x, y)) {
            wc::audio::setVolume(wc::audio::volume() == 0 ? 2 : 0);
            if (wc::audio::volume() > 0) sfx::click();
            menu_dirty = true;
        } else if (ui::rowRect(2).hit(x, y)) {
            flat_play = !flat_play;
            save();
            menu_dirty = true;
        } else if (ui::rowRect(3).hit(x, y)) {
            won = false;
            startLevel(level);
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
        ui::clearScreen(g);
        ui::title(g, "PAUSED");
        char buf[16];
        snprintf(buf, sizeof(buf), "%d / %d", level, depth);
        ui::row(g, 0, "LEVEL", buf);
        ui::row(g, 1, "SOUND", wc::audio::volume() ? "ON" : "OFF", wc::audio::volume() ? ui::GO : ui::DIM);
        ui::row(g, 2, "FLAT PLAY", flat_play ? "GYRO" : "OFF", flat_play ? ui::VALUE : ui::DIM);
        ui::row(g, 3, "RESET LEVEL", "GO", ui::ACCENT);
        ui::button(g, ui::buttonRect(0, 2), "RESUME");
        ui::outlineButton(g, ui::buttonRect(1, 2), "HOME");
    }

    // ------------------------------------------------------------------ drawing
    float visRot(int i) const
    {
        // Mechanical, not smooth: three discrete jumps over the tween.
        const float step = std::min(1.0f, std::floor(tween_t * 3.0f + 1.0f) / 3.0f);
        return vis_from[i] + (vis_to[i] - vis_from[i]) * (tween_t >= 1 ? 1.0f : step);
    }

    // Background, bands, teeth, holes and glyphs in one pass over the canvas.
    void drawBoard(Canvas &cv)
    {
        const Geo &G = GEO[lvl.n_rings];
        int32_t rot16[MAX_RINGS];
        for (int i = 0; i < lvl.n_rings; i++) rot16[i] = int32_t(visRot(i) * 8192.0f);
        uint8_t *px = cv.pixels();
        for (int y = 0; y < CW; y++) {
            for (int x = 0; x < CW; x++) {
                const int idx = (2 * y) * Gfx::W + 2 * x;
                const float r = Polar::radius16(idx) / 32.0f;
                uint8_t c = r < DISC_R ? c_disc : (r > BEZEL_R && r < BEZEL_R + 1.5f) ? c_bezel : c_void;
                if (r < G.rad[0] + 10 && r > G.star) {
                    for (int k = 0; k < lvl.n_rings; k++) {
                        const float dr = r - G.rad[k];
                        if (dr < -BAND_HALF || dr > 9.5f) continue;
                        // Angle in the ring's own frame, 0 at its spoke 0.
                        const uint16_t a16 = uint16_t(int32_t(Polar::angle(idx)) + 16384 - rot16[k]);
                        if (dr <= BAND_HALF) {
                            c = c_plate[k];
                            const int spoke = ((a16 + 4096) >> 13) & 7;
                            const uint8_t el = lvl.ring[k][spoke];
                            if (el != BLOCK) {
                                const int16_t off = int16_t(uint16_t(a16 + 4096 - (spoke << 13))) - 4096;
                                const float tang = off * (TAU / 65536.0f) * r;   // + = clockwise
                                if (std::fabs(tang) <= HOLE_HALF) {
                                    c = c_disc;   // a hole: the wall is cut here
                                    // Glyphs are 5x5 grids of 2 px blocks; x runs outward, y clockwise.
                                    const int bx = int(std::floor((dr + 5.0f) / 2.0f)), by = int(std::floor((tang + 5.0f) / 2.0f));
                                    const bool in_grid = bx >= 0 && bx < 5 && by >= 0 && by < 5;
                                    if (el == GAP) {
                                        if (std::fabs(tang) >= 5.25f) c = c_lip;
                                    } else if (el == MIRROR_L) {
                                        if (in_grid && bx == by) c = c_mirror;
                                    } else if (el == MIRROR_R) {
                                        if (in_grid && bx + by == 4) c = c_mirror;
                                    } else if (in_grid && bx + by == 4 && bx != 2) {
                                        c = c_split;
                                    }
                                }
                            }
                        } else if (dr >= 6.0f) {
                            // Teeth, so rotation is visible: every tooth index that isn't a spoke.
                            const int T = G.teeth[k];
                            const uint32_t t16 = uint32_t(a16) * T + 32768;
                            const int ti = int(t16 >> 16) % T;
                            if (ti % (T / 8) != 0) {
                                const float toff = (int32_t(t16 & 0xFFFF) - 32768) / 65536.0f * (TAU * r / T);
                                if (std::fabs(toff) <= 2.5f && dr <= 9.0f) c = c_tooth[k];
                            }
                        }
                        break;
                    }
                }
                px[y * CW + x] = c;
            }
        }
    }

    void octagon(Canvas &cv, float R, uint8_t col)
    {
        const int ri = int(std::ceil(R));
        for (int dy = -ri; dy <= ri; dy++)
            for (int dx = -ri; dx <= ri; dx++)
                if (std::abs(dx) <= R && std::abs(dy) <= R && std::abs(dx) + std::abs(dy) <= R * 1.414f)
                    cv.pixel(int(C) + dx, int(C) + dy, col);
    }

    void thickLine(Canvas &cv, const Seg &s, int w, uint8_t col)
    {
        const float dx = s.x1 - s.x0, dy = s.y1 - s.y0, len = std::max(0.001f, std::hypot(dx, dy));
        const float nx = -dy / len, ny = dx / len;
        for (int k = 0; k < w; k++) {
            const float o = k - (w - 1) / 2.0f;
            cv.line(int(std::lround(s.x0 + nx * o)), int(std::lround(s.y0 + ny * o)), int(std::lround(s.x1 + nx * o)),
                    int(std::lround(s.y1 + ny * o)), col);
        }
    }

    // A rectangle in the emitter's frame: u runs outward along the spoke, v across it.
    void spokeRect(Canvas &cv, float ang, float u0, float u1, float half_v, uint8_t col)
    {
        const float ca = std::cos(ang), sa = std::sin(ang);
        const int x0 = int(C + ca * (u0 + u1) / 2 - 12), y0 = int(C + sa * (u0 + u1) / 2 - 12);
        for (int y = y0; y < y0 + 24; y++)
            for (int x = x0; x < x0 + 24; x++) {
                const float rx = x + 0.5f - C, ry = y + 0.5f - C;
                const float u = rx * ca + ry * sa, v = -rx * sa + ry * ca;
                if (u >= u0 && u <= u1 && std::fabs(v) <= half_v) cv.pixel(x, y, col);
            }
    }

    void drawStar(Canvas &cv)
    {
        const Geo &G = GEO[lvl.n_rings];
        const bool awake = won;
        if (awake) octagon(cv, G.star + 9, c_halo);
        octagon(cv, G.star, awake ? c_awake : c_asleep);
        // The door: a bright notch on the star's edge at board spoke 4 (straight down).
        const Pt d = polar(G.star, DOOR);
        cv.fillRect(int(d.x) - 5, int(d.y) - 3, 11, 7, awake ? c_door_lit : c_door);
        // Face: shut eyes and a small mouth asleep; open eyes and a smile awake.
        const int u = std::max(1, int(std::lround(G.star / 7)));
        const int cx = int(C), cy = int(C);
        const uint8_t f = awake ? c_face_lit : c_disc;
        if (awake) {
            cv.fillRect(cx - 4 * u, cy - 2 * u, 2 * u, 2 * u, f);
            cv.fillRect(cx + 2 * u, cy - 2 * u, 2 * u, 2 * u, f);
            cv.fillRect(cx - 3 * u, cy + u, 6 * u, u, f);
            cv.fillRect(cx - 4 * u, cy, u, u, f);
            cv.fillRect(cx + 3 * u, cy, u, u, f);
            for (int s = 0; s < 8; s++) {
                const Pt p = polar(G.star + 7, float(s));
                cv.fillRect(int(p.x) - 2, int(p.y) - 2, 5, 5, c_rays);
            }
        } else {
            cv.fillRect(cx - 4 * u, cy - u, 2 * u, u, f);
            cv.fillRect(cx + 2 * u, cy - u, 2 * u, u, f);
            cv.fillRect(cx - u, cy + 2 * u, 2 * u, u, f);
        }
    }

    void banner(Canvas &cv, const char *top, const char *mid, const char *bottom, uint8_t col)
    {
        const int h = bottom ? 40 : 30;
        cv.fillRect(20, 96, CW - 40, h, c_panel);
        cv.fillRect(20, 96, CW - 40, 2, col);
        cv.fillRect(20, 96 + h - 2, CW - 40, 2, col);
        cv.textCentered(int(C), 108, top, col, 2, true);
        if (mid) cv.textCentered(int(C), 122, mid, c_text, 1, false);
        if (bottom) cv.textCentered(int(C), 131, bottom, c_awake, 1, true);
    }

    void draw(Engine &e, Gfx &g)
    {
        if (menu) {
            if (menu_dirty) {
                drawMenu(g);
                menu_dirty = false;
            }
            return;
        }
        // Turn-based: nothing moves unless something changed, so don't redraw (or send a frame).
        if (!dirty) return;
        dirty = false;

        Canvas &cv = canvas;
        drawBoard(cv);
        drawStar(cv);
        for (int i = 0; i < n_segs; i++) thickLine(cv, segs[i], 5, c_glow);
        for (int i = 0; i < n_segs; i++) thickLine(cv, segs[i], 3, c_beam);
        for (int i = 0; i < n_segs; i++) thickLine(cv, segs[i], 1, c_white);
        for (int i = 0; i < n_stops; i++) {
            cv.fillRect(int(stops[i].x) - 2, int(stops[i].y) - 2, 5, 5, c_stop);
            cv.fillRect(int(stops[i].x) - 7, int(stops[i].y) - 1, 3, 3, c_stop);
            cv.fillRect(int(stops[i].x) + 5, int(stops[i].y) - 1, 3, 3, c_stop);
        }
        // The emitter sits on the rim wherever world-up is, pointing in.
        const float ea = (entry * 45.0f - 90.0f) * PI / 180.0f;
        spokeRect(cv, ea, 88, 103, 7.5f, c_emit);
        spokeRect(cv, ea, 96, 100, 5.0f, c_emit_hi);
        spokeRect(cv, ea, 82, 88, 3.8f, c_beam);
        spokeRect(cv, ea, 78, 82, 2.0f, c_white);

        // HUD: level, taps against best, progress pips, reset.
        char buf[24];
        snprintf(buf, sizeof(buf), "LVL %d", level);
        cv.text(38, 42, buf, c_text, 1, true);
        snprintf(buf, sizeof(buf), "%d/%d", taps, lvl.best);
        cv.text(195 - Gfx::textWidth(buf, 1, true), 42, buf, taps > lvl.best ? c_orange : c_dim, 1, true);
        const int n_pips = lvl.n_rings + 1, pw = n_pips * 9 - 3;
        for (int i = 0; i < n_pips; i++) {
            const bool star_pip = i == lvl.n_rings;
            const bool lit = star_pip ? won : reached > i;
            const int x = int(C) - pw / 2 + i * 9;
            cv.fillRect(x, 192, 6, 6, lit ? (star_pip ? c_awake : c_beam) : c_disc);
            cv.rect(x, 192, 6, 6, lit ? (star_pip ? c_awake : c_beam) : (star_pip ? c_awake : c_pip_edge));
        }
        cv.fillRect(13, 105, 25, 23, c_panel);
        cv.rect(13, 105, 25, 23, c_emit);
        cv.textCentered(25, 117, "RST", c_dim, 1, false);
        if (nudge) cv.textCentered(int(C), 208, "TILT ME", c_cyan, 1, true);

        if (phase == TUTORIAL) {
            cv.fillRect(24, 50, CW - 48, 134, c_panel);
            cv.rect(24, 50, CW - 48, 134, c_beam);
            cv.rect(25, 51, CW - 50, 132, c_beam);
            cv.textCentered(int(C), 63, "WAKE THE STAR", c_beam, 1, true);
            cv.text(32, 78, "1", c_cyan, 1, true);
            cv.text(44, 78, "THE LASER ALWAYS FALLS", c_text, 1, false);
            cv.text(44, 88, "STRAIGHT DOWN. TURN YOUR", c_text, 1, false);
            cv.text(44, 98, "WRIST AND IT MOVES.", c_text, 1, false);
            cv.text(32, 114, "2", c_beam, 1, true);
            cv.text(44, 114, "TAP A RING TO TURN IT", c_text, 1, false);
            cv.text(44, 124, "ONE NOTCH.", c_text, 1, false);
            cv.text(32, 140, "3", c_awake, 1, true);
            cv.text(44, 140, "THE RING INSIDE IT TURNS", c_text, 1, false);
            cv.text(44, 150, "THE OTHER WAY.", c_text, 1, false);
            cv.textCentered(int(C), 172, "TAP: LET'S GO", c_beam, 1, true);
        } else if (phase == SOLVED && phase_t > 0.25f) {
            char mid[64], stars[8] = "";
            if (taps <= lvl.best) snprintf(mid, sizeof(mid), "PERFECT - %d TAP%s", taps, taps == 1 ? "" : "S");
            else snprintf(mid, sizeof(mid), "DONE IN %d - BEST IS %d", taps, lvl.best);
            for (int i = 0; i < stars_earned; i++) std::strcat(stars, "* ");
            banner(cv, CHEERS[level % 6], mid, stars, c_awake);
        }
        cv.present(e.presenter());
    }
};

Star::Star() : s_(new State) {}
Star::~Star() { delete s_; }

void Star::begin(Engine &e)
{
    if (!Polar::init()) ESP_LOGE(TAG, "polar tables alloc failed");
    if (!s_->loadAssets()) ESP_LOGE(TAG, "no memory for the canvas");
    selfTest();
    s_->load();
    s_->startLevel(s_->level);
}

void Star::enter(Engine &e)
{
    s_->menu = false;
    s_->dirty = true;
}

void Star::redraw() { s_->dirty = s_->menu_dirty = true; }

void Star::update(Engine &e, float dt) { s_->update(e, std::min(dt, 0.1f)); }

void Star::draw(Engine &e, Gfx &g)
{
    if (!s_->canvas.pixels()) return;
    s_->draw(e, g);
}

}  // namespace games
