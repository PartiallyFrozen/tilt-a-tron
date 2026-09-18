#include "games/racer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "audio/audio.h"
#include "console/pause_menu.h"
#include "console/ui.h"
#include "engine/canvas.h"
#include "engine/gestures.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "nvs.h"

namespace wc {
extern const uint8_t kFont5x7[][5];
}

// Sprite sheets, embedded from components/games/assets/racer/ (see tools/make_sprites.py).
extern "C" {
extern const uint8_t _binary_car_png_start[], _binary_car_png_end[];
extern const uint8_t _binary_trees_png_start[], _binary_trees_png_end[];
extern const uint8_t _binary_sign_png_start[], _binary_sign_png_end[];
extern const uint8_t _binary_stand_png_start[], _binary_stand_png_end[];
extern const uint8_t _binary_bush_png_start[], _binary_bush_png_end[];
}

using namespace wc;

namespace games {

namespace {

const char *TAG = "racer";

// The scene is drawn upright into a small 8-bit picture, then rotated (to cancel
// the watch's roll) and doubled onto the round screen.
// 256 wide: a little bigger than the 233 px circle so the rotated corners have
// something to show.
constexpr int VW = 256, VH = 256, VC = 128;
constexpr int HORIZON = 108;

// Road model (after the classic pseudo-3D racers).
constexpr float SEG_LEN = 200.0f;
constexpr int RUMBLE_LEN = 3;          // segments per kerb stripe
constexpr float ROAD_W = 2000.0f;      // half-width of the road in world units
constexpr float CAM_H = 1000.0f;
constexpr float CAM_DEPTH = 0.84f;     // 1 / tan(fov / 2), ~100 degree view
constexpr int DRAW_DIST = 110;
constexpr float MAX_SPEED = SEG_LEN * 60.0f;
constexpr float ACCEL = MAX_SPEED / 5.0f;
constexpr float BRAKE = -MAX_SPEED;
constexpr float OFFROAD_DECEL = -MAX_SPEED / 2.0f;
constexpr float OFFROAD_LIMIT = MAX_SPEED / 4.0f;
constexpr float CENTRIFUGAL = 0.18f;   // how hard corners push you wide
constexpr float PLAYER_Z = CAM_H * CAM_DEPTH;
constexpr int LAPS = 3, RIVALS = 5;
constexpr float FULL_LOCK = 0.70f;     // radians of wheel for full steering (~40 degrees)
constexpr float STEER_RATE = 3.0f;     // how fast the car moves across the road at full lock:
                                       // comfortably more than the sharpest corner pushes back

enum Pal : uint8_t {
    P_BLACK, P_WHITE,
    P_SKY0, P_SKY1, P_SKY2, P_SKY3, P_SKY4, P_SKY5, P_SKY6, P_SKY7,   // top of the sky to the horizon
    P_SUN, P_SUN_GLOW, P_CLOUD, P_MOUNTAIN_FAR, P_MOUNTAIN, P_GRASS_A, P_GRASS_B, P_ROAD_A, P_ROAD_B,
    P_KERB_RED, P_KERB_WHITE, P_LANE, P_TIRE, P_TIRE_LIT, P_WING, P_GREY, P_HELMET, P_YELLOW, P_RED, P_GREEN,
    P_PANEL, P_FLAME_A, P_FLAME_B, P_TRUNK, P_LEAF, P_LEAF_DARK, P_SIGN, P_SIGN_STRIPE,
    // Car liveries: body colors, then the matching shade at the same offset.
    P_CAR0, P_CAR1, P_CAR2, P_CAR3, P_CAR4, P_CAR_PLAYER,
    P_CAR0_D, P_CAR1_D, P_CAR2_D, P_CAR3_D, P_CAR4_D, P_CAR_PLAYER_D,
    P_COUNT
};
constexpr int CAR_SHADE = P_CAR0_D - P_CAR0;

constexpr Color kPalette[P_COUNT] = {
    rgb(0, 0, 0), rgb(255, 255, 255),
    rgb(24, 58, 140), rgb(36, 78, 162), rgb(52, 100, 184), rgb(74, 124, 204), rgb(102, 150, 220),
    rgb(136, 176, 232), rgb(172, 202, 242), rgb(208, 226, 248),
    rgb(255, 244, 190), rgb(250, 226, 150), rgb(244, 248, 255), rgb(104, 124, 160), rgb(64, 78, 104),
    rgb(38, 140, 52), rgb(30, 122, 44), rgb(98, 98, 104), rgb(88, 88, 94),
    rgb(214, 40, 40), rgb(240, 240, 240), rgb(235, 235, 235), rgb(22, 22, 26), rgb(62, 62, 70),
    rgb(34, 34, 42), rgb(112, 116, 126), rgb(255, 214, 40), rgb(255, 220, 40), rgb(255, 50, 50), rgb(40, 220, 110),
    rgb(14, 16, 24), rgb(255, 150, 30), rgb(255, 232, 90), rgb(92, 60, 30), rgb(40, 150, 60), rgb(26, 104, 44),
    rgb(244, 244, 248), rgb(220, 40, 50),
    rgb(40, 120, 255), rgb(255, 204, 30), rgb(40, 190, 90), rgb(170, 80, 220), rgb(255, 130, 30), rgb(225, 30, 40),
    rgb(22, 72, 170), rgb(190, 146, 14), rgb(22, 128, 56), rgb(112, 46, 152), rgb(186, 86, 12), rgb(150, 16, 26),
};

// The car sprite is 32 x 16 in the sheet; its blue livery colours (P_CAR0 and
// P_CAR0_D) are swapped for each car's own at draw time.
constexpr int CAR_W = 32, CAR_H = 16;

float frand() { return esp_random() / 4294967296.0f; }
float clampf(float v, float a, float b) { return std::max(a, std::min(b, v)); }
float easeIn(float a, float b, float t) { return a + (b - a) * t * t; }
float easeInOut(float a, float b, float t) { return a + (b - a) * ((-std::cos(t * 3.14159265f) / 2) + 0.5f); }

struct Segment {
    float curve;
};

struct Car {
    float z, offset, speed;
    int lap;
    uint8_t color;
};

struct Projected {
    float x, y, w, scale;
};

// Things beside the track. offset is in road half-widths (beyond +-1 is off the road).
enum PropKind : uint8_t { PROP_TREE, PROP_SIGN, PROP_GANTRY, PROP_STAND, PROP_BUSH };
struct Prop {
    int seg;
    float offset;
    PropKind kind;
};

enum Phase { READY, COUNTDOWN, RACING, FINISHED };

namespace sfx {
using wc::audio::Wave;
void beep(bool go) { wc::audio::play({.f0 = go ? 1040.0f : 520.0f, .ms = uint16_t(go ? 420 : 160), .wave = Wave::Square, .volume = 0.7f}); }
void bump() { wc::audio::play({.f0 = 170, .f1 = 70, .ms = 110, .wave = Wave::Noise, .volume = 0.7f}); }
void kerb() { wc::audio::play({.f0 = 120, .f1 = 90, .ms = 30, .wave = Wave::Noise, .volume = 0.22f}); }
void lap()
{
    const wc::audio::Tone t[] = {{.f0 = 784, .ms = 80, .wave = Wave::Square, .volume = 0.6f, .delay_ms = 0},
                                 {.f0 = 1047, .ms = 140, .wave = Wave::Square, .volume = 0.6f, .delay_ms = 80}};
    wc::audio::play(t, 2);
}
void finish()
{
    const wc::audio::Tone t[] = {{.f0 = 523, .ms = 100, .wave = Wave::Square, .volume = 0.7f, .delay_ms = 0},
                                 {.f0 = 659, .ms = 100, .wave = Wave::Square, .volume = 0.7f, .delay_ms = 100},
                                 {.f0 = 784, .ms = 100, .wave = Wave::Square, .volume = 0.7f, .delay_ms = 200},
                                 {.f0 = 1047, .ms = 300, .wave = Wave::Square, .volume = 0.8f, .delay_ms = 300}};
    wc::audio::play(t, 4);
}
void engine(float speed01)
{
    // Climb through five "gears": the pitch rises, drops at each shift, and rises again.
    const float g = speed01 * 5.0f;
    const int gear = std::min(4, int(g));
    const float f = 70.0f + gear * 16.0f + (g - gear) * 120.0f;
    wc::audio::play({.f0 = f, .f1 = f * 1.03f, .ms = 85, .wave = Wave::Triangle, .volume = 0.16f + 0.10f * speed01});
}
}  // namespace sfx

}  // namespace

struct Racer::State {
    // ---- track and race
    std::vector<Segment> segs;
    std::vector<Prop> props;          // sorted by segment
    std::vector<int> prop_first;      // first prop index for each segment (props.size() if none)
    float track_len = 0;
    Car rivals[RIVALS];
    float pos = 0, px = 0, speed = 0;   // player: distance along the track, lateral (-1..1 on road), speed
    int lap = 1;
    float lap_t = 0, best_lap = 0, last_lap = 0, race_t = 0;
    int place = RIVALS + 1;
    Phase phase = READY;
    float phase_t = 0;
    int countdown = 3;
    float sky_off = 0;
    float engine_t = 0, kerb_t = 0;
    int wins = 0;

    // ---- steering
    float grav_x = 0, grav_y = 1;   // smoothed gravity in screen axes
    float roll = 0;                 // how far the picture is rotated (radians)
    float steer = 0;                // -1..1
    bool mirror = false;
    // Tipping the watch forward/back is the throttle. "Level" is however it was held
    // when the race started; 0 = off (always flat out), 1..3 = low/med/high sensitivity.
    float grav_z = 0, pitch = 0, pitch_neutral = 0, throttle = 1;
    int pitch_sens = 2;
    // ---- rendering
    Canvas canvas;                  // VW x VH palette picture, rotated onto the screen
    Sheet car_sheet, trees, sign, stand, bush;
    uint8_t *view = nullptr;        // canvas pixels
    Projected proj_near[DRAW_DIST], proj_far[DRAW_DIST];
    bool proj_ok[DRAW_DIST];
    int64_t fps_t0 = 0;
    int fps_frames = 0;
    float fps = 0;
    uint32_t scene_us = 0, present_us = 0;

    // ---- menu
    Gestures ges;
    console::ui::PauseMenu menu;
    bool menu_swallow = false;

    // ================================================================= track
    void addSegment(float curve) { segs.push_back({curve}); }

    void addRoad(int enter, int hold, int leave, float curve)
    {
        for (int i = 0; i < enter; i++) addSegment(easeIn(0, curve, float(i) / enter));
        for (int i = 0; i < hold; i++) addSegment(curve);
        for (int i = 0; i < leave; i++) addSegment(easeInOut(curve, 0, float(i) / leave));
    }

    // A fresh circuit every race: straights and corners of random length and sharpness.
    void buildTrack()
    {
        segs.clear();
        addRoad(10, 40, 10, 0);   // start/finish straight
        while (int(segs.size()) < 1300) {
            const float r = frand();
            if (r < 0.25f) {
                addRoad(8, 20 + int(frand() * 50), 8, 0);
            } else {
                const float sharp = (r < 0.55f ? 2.0f : r < 0.85f ? 3.5f : 5.0f) * (frand() < 0.5f ? -1 : 1);
                addRoad(20 + int(frand() * 20), 20 + int(frand() * 50), 20 + int(frand() * 20), sharp);
                if (frand() < 0.3f) addRoad(15, 15 + int(frand() * 20), 15, -sharp * 0.8f);   // chicane
            }
        }
        addRoad(10, 30, 10, 0);
        track_len = segs.size() * SEG_LEN;

        // Scenery: trees scattered along both sides, arrow boards on the outside of corners.
        props.clear();
        const int n = int(segs.size());
        props.push_back({4, 0, PROP_GANTRY});
        for (int i = 6; i < 44; i += 6) {
            props.push_back({i, -2.1f, PROP_STAND});
            props.push_back({i, 2.1f, PROP_STAND});
        }
        for (int i = 48; i < n; i += 3 + int(frand() * 5)) {
            const float side = frand() < 0.5f ? -1.0f : 1.0f;
            if (std::fabs(segs[i].curve) > 2.5f && frand() < 0.5f)
                props.push_back({i, (segs[i].curve > 0 ? -1.0f : 1.0f) * 1.45f, PROP_SIGN});
            else if (frand() < 0.3f)
                props.push_back({i, side * (1.3f + frand() * 0.8f), PROP_BUSH});
            else
                props.push_back({i, side * (1.5f + frand() * 1.6f), PROP_TREE});
        }
        prop_first.assign(n + 1, int(props.size()));
        for (int k = int(props.size()) - 1; k >= 0; k--) prop_first[props[k].seg] = k;
        for (int i = n - 1; i >= 0; i--)
            if (prop_first[i] == int(props.size()) || props[prop_first[i]].seg != i)
                prop_first[i] = std::min(prop_first[i], prop_first[i + 1]);
    }

    void newRace()
    {
        buildTrack();
        pos = 0;
        px = 0;
        speed = 0;
        lap = 1;
        lap_t = best_lap = last_lap = race_t = 0;
        static constexpr uint8_t colors[RIVALS] = {P_CAR0, P_CAR1, P_CAR2, P_CAR3, P_CAR4};
        for (int i = 0; i < RIVALS; i++) {
            // A grid ahead of you, fastest at the front.
            rivals[i] = {(i + 1) * 5.0f * SEG_LEN + PLAYER_Z, (i % 2 ? 0.45f : -0.45f),
                         MAX_SPEED * (0.74f + 0.045f * i), 1, colors[i]};
        }
        place = RIVALS + 1;
        phase = READY;
        phase_t = 0;
    }

    // ================================================================= settings
    void load()
    {
        nvs_handle_t h;
        if (nvs_open("racer", NVS_READONLY, &h) != ESP_OK) return;
        uint8_t v;
        if (nvs_get_u8(h, "mirror", &v) == ESP_OK) mirror = v;
        if (nvs_get_u8(h, "pitch", &v) == ESP_OK && v < 4) pitch_sens = v;
        if (nvs_get_u8(h, "wins", &v) == ESP_OK) wins = v;
        nvs_close(h);
    }

    void save()
    {
        nvs_handle_t h;
        if (nvs_open("racer", NVS_READWRITE, &h) != ESP_OK) return;
        nvs_set_u8(h, "mirror", mirror);
        nvs_set_u8(h, "pitch", uint8_t(pitch_sens));
        nvs_set_u8(h, "wins", uint8_t(std::min(wins, 250)));
        nvs_commit(h);
        nvs_close(h);
    }

    // ================================================================= update
    void updateSteering(const InputState &in, float dt)
    {
        // Low-pass gravity to take out hand tremor, then read the wheel angle from it.
        const float gx = in.tilt.ax * (mirror ? -1 : 1), gy = in.tilt.ay;
        const float k = 1.0f - std::exp(-dt / 0.12f);
        grav_x += (gx - grav_x) * k;
        grav_y += (gy - grav_y) * k;
        grav_z += (in.tilt.az - grav_z) * k;
        // Forward/back lean: 0 with the screen facing you, positive as the top tips away.
        pitch = std::atan2(grav_z, std::sqrt(grav_x * grav_x + grav_y * grav_y));
        if (grav_x * grav_x + grav_y * grav_y > 0.09f) {   // lying flat: no "down" to read, hold the last angle
            // Upright = 0. Turning the wheel clockwise tips gravity toward +x in screen space.
            roll = std::atan2(grav_x, grav_y);
        }
        float s = roll / FULL_LOCK;
        const float dead = 0.06f;
        s = std::fabs(s) < dead ? 0 : (s - std::copysign(dead, s)) / (1 - dead);
        s = clampf(s, -1, 1);
        // Gentle around the middle for small corrections, full authority at the ends.
        s = 0.65f * s + 0.35f * s * std::fabs(s);
        steer += (s - steer) * (1.0f - std::exp(-dt / 0.08f));
    }

    float totalDistance(float z, int lp) const { return (lp - 1) * track_len + z; }

    void updateRace(const InputState &in, float dt)
    {
        const float sp = speed / MAX_SPEED;
        const int seg_i = int(std::fmod(pos + PLAYER_Z, track_len) / SEG_LEN) % int(segs.size());

        // Steering bites harder the faster you go; corners push you wide.
        px += steer * sp * dt * STEER_RATE;
        px -= segs[seg_i].curve * sp * sp * CENTRIFUGAL * dt;

        // Tip forward to go faster, back to slow right down. With it off, flat out.
        if (pitch_sens == 0) {
            throttle = 1.0f;
        } else {
            static constexpr float kRange[4] = {0, 0.60f, 0.40f, 0.26f};   // radians from level to full/stop
            throttle = clampf(0.70f + (pitch - pitch_neutral) / kRange[pitch_sens] * 0.62f, 0.08f, 1.0f);
        }
        const float target = MAX_SPEED * throttle;
        if (speed < target) speed = std::min(target, speed + ACCEL * dt);
        else speed = std::max(target, speed + BRAKE * 0.55f * dt);
        const bool offroad = std::fabs(px) > 1.0f;
        if (offroad && speed > OFFROAD_LIMIT) speed += OFFROAD_DECEL * dt;
        px = clampf(px, -2.2f, 2.2f);
        speed = clampf(speed, 0, MAX_SPEED);

        // Kerbs buzz through the speaker.
        kerb_t -= dt;
        if (std::fabs(px) > 0.85f && speed > MAX_SPEED * 0.15f && kerb_t <= 0) {
            sfx::kerb();
            kerb_t = 0.07f;
        }

        // Rivals hold their line and drift a little.
        for (Car &c : rivals) {
            c.z += c.speed * dt;
            if (c.z >= track_len) {
                c.z -= track_len;
                c.lap++;
            }
            c.offset += std::sin(race_t * 0.7f + c.color) * 0.05f * dt;
            c.offset = clampf(c.offset, -0.75f, 0.75f);
        }

        // Running into the back of someone costs you speed.
        const float my_z = std::fmod(pos + PLAYER_Z, track_len);
        for (Car &c : rivals) {
            float gap = c.z - my_z;
            if (gap < -track_len / 2) gap += track_len;
            // Cars are 0.31 road half-widths wide, so that's how close centres get before touching.
            if (gap > 0 && gap < SEG_LEN * 0.8f && speed > c.speed && std::fabs(px - c.offset) < 0.29f) {
                speed = c.speed * 0.75f;
                sfx::bump();
            }
        }

        pos += speed * dt;
        lap_t += dt;
        race_t += dt;
        sky_off += segs[seg_i].curve * sp * dt * 28.0f;
        if (pos >= track_len) {
            pos -= track_len;
            last_lap = lap_t;
            if (best_lap == 0 || lap_t < best_lap) best_lap = lap_t;
            lap_t = 0;
            lap++;
            if (lap > LAPS) {
                lap = LAPS;
                phase = FINISHED;
                phase_t = 0;
                if (place == 1) {
                    wins++;
                    save();
                }
                sfx::finish();
                return;
            }
            sfx::lap();
        }

        // Where am I in the field?
        const float mine = totalDistance(pos + PLAYER_Z, lap);
        place = 1;
        for (const Car &c : rivals)
            if (totalDistance(c.z, c.lap) > mine) place++;

        engine_t -= dt;
        if (engine_t <= 0) {
            sfx::engine(sp);
            engine_t = 0.075f;
        }
    }

    void update(Engine &e, float dt)
    {
        const InputState &in = e.input();
        ges.update(in.touch);
        phase_t += dt;
        updateSteering(in, dt);

        if (menu.isOpen()) {
            // The touch that paused the game is still down when the menu appears; its
            // release must not count as a tap on whatever row is under the finger.
            if (menu_swallow) {
                if (in.touch.released || !in.touch.down) menu_swallow = false;
                return;
            }
            switch (menu.update(e, ges, in)) {
            case 0:
                mirror = !mirror;
                save();
                break;
            case 1:
                pitch_sens = (pitch_sens + 1) % 4;
                pitch_neutral = pitch;
                save();
                break;
            case 2: menu.toggleSound(); break;
            case 3:
                newRace();   // fresh circuit, back on the grid
                menu.close();
                break;
            }
            if (!menu.isOpen()) pitch_neutral = pitch;   // your grip may have changed while paused
            return;
        }
        // Touching the screen mid-race pauses (you're holding a steering wheel, not
        // looking for a button). Swipe left works everywhere.
        if (ges.swipe_left || (in.touch.pressed && (phase == RACING || phase == COUNTDOWN))) {
            menu.open();
            menu_swallow = in.touch.down;
            return;
        }

        switch (phase) {
        case READY:
            if (ges.tap) {
                pitch_neutral = pitch;   // however you're holding it now is "cruising"
                phase = COUNTDOWN;
                phase_t = 0;
                countdown = 3;
                sfx::beep(false);
            }
            break;
        case COUNTDOWN:
            if (phase_t >= 1.0f) {
                phase_t = 0;
                countdown--;
                sfx::beep(countdown == 0);
                if (countdown == 0) phase = RACING;
            }
            break;
        case RACING:
            updateRace(in, dt);
            break;
        case FINISHED:
            speed = std::max(0.0f, speed - MAX_SPEED * 0.4f * dt);
            pos = std::fmod(pos + speed * dt, track_len);
            if (ges.tap && phase_t > 1.0f) newRace();
            break;
        }
    }

    // ================================================================= menu
    void drawMenu(Gfx &g)
    {
        namespace ui = console::ui;
        static const char *const kSens[4] = {"OFF", "LOW", "MED", "HIGH"};
        const ui::PauseMenu::Row rows[] = {{"STEERING", mirror ? "MIRROR" : "NORMAL"},
                                           {"TILT SPEED", kSens[pitch_sens], pitch_sens ? ui::VALUE : ui::DIM},
                                           menu.soundRow(),
                                           {"RESTART RACE", "GO", ui::ACCENT}};
        menu.draw(g, rows, 4);
    }

    // ================================================================= scene (8-bit, upright)
    void hspan(int y, int x0, int x1, uint8_t c)
    {
        if (y < 0 || y >= VH) return;
        x0 = std::max(x0, 0);
        x1 = std::min(x1, VW);
        if (x1 > x0) std::memset(view + y * VW + x0, c, x1 - x0);
    }

    void rect(int x, int y, int w, int h, uint8_t c)
    {
        for (int j = 0; j < h; j++) hspan(y + j, x, x + w, c);
    }

    void text(int x, int y, const char *s, uint8_t c, int scale = 1)
    {
        for (; *s; s++) {
            unsigned ch = static_cast<unsigned char>(*s);
            if (ch < 32 || ch > 126) ch = '?';
            const uint8_t *glyph = kFont5x7[ch - 32];
            for (int col = 0; col < 5; col++)
                for (int row = 0; row < 7; row++)
                    if (glyph[col] & (1 << row)) rect(x + col * scale, y + row * scale, scale + (scale > 1), scale, c);
            x += 6 * scale;
        }
    }

    void textCentered(int cx, int y, const char *s, uint8_t c, int scale = 1)
    {
        text(cx - int(std::strlen(s)) * 6 * scale / 2, y, s, c, scale);
    }

    void drawBackground()
    {
        // Sky: eight bands, deep blue overhead fading to haze at the horizon.
        for (int y = 0; y < HORIZON; y++) std::memset(view + y * VW, P_SKY0 + std::min(7, y * 8 / HORIZON), VW);

        // A low sun and a few clouds that drift with the corners, slower than the hills.
        const int sun_x = VC + 46 - int(sky_off * 0.25f) % VW;
        for (int dy = -11; dy <= 11; dy++) {
            const int half = int(std::sqrt(float(11 * 11 - dy * dy)));
            hspan(HORIZON - 34 + dy, sun_x - half - 2, sun_x + half + 2, P_SUN_GLOW);
        }
        for (int dy = -8; dy <= 8; dy++) {
            const int half = int(std::sqrt(float(8 * 8 - dy * dy)));
            hspan(HORIZON - 34 + dy, sun_x - half, sun_x + half, P_SUN);
        }
        for (int k = 0; k < 4; k++) {
            const int cx = ((k * 83 + 20 - int(sky_off * 0.4f)) % (VW + 60) + VW + 60) % (VW + 60) - 30;
            const int cy = 38 + (k * 13) % 26;
            hspan(cy, cx - 12, cx + 12, P_CLOUD);
            hspan(cy - 1, cx - 8, cx + 9, P_CLOUD);
            hspan(cy - 2, cx - 3, cx + 5, P_CLOUD);
            hspan(cy + 1, cx - 9, cx + 8, P_CLOUD);
        }

        // Two ranges of hills: the far one paler and slower, for depth.
        for (int x = 0; x < VW; x++) {
            const float tf = x + sky_off * 0.55f, tn = x + sky_off;
            const int hf = int(19 + 8 * std::sin(tf * 0.027f) + 4 * std::sin(tf * 0.071f + 0.7f));
            const int hn = int(10 + 6 * std::sin(tn * 0.043f + 2.1f) + 3 * std::sin(tn * 0.117f + 1.3f));
            for (int y = HORIZON - hf; y < HORIZON - hn; y++) view[y * VW + x] = P_MOUNTAIN_FAR;
            for (int y = HORIZON - hn; y < HORIZON; y++) view[y * VW + x] = P_MOUNTAIN;
        }
        for (int y = HORIZON; y < VH; y++) std::memset(view + y * VW, P_GRASS_B, VW);
    }

    Projected project(float world_x, float rel_z, float cam_x) const
    {
        Projected p;
        p.scale = CAM_DEPTH / rel_z;
        p.x = VC + p.scale * (world_x - cam_x) * VC;
        p.y = HORIZON + p.scale * CAM_H * VC;
        p.w = p.scale * ROAD_W * VC;
        return p;
    }

    void drawSegment(const Projected &n, const Projected &f, int index, int max_y)
    {
        const int y_far = std::max(int(f.y), HORIZON), y_near = std::min(int(n.y), max_y);
        if (y_near <= y_far) return;
        const bool alt = (index / RUMBLE_LEN) % 2;
        const uint8_t grass = alt ? P_GRASS_A : P_GRASS_B, road = alt ? P_ROAD_A : P_ROAD_B;
        const uint8_t kerb = alt ? P_KERB_RED : P_KERB_WHITE;
        const float span = std::max(1.0f, n.y - f.y);
        for (int y = y_far; y < y_near; y++) {
            const float t = (y - f.y) / span;
            const float x = f.x + (n.x - f.x) * t, w = f.w + (n.w - f.w) * t;
            const float kerb_w = w * 0.12f;
            std::memset(view + y * VW, grass, VW);
            hspan(y, int(x - w - kerb_w), int(x - w), kerb);
            hspan(y, int(x + w), int(x + w + kerb_w), kerb);
            hspan(y, int(x - w), int(x + w), index < 3 ? P_WHITE : road);   // start/finish line
            if (alt && index >= 3) {
                const float lane_w = std::max(1.0f, w * 0.03f);
                hspan(y, int(x - w / 3 - lane_w), int(x - w / 3 + lane_w), P_LANE);
                hspan(y, int(x + w / 3 - lane_w), int(x + w / 3 + lane_w), P_LANE);
            }
        }
    }

    // The car sprite, scaled to `w` pixels wide with its wheels sitting on `bottom`.
    void drawCar(float cx, float bottom, float w, uint8_t body, float lean, bool flame)
    {
        if (w < 5) {   // a dot in the distance
            rect(int(cx) - 1, int(bottom) - 2, 3, 2, body);
            return;
        }
        if (!car_sheet.valid()) return;
        const int dw = int(w), dh = std::max(3, int(w * CAR_H / CAR_W));
        const int x0 = int(cx - w / 2), y0 = int(bottom) - dh;
        const uint8_t shade = uint8_t(body + CAR_SHADE);
        const uint8_t fire = (esp_random() & 1) ? P_FLAME_A : P_FLAME_B;
        for (int dy = 0; dy < dh; dy++) {
            const int y = y0 + dy;
            if (y < 0 || y >= VH) continue;
            const uint8_t *row = car_sheet.px + (dy * CAR_H / dh) * car_sheet.w;
            // The body leans into the corner a little; the tyres stay planted.
            const int shift = int(lean * w * 0.05f * (1.0f - float(dy) / dh));
            uint8_t *out = view + y * VW;
            for (int dx = 0; dx < dw; dx++) {
                const int x = x0 + dx;
                if (x < 0 || x >= VW) continue;
                uint8_t c = row[dx * CAR_W / dw];
                if (!c) continue;
                if (c == P_CAR0) c = body;
                else if (c == P_CAR0_D) c = shade;
                else if (c == P_FLAME_A) {
                    if (!flame) continue;
                    c = fire;
                }
                const bool planted = c == P_TIRE || c == P_TIRE_LIT;
                const int xs = planted ? x : x + shift;
                if (xs >= 0 && xs < VW) out[xs] = c;
            }
        }
    }

    // A sprite frame scaled to `h` pixels tall (width follows), feet on `bottom`.
    void blitScaled(const Sheet &sh, int frame, float cx, float bottom, float h, bool flip = false)
    {
        if (!sh.valid() || h < 2) return;
        const int dh = int(h), dw = std::max(1, int(h * sh.fw / sh.fh));
        const int x0 = int(cx - dw / 2.0f), y0 = int(bottom) - dh;
        if (y0 >= VH || y0 + dh <= 0 || x0 >= VW || x0 + dw <= 0) return;
        const int fx = (frame % sh.cols()) * sh.fw, fy = (frame / sh.cols()) * sh.fh;
        for (int dy = 0; dy < dh; dy++) {
            const int y = y0 + dy;
            if (y < 0 || y >= VH) continue;
            const uint8_t *row = sh.px + (fy + dy * sh.fh / dh) * sh.w + fx;
            uint8_t *out = view + y * VW;
            for (int dx = 0; dx < dw; dx++) {
                const int x = x0 + dx;
                if (x < 0 || x >= VW) continue;
                int sx = dx * sh.fw / dw;
                if (flip) sx = sh.fw - 1 - sx;
                const uint8_t c = row[sx];
                if (c) out[x] = c;
            }
        }
    }

    void drawTree(float cx, float bottom, float scale, int kind)
    {
        blitScaled(trees, kind & 1, cx, bottom, scale * 2600.0f * VC);
    }

    void drawBush(float cx, float bottom, float scale)
    {
        blitScaled(bush, 0, cx, bottom, scale * 700.0f * VC);
    }

    // The start/finish gantry: two towers and a beam across the road with the lights on it.
    void drawGantry(float cx, float bottom, float scale)
    {
        const float road = scale * ROAD_W * VC;
        const int h = int(scale * 2600.0f * VC);
        if (h < 6) return;
        const int post = std::max(1, int(road * 0.07f)), beam = std::max(2, h / 5);
        const int xl = int(cx - road * 1.22f), xr = int(cx + road * 1.22f), top = int(bottom) - h;
        rect(xl, top, post, h, P_GREY);
        rect(xr - post, top, post, h, P_GREY);
        rect(xl, top, xr - xl, beam, P_WING);
        // Checkered strip along the beam, and the start lights hanging under it.
        const int sq = std::max(1, beam / 3);
        for (int x = xl + post; x < xr - post; x += sq)
            rect(x, top + 1, sq, sq, ((x - xl) / sq) % 2 ? P_WHITE : P_BLACK);
        const int lamp = std::max(1, beam / 3);
        for (int k = -2; k <= 2; k++)
            rect(int(cx) + k * lamp * 2 - lamp / 2, top + beam, lamp, lamp, phase == COUNTDOWN ? P_RED : P_GREEN);
    }

    void drawStand(float cx, float bottom, float scale)
    {
        blitScaled(stand, 0, cx, bottom, scale * 1500.0f * VC);
    }

    void drawSign(float cx, float bottom, float scale, bool points_right)
    {
        blitScaled(sign, 0, cx, bottom, scale * 1500.0f * VC, !points_right);
    }

    void drawRoad()
    {
        const int n_segs = int(segs.size());
        const int base = int(pos / SEG_LEN) % n_segs;
        const float base_pct = std::fmod(pos, SEG_LEN) / SEG_LEN;
        const float cam_x = px * ROAD_W;
        float x = 0, dx = -segs[base].curve * base_pct;
        int max_y = VH;

        for (int n = 0; n < DRAW_DIST; n++) {
            const int i = (base + n) % n_segs;
            const float z_near = n * SEG_LEN - base_pct * SEG_LEN, z_far = z_near + SEG_LEN;
            proj_ok[n] = z_near > CAM_DEPTH * 40;
            if (proj_ok[n]) {
                proj_near[n] = project(-x, z_near, cam_x);
                proj_far[n] = project(-x - dx, z_far, cam_x);
            }
            x += dx;
            dx += segs[i].curve;
            if (!proj_ok[n] || proj_far[n].y >= max_y) continue;
            drawSegment(proj_near[n], proj_far[n], i, max_y);
            max_y = std::min(max_y, int(proj_far[n].y));
        }

        // Scenery and rivals, far to near so closer things cover farther ones.
        for (int n = DRAW_DIST - 1; n >= 1; n--) {
            if (!proj_ok[n]) continue;
            const int seg_i = (base + n) % n_segs;
            for (int k = prop_first[seg_i]; k < int(props.size()) && props[k].seg == seg_i; k++) {
                const Projected &a = proj_near[n];
                const float sx = a.x + a.scale * props[k].offset * ROAD_W * VC;
                switch (props[k].kind) {
                case PROP_TREE: drawTree(sx, a.y, a.scale, seg_i + k); break;
                case PROP_BUSH: drawBush(sx, a.y, a.scale); break;
                case PROP_SIGN: drawSign(sx, a.y, a.scale, segs[seg_i].curve > 0); break;
                case PROP_GANTRY: drawGantry(a.x, a.y, a.scale); break;
                case PROP_STAND: drawStand(sx, a.y, a.scale); break;
                }
            }
            const float seg_z0 = std::fmod((int(pos / SEG_LEN) + n) * SEG_LEN, track_len);
            for (const Car &c : rivals) {
                float rel = c.z - seg_z0;
                if (rel < 0 && rel > -SEG_LEN * 0.001f) rel = 0;
                if (rel < 0 || rel >= SEG_LEN) continue;
                const float t = rel / SEG_LEN;
                const Projected &a = proj_near[n], &b = proj_far[n];
                const float scale = a.scale + (b.scale - a.scale) * t;
                const float sx = a.x + (b.x - a.x) * t + scale * c.offset * ROAD_W * VC;
                const float sy = a.y + (b.y - a.y) * t;
                drawCar(sx, sy, scale * 620.0f * VC, c.color, 0, true);
            }
        }
    }

    void drawHud()
    {
        char buf[24];
        snprintf(buf, sizeof(buf), "%d", int(speed / MAX_SPEED * 320));
        textCentered(VC, 20, buf, P_WHITE, 2);
        textCentered(VC, 37, "KM/H", P_WHITE, 1);
        snprintf(buf, sizeof(buf), "P%d", place);
        text(48, 56, buf, P_YELLOW, 2);
        snprintf(buf, sizeof(buf), "L%d/%d", lap, LAPS);
        text(VW - 48 - int(std::strlen(buf)) * 12, 56, buf, P_WHITE, 2);
        snprintf(buf, sizeof(buf), "%.1f", lap_t);
        textCentered(VC, 50, buf, P_WHITE, 1);

        // Steering indicator: a little wheel mark that shows how far you've turned.
        const int bar = int(steer * 28);
        if (pitch_sens) {   // throttle gauge beside the speed
            const int h = int(throttle * 22);
            rect(VC + 34, 18, 4, 24, P_PANEL);
            rect(VC + 34, 18 + 24 - h - 1, 4, h + 1, throttle > 0.95f ? P_RED : P_GREEN);
        }
        rect(VC - 30, 234, 60, 3, P_PANEL);
        rect(VC + std::min(0, bar), 234, std::abs(bar) + 1, 3, std::fabs(steer) > 0.98f ? P_RED : P_GREEN);
    }

    void panel(int y, int h) { rect(38, y, VW - 76, h, P_PANEL); }

    void drawOverlay()
    {
        char buf[32];
        switch (phase) {
        case READY:
            panel(112, 62);
            textCentered(VC, 118, "TAP TO RACE", P_WHITE, 2);
            textCentered(VC, 140, "TURN THE WATCH TO STEER", P_YELLOW, 1);
            textCentered(VC, 152, pitch_sens ? "TIP FORWARD = FASTER" : "SPEED IS AUTOMATIC", P_YELLOW, 1);
            textCentered(VC, 164, "TOUCH TO PAUSE", P_WHITE, 1);
            break;
        case COUNTDOWN:
            snprintf(buf, sizeof(buf), "%d", countdown);
            panel(108, 50);
            textCentered(VC, 116, buf, P_RED, 5);
            break;
        case RACING:
            if (race_t < 1.0f) {
                panel(108, 50);
                textCentered(VC, 116, "GO!", P_GREEN, 5);
            }
            break;
        case FINISHED:
            panel(100, 84);
            snprintf(buf, sizeof(buf), "FINISHED P%d", place);
            textCentered(VC, 106, buf, place == 1 ? P_GREEN : P_WHITE, 2);
            snprintf(buf, sizeof(buf), "BEST LAP %.1f", best_lap);
            textCentered(VC, 130, buf, P_YELLOW, 1);
            snprintf(buf, sizeof(buf), "RACES WON %d", wins);
            textCentered(VC, 144, buf, P_WHITE, 1);
            textCentered(VC, 164, "TAP TO RACE AGAIN", P_WHITE, 1);
            break;
        }
    }

    void draw(Engine &e, Gfx &g)
    {
        if (menu.isOpen()) {
            drawMenu(g);
            return;
        }
        const int64_t t_draw0 = esp_timer_get_time();
        drawBackground();
        drawRoad();
        // Your car: leans into the turn, and shudders a touch at speed.
        const float wobble = (speed > MAX_SPEED * 0.6f) ? ((esp_random() & 1) ? 0.5f : -0.5f) : 0;
        drawCar(VC + steer * 5, 224 + wobble, 70, P_CAR_PLAYER, steer, throttle > 0.55f && speed > MAX_SPEED * 0.1f);
        drawHud();
        drawOverlay();
        const int64_t t_scene = esp_timer_get_time();
        // Rotate the upright scene by the roll of the watch onto the round screen.
        canvas.presentRotated(e.presenter(), roll);

        fps_frames++;
        const int64_t now = esp_timer_get_time();
        scene_us += uint32_t(t_scene - t_draw0);
        present_us += uint32_t(now - t_scene);
        if (now - fps_t0 > 5000000) {
            fps = fps_frames * 1e6f / float(now - fps_t0);
            const PresentStats &ps = e.presenter().stats();
            ESP_LOGI(TAG, "%.1f fps (scene %.1f, present %.1f ms | wire %.1f, vsync wait %.1f, slot wait %.1f ms) thr %.2f",
                     fps, scene_us / 1000.0f / fps_frames, present_us / 1000.0f / fps_frames,
                     ps.last_xfer_us / 1000.0f, ps.last_vsync_wait_us / 1000.0f, ps.last_wait_us / 1000.0f, throttle);
            scene_us = present_us = 0;
            fps_frames = 0;
            fps_t0 = now;
        }
    }
};

Racer::Racer() : s_(new State) {}
Racer::~Racer() { delete s_; }

void Racer::begin(Engine &e)
{
    if (s_->canvas.init(2, VW)) {
        s_->view = s_->canvas.pixels();
        s_->canvas.setPalette(kPalette, P_COUNT);
        auto load = [&](Sheet &sh, const uint8_t *a, const uint8_t *b, int fw, int fh) {
            if (!s_->canvas.loadSheet(sh, a, b - a, fw, fh)) ESP_LOGE(TAG, "sheet failed");
        };
        load(s_->car_sheet, _binary_car_png_start, _binary_car_png_end, CAR_W, CAR_H);
        load(s_->trees, _binary_trees_png_start, _binary_trees_png_end, 24, 32);
        load(s_->sign, _binary_sign_png_start, _binary_sign_png_end, 24, 18);
        load(s_->stand, _binary_stand_png_start, _binary_stand_png_end, 48, 30);
        load(s_->bush, _binary_bush_png_start, _binary_bush_png_end, 16, 10);
    } else {
        ESP_LOGE(TAG, "no memory for the view");
    }
    s_->load();
    s_->newRace();
}

void Racer::enter(Engine &e)
{
    s_->fps_t0 = esp_timer_get_time();
    s_->fps_frames = 0;
    if (s_->phase == RACING) {   // came back from the home screen mid-race
        s_->menu.open();
    }
}

bool Racer::keepAwake() const { return !s_->menu.isOpen() && (s_->phase == RACING || s_->phase == COUNTDOWN); }

void Racer::update(Engine &e, float dt)
{
    if (s_->view) s_->update(e, std::min(dt, 1.0f / 20));
}

void Racer::draw(Engine &e, Gfx &g)
{
    if (s_->view) s_->draw(e, g);
}

}  // namespace games
