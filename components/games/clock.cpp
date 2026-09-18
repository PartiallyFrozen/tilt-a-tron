#include "games/clock.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <sys/time.h>

#include "audio/audio.h"
#include "console/pause_menu.h"
#include "console/ui.h"
#include "engine/canvas.h"
#include "engine/store.h"
#include "engine/gestures.h"
#include "engine/polar.h"
#include "esp_log.h"
#include "net/net.h"

using namespace wc;

// Sprites borrowed from the games for the fun faces.
extern "C" {
extern const uint8_t _binary_hopper_png_start[], _binary_hopper_png_end[];
extern const uint8_t _binary_clouds_png_start[], _binary_clouds_png_end[];
extern const uint8_t _binary_ledges_png_start[], _binary_ledges_png_end[];
extern const uint8_t _binary_car_png_start[], _binary_car_png_end[];
}

namespace games {

namespace {

const char *TAG = "clock";

constexpr int SCALE = 2, CW = (Gfx::W + SCALE - 1) / SCALE;
constexpr float C = CW / 2.0f;
constexpr float PI = 3.14159265f, TAU = 2 * PI;

enum Face { POCKET, LCD, SKY, TACHO, RINGS, FACE_COUNT };
const char *const FACE_NAMES[FACE_COUNT] = {"POCKET", "RETRO LCD", "HOPPER SKY", "TACHO", "RINGS"};
const char *const DAYS[7] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
const char *const MONTHS[12] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};

float clampf(float v, float a, float b) { return std::max(a, std::min(b, v)); }
Color mixc(int r0, int g0, int b0, int r1, int g1, int b1, float t)
{
    return rgb(uint8_t(r0 + (r1 - r0) * t), uint8_t(g0 + (g1 - g0) * t), uint8_t(b0 + (b1 - b0) * t));
}

// Seven-segment digit: segments a..g as bit 0..6.
constexpr uint8_t SEG[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};

}  // namespace

struct Clock::State {
    // ---- settings
    int face = POCKET;
    int tz_min = 0;            // minutes east of UTC
    bool tz_auto = true;       // take the zone from the network when it's known
    bool h24 = false;

    // ---- ui
    Gestures ges;
    console::ui::PauseMenu menu;
    bool setting = false, set_dirty = false;
    int set_h = 12, set_m = 0;
    float t = 0;               // animation time
    int last_face_tap = -1;

    // ---- drawing
    Canvas canvas;
    Sheet hopper, clouds, ledges, car;
    uint8_t sky0 = 0;          // 24 sky gradient entries
    uint8_t c_black = 0, c_white = 0, c_dim = 0, c_red = 0, c_gold = 0, c_gold_dark = 0, c_cream = 0, c_ink = 0;
    uint8_t c_ink_light = 0, c_lcd = 0, c_lcd_off = 0, c_lcd_on = 0, c_bezel = 0, c_grey = 0, c_grey_dark = 0;
    uint8_t c_cyan = 0, c_yellow = 0, c_purple = 0, c_panel = 0, c_box = 0, c_sun = 0, c_moon = 0, c_star = 0;
    uint8_t c_needle = 0, c_green = 0, c_orange = 0, c_shadow = 0;

    // ------------------------------------------------------------------ time
    struct Now {
        int h, m, s, ms, wday, mday, mon, year;
        bool known;
    };
    Now now() const
    {
        timeval tv;
        gettimeofday(&tv, nullptr);
        const time_t local = tv.tv_sec + tz_min * 60;
        tm tmv;
        gmtime_r(&local, &tmv);
        return {tmv.tm_hour, tmv.tm_min, tmv.tm_sec, int(tv.tv_usec / 1000), tmv.tm_wday, tmv.tm_mday,
                tmv.tm_mon, tmv.tm_year + 1900, tv.tv_sec > 1700000000};
    }

    // ------------------------------------------------------------------ persistence
    void load()
    {
        wc::Store s("clock");
        s.get("face", face, FACE_COUNT);
        s.get("h24", h24);
        s.get("tz", tz_min);
        s.get("tz_auto", tz_auto);
    }
    void save()
    {
        wc::Store s("clock", wc::Store::Write);
        s.set("face", face);
        s.set("h24", h24);
        s.set("tz", tz_min);
        s.set("tz_auto", tz_auto);
    }

    bool loadAssets()
    {
        if (!canvas.init(SCALE)) return false;
        c_black = canvas.color(rgb(0, 0, 0));
        c_white = canvas.color(rgb(255, 255, 255));
        c_dim = canvas.color(rgb(150, 150, 160));
        c_red = canvas.color(rgb(220, 40, 40));
        c_gold = canvas.color(rgb(214, 170, 60));
        c_gold_dark = canvas.color(rgb(140, 100, 30));
        c_cream = canvas.color(rgb(244, 234, 205));
        c_ink = canvas.color(rgb(40, 30, 24));
        c_ink_light = canvas.color(rgb(120, 100, 80));
        c_lcd = canvas.color(rgb(150, 172, 140));
        c_lcd_off = canvas.color(rgb(138, 158, 128));
        c_lcd_on = canvas.color(rgb(28, 40, 30));
        c_bezel = canvas.color(rgb(44, 46, 54));
        c_grey = canvas.color(rgb(112, 116, 126));
        c_grey_dark = canvas.color(rgb(60, 62, 72));
        c_cyan = canvas.color(rgb(80, 220, 240));
        c_yellow = canvas.color(rgb(255, 215, 60));
        c_purple = canvas.color(rgb(190, 90, 240));
        c_panel = canvas.color(rgb(16, 18, 26));
        c_box = canvas.color(rgb(90, 90, 100));
        c_sun = canvas.color(rgb(255, 230, 120));
        c_moon = canvas.color(rgb(230, 230, 210));
        c_star = canvas.color(rgb(200, 205, 230));
        c_needle = canvas.color(rgb(255, 70, 70));
        c_green = canvas.color(rgb(40, 200, 110));
        c_orange = canvas.color(rgb(255, 150, 40));
        c_shadow = canvas.color(rgb(20, 20, 26));
        sky0 = canvas.reserve(24);
        bool ok = true;
        ok &= canvas.loadSheet(hopper, _binary_hopper_png_start, _binary_hopper_png_end - _binary_hopper_png_start, 20, 22);
        ok &= canvas.loadSheet(clouds, _binary_clouds_png_start, _binary_clouds_png_end - _binary_clouds_png_start, 20, 8);
        ok &= canvas.loadSheet(ledges, _binary_ledges_png_start, _binary_ledges_png_end - _binary_ledges_png_start, 30, 7);
        ok &= canvas.loadSheet(car, _binary_car_png_start, _binary_car_png_end - _binary_car_png_start, 32, 16);
        return ok;
    }

    // ------------------------------------------------------------------ helpers
    void thickLine(Canvas &c, float x0, float y0, float x1, float y1, int w, uint8_t col)
    {
        const float dx = x1 - x0, dy = y1 - y0, len = std::max(0.001f, std::hypot(dx, dy));
        const float nx = -dy / len, ny = dx / len;
        for (int k = 0; k < w; k++) {
            const float o = k - (w - 1) / 2.0f;
            c.line(int(x0 + nx * o), int(y0 + ny * o), int(x1 + nx * o), int(y1 + ny * o), col);
        }
    }

    // A watch hand from the centre at angle a (0 = 12 o'clock, clockwise).
    void hand(Canvas &c, float a, float len, int w, uint8_t col, float tail = 0)
    {
        const float sx = std::sin(a), sy = -std::cos(a);
        thickLine(c, C - sx * tail, C - sy * tail, C + sx * len, C + sy * len, w, col);
    }

    void ring(Canvas &c, float r0, float r1, uint8_t col)
    {
        for (int y = 0; y < CW; y++)
            for (int x = 0; x < CW; x++) {
                const float d = std::hypot(x + 0.5f - C, y + 0.5f - C);
                if (d >= r0 && d < r1) c.pixel(x, y, col);
            }
    }

    void disc(Canvas &c, float cx, float cy, float r, uint8_t col)
    {
        const int y0 = int(std::floor(cy - r)), y1 = int(std::ceil(cy + r));
        for (int y = y0; y <= y1; y++) {
            const float dy = y + 0.5f - cy;
            if (dy * dy > r * r) continue;
            const float half = std::sqrt(r * r - dy * dy);
            c.fillRect(int(std::ceil(cx - half - 0.5f)), y, int(2 * half), 1, col);
        }
    }

    // Seven-segment digit, top-left (x, y), size w x h, stroke s.
    void segDigit(Canvas &c, int d, int x, int y, int w, int h, int s, uint8_t on, uint8_t off)
    {
        const uint8_t m = d < 0 ? 0 : SEG[d];
        auto seg = [&](int bit, int sx, int sy, int sw, int sh) { c.fillRect(sx, sy, sw, sh, (m >> bit) & 1 ? on : off); };
        const int mid = y + h / 2 - s / 2;
        seg(0, x + s, y, w - 2 * s, s);                       // a top
        seg(1, x + w - s, y + s, s, h / 2 - s);               // b top right
        seg(2, x + w - s, y + h / 2, s, h / 2 - s);           // c bottom right
        seg(3, x + s, y + h - s, w - 2 * s, s);               // d bottom
        seg(4, x, y + h / 2, s, h / 2 - s);                   // e bottom left
        seg(5, x, y + s, s, h / 2 - s);                       // f top left
        seg(6, x + s, mid, w - 2 * s, s);                     // g middle
    }

    void hourText(char *buf, size_t n, const Now &nw, bool with_secs)
    {
        int h = nw.h;
        if (!h24) {
            h = h % 12;
            if (h == 0) h = 12;
        }
        if (with_secs) snprintf(buf, n, "%d:%02d:%02d", h, nw.m, nw.s);
        else snprintf(buf, n, "%d:%02d", h, nw.m);
    }

    // ------------------------------------------------------------------ faces
    void drawPocket(Canvas &c, const Now &nw)
    {
        c.clear(c_black);
        ring(c, 0, 116.5f, c_gold);
        ring(c, 0, 110, c_gold_dark);
        ring(c, 0, 106, c_cream);
        // Crown at 12.
        c.fillRect(int(C) - 4, 0, 8, 6, c_gold_dark);
        c.fillRect(int(C) - 3, 0, 6, 4, c_gold);
        // Ticks: a fine one every minute, a bold one every hour.
        for (int i = 0; i < 60; i++) {
            const float a = i * TAU / 60, sx = std::sin(a), sy = -std::cos(a);
            const bool hr = i % 5 == 0;
            thickLine(c, C + sx * (hr ? 94 : 100), C + sy * (hr ? 94 : 100), C + sx * 104, C + sy * 104, hr ? 2 : 1,
                      hr ? c_ink : c_ink_light);
        }
        // Numerals.
        static const char *const NUM[12] = {"12", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11"};
        for (int i = 0; i < 12; i++) {
            const float a = i * TAU / 12;
            const bool big = i % 3 == 0;
            c.textCentered(int(C + std::sin(a) * 80), int(C - std::cos(a) * 80), NUM[i], c_ink, big ? 2 : 1, big);
        }
        // Date window at 3 o'clock.
        char buf[8];
        snprintf(buf, sizeof(buf), "%d", nw.mday);
        c.fillRect(int(C) + 40, int(C) - 8, 22, 16, c_white);
        c.rect(int(C) + 40, int(C) - 8, 22, 16, c_ink_light);
        c.textCentered(int(C) + 51, int(C), buf, c_ink, 1, true);
        // Hands, with a soft shadow.
        const float sec = nw.s + nw.ms / 1000.0f;
        const float ah = (nw.h % 12 + nw.m / 60.0f) * TAU / 12, am = (nw.m + nw.s / 60.0f) * TAU / 60, as = sec * TAU / 60;
        c.pixel(0, 0, c_black);
        hand(c, ah, 52, 5, c_ink_light, 8);
        hand(c, am, 78, 3, c_ink_light, 8);
        hand(c, ah, 50, 4, c_ink, 6);
        hand(c, am, 76, 2, c_ink, 6);
        hand(c, as, 92, 1, c_red, 16);
        disc(c, C, C, 4, c_ink);
        disc(c, C, C, 2, c_red);
    }

    void drawLcd(Canvas &c, const Now &nw)
    {
        c.clear(c_black);
        ring(c, 0, 116.5f, c_bezel);
        ring(c, 0, 108, c_grey_dark);
        ring(c, 0, 104, c_lcd);
        char buf[16];
        // Day and date along the top.
        snprintf(buf, sizeof(buf), "%s %d %s", DAYS[nw.wday], nw.mday, MONTHS[nw.mon]);
        c.textCentered(int(C), 50, nw.known ? buf : "SET TIME", c_lcd_on, 1, true);
        // Big hours and minutes.
        int h = nw.h;
        bool pm = false;
        if (!h24) {
            pm = h >= 12;
            h = h % 12;
            if (h == 0) h = 12;
        }
        const int dw = 26, dh = 46, s = 5, gap = 6, colon = 10;
        const int total = 4 * dw + 3 * gap + colon;
        int x = int(C) - total / 2;
        const int y = int(C) - dh / 2 - 2;
        segDigit(c, h >= 10 ? h / 10 : (h24 ? 0 : -1), x, y, dw, dh, s, c_lcd_on, c_lcd_off);
        x += dw + gap;
        segDigit(c, h % 10, x, y, dw, dh, s, c_lcd_on, c_lcd_off);
        x += dw + gap;
        const bool blink = nw.ms < 500;
        c.fillRect(x + 3, y + 12, 4, 4, blink ? c_lcd_on : c_lcd_off);
        c.fillRect(x + 3, y + dh - 16, 4, 4, blink ? c_lcd_on : c_lcd_off);
        x += colon;
        segDigit(c, nw.m / 10, x, y, dw, dh, s, c_lcd_on, c_lcd_off);
        x += dw + gap;
        segDigit(c, nw.m % 10, x, y, dw, dh, s, c_lcd_on, c_lcd_off);
        // Seconds, small, and AM/PM.
        const int sx = int(C) - 14, sy = int(C) + 32;
        segDigit(c, nw.s / 10, sx, sy, 12, 20, 3, c_lcd_on, c_lcd_off);
        segDigit(c, nw.s % 10, sx + 16, sy, 12, 20, 3, c_lcd_on, c_lcd_off);
        if (!h24) c.text(int(C) + 40, sy + 6, pm ? "PM" : "AM", c_lcd_on, 1, true);
        c.textCentered(int(C), 172, "TILT-A-TRON", c_lcd_off, 1, true);
    }

    void drawSky(Canvas &c, const Now &nw)
    {
        // Sky by time of day: night, dawn/dusk, day.
        const float hour = nw.h + nw.m / 60.0f;
        const float day = clampf(1.0f - std::fabs(hour - 13.0f) / 7.5f, 0.0f, 1.0f);   // 1 mid-afternoon
        const float glow = clampf(1.0f - std::fabs(std::fmod(hour + 24 - 6.5f, 12.0f) - 6.0f) / 1.2f, 0, 1) *
                           clampf(1 - day, 0, 1);   // orange around 6:30 and 18:30
        for (int i = 0; i < 24; i++) {
            const float k = i / 23.0f;
            Color col = mixc(6 + 66 * day + int(40 * glow * k), 6 + 134 * day + int(20 * glow * k),
                             30 + 205 * day - int(40 * glow * k), 40 + 110 * day + int(120 * glow * k),
                             24 + 191 * day + int(60 * glow * k), 80 + 175 * day - int(60 * glow * k), k);
            c.setColor(uint8_t(sky0 + i), col);
        }
        uint8_t *px = c.pixels();
        for (int y = 0; y < CW; y++, px += CW) std::memset(px, sky0 + std::min(23, y * 24 / CW), CW);

        // Stars at night, sun by day, moon at night: they cross the top of the sky.
        if (day < 0.5f) {
            for (int i = 0; i < 40; i++) {
                const int x = (i * 97 + 13) % CW, y = (i * 61 + 7) % 120;
                const bool tw = ((i + int(t * 2)) % 9) == 0;
                c.fillRect(x, y, tw ? 2 : 1, tw ? 2 : 1, c_star);
            }
        }
        const float sun_a = (hour - 6.0f) / 12.0f;   // 0 at sunrise, 1 at sunset
        if (sun_a > -0.05f && sun_a < 1.05f) {
            const float x = 20 + sun_a * (CW - 40), y = 110 - std::sin(sun_a * PI) * 90;
            disc(c, x, y, 12, c_sun);
        } else {
            const float moon_a = std::fmod(hour + 6.0f, 12.0f) / 12.0f;
            const float x = 20 + moon_a * (CW - 40), y = 110 - std::sin(moon_a * PI) * 90;
            disc(c, x, y, 10, c_moon);
            disc(c, x + 5, y - 3, 8, uint8_t(sky0 + 4));
        }
        // Clouds drift.
        for (int i = 0; i < 3; i++) {
            const int x = int(std::fmod(t * (4 + i * 2) + i * 90, CW + 40.0f)) - 20;
            c.sprite(clouds, i & 1, x, 30 + i * 28);
        }
        // Ground: ledges across the bottom, Hopper hopping once a second.
        for (int x = -10; x < CW; x += 30) c.sprite(ledges, 0, x, CW - 34);
        c.fillRect(0, CW - 27, CW, 40, c_ink_light);
        const float f = nw.ms / 1000.0f;
        const float hop = f < 0.5f ? std::sin(f / 0.5f * PI) * 14 : 0;
        const float sq = f > 0.5f && f < 0.6f ? 1 - (0.6f - f) * 2 : 1;   // a squash on landing
        c.spriteScaled(hopper, (nw.s % 7 == 3 && f > 0.7f) ? 1 : 0, C, CW - 34 - hop, 1.5f * (2 - sq), 1.5f * sq);
        // The time, big.
        char buf[16];
        hourText(buf, sizeof(buf), nw, false);
        c.textCentered(int(C) + 1, 71, buf, c_black, 3, true);
        c.textCentered(int(C), 70, buf, c_white, 3, true);
        snprintf(buf, sizeof(buf), "%s %d %s", DAYS[nw.wday], nw.mday, MONTHS[nw.mon]);
        c.textCentered(int(C), 96, nw.known ? buf : "SET TIME IN THE MENU", c_white, 1, true);
    }

    void drawTacho(Canvas &c, const Now &nw)
    {
        c.clear(c_black);
        ring(c, 0, 116.5f, c_bezel);
        ring(c, 0, 110, c_black);
        // Minutes on a 270-degree sweep, 0 at 7:30 o'clock, 60 at 4:30; red zone from 50.
        auto angle = [](float m) { return -PI * 0.75f + m / 60.0f * PI * 1.5f; };
        for (int m = 0; m <= 60; m++) {
            const float a = angle(float(m)), sx = std::sin(a), sy = -std::cos(a);
            const bool big = m % 10 == 0;
            const uint8_t col = m >= 50 ? c_red : c_white;
            thickLine(c, C + sx * (big ? 90 : 98), C + sy * (big ? 90 : 98), C + sx * 104, C + sy * 104, big ? 2 : 1, col);
            if (big && m < 60) {
                char buf[4];
                snprintf(buf, sizeof(buf), "%d", m);
                c.textCentered(int(C + sx * 78), int(C - std::cos(a) * 78), buf, col, 1, true);
            }
        }
        // Hours: a small inner dial at the top.
        const float hy = C - 42;
        disc(c, C, hy, 22, c_grey_dark);
        disc(c, C, hy, 20, c_black);
        for (int h = 0; h < 12; h++) {
            const float a = h * TAU / 12;
            c.pixel(int(C + std::sin(a) * 17), int(hy - std::cos(a) * 17), c_white);
        }
        {
            const float ah = (nw.h % 12 + nw.m / 60.0f) * TAU / 12;
            thickLine(c, C, hy, C + std::sin(ah) * 15, hy - std::cos(ah) * 15, 2, c_orange);
            disc(c, C, hy, 2, c_white);
        }
        // Minute needle with a seconds shadow needle.
        const float am = angle(nw.m + nw.s / 60.0f), as = angle(nw.s + nw.ms / 1000.0f);
        hand(c, as, 96, 1, c_grey_dark, 0);
        hand(c, am, 96, 3, c_needle, 10);
        disc(c, C, C, 5, c_grey);
        disc(c, C, C, 3, c_black);
        // Digital readout and the car.
        char buf[16];
        hourText(buf, sizeof(buf), nw, true);
        c.fillRect(int(C) - 40, int(C) + 30, 80, 20, c_panel);
        c.rect(int(C) - 40, int(C) + 30, 80, 20, c_box);
        c.textCentered(int(C), int(C) + 40, buf, c_cyan, 1, true);
        c.textCentered(int(C), int(C) + 60, "MIN", c_dim, 1, false);
        c.sprite(car, 0, int(C) - 16, CW - 40);
    }

    void drawRings(Canvas &c, const Now &nw)
    {
        // Three rings of wedges fill up like Tilt-a-tris: seconds outside, then
        // minutes, then hours; the core shows the digits.
        uint8_t *px = c.pixels();
        const float sec = nw.s + nw.ms / 1000.0f;
        for (int y = 0; y < CW; y++) {
            for (int x = 0; x < CW; x++) {
                const int i = (2 * y) * Gfx::W + 2 * x;
                const float r = Polar::radius16(i) / 16.0f / SCALE;
                const uint16_t a16 = uint16_t(Polar::angle(i) + 16384);   // 0 at 12 o'clock
                uint8_t col = c_black;
                if (r < 30) col = r > 28 ? c_box : c_panel;
                else if (r < 108) {
                    int n, filled;
                    uint8_t on;
                    float rf;
                    if (r >= 86) n = 60, filled = int(sec) + 1, on = c_cyan, rf = r - 86;
                    else if (r >= 64) n = 60, filled = nw.m + 1, on = c_yellow, rf = r - 64;
                    else if (r >= 42) n = 12, filled = (nw.h % 12) + 1, on = c_purple, rf = r - 42;
                    else { px[y * CW + x] = c_black; continue; }
                    const uint32_t rel = uint32_t(a16) * n;
                    const int cell = rel >> 16;
                    const uint32_t frac = rel & 0xFFFF, gap = uint32_t(65536.0f / (TAU * r / n));
                    const bool lit = cell < filled;
                    if (frac < gap || frac > 65535 - gap || rf < 1 || rf > 20) col = c_black;
                    else if (!lit) col = c_grey_dark;
                    else if (rf > 18 || frac < gap * 3) col = c_white;
                    else col = on;
                    if (n == 60 && r >= 86 && cell == int(sec)) {
                        // The current second fills up smoothly.
                        const float part = sec - int(sec);
                        if (frac > uint32_t(part * 65535)) col = lit ? c_grey_dark : col;
                    }
                }
                px[y * CW + x] = col;
            }
        }
        char buf[16];
        hourText(buf, sizeof(buf), nw, false);
        c.textCentered(int(C), int(C) - 5, buf, c_white, 2, true);
        snprintf(buf, sizeof(buf), "%s %d", DAYS[nw.wday], nw.mday);
        c.textCentered(int(C), int(C) + 12, nw.known ? buf : "SET TIME", c_dim, 1, false);
    }

    // ------------------------------------------------------------------ menu / set time
    void drawMenu(Gfx &g)
    {
        namespace ui = console::ui;
        char tz[24];
        snprintf(tz, sizeof(tz), "%s%+d:%02d", tz_auto ? "AUTO " : "", tz_min / 60, std::abs(tz_min % 60));
        const ui::PauseMenu::Row rows[] = {
            {"FACE", FACE_NAMES[face]},
            {"CLOCK", h24 ? "24H" : "12H"},
            {"TIME ZONE", tz},
            {"SET TIME", now().known && net_state() == NET_CONNECTED ? "AUTO" : "GO", ui::ACCENT}};
        menu.draw(g, rows, 4, "CLOCK");
    }

    // AUTO (from the network), then whole hours -12 .. +14 by hand, then AUTO again.
    void nextTimeZone()
    {
        if (tz_auto) {
            tz_auto = false;
            tz_min = -12 * 60;
        } else if (tz_min >= 14 * 60) {
            tz_auto = true;
            int m;
            tz_min = net_tz_offset_min(&m) ? m : 0;
        } else {
            tz_min = (tz_min / 60 + 1) * 60;
        }
        save();
    }

    void drawSetTime(Gfx &g)
    {
        namespace ui = console::ui;
        ui::clearScreen(g);
        ui::title(g, "SET TIME");
        char h[8], m[8];
        snprintf(h, sizeof(h), "%02d", set_h);
        snprintf(m, sizeof(m), "%02d", set_m);
        ui::row(g, 0, "HOUR  +1", h);
        ui::row(g, 1, "MINUTE  +1", m);
        ui::row(g, 2, "MINUTE  +10", m);
        ui::row(g, 3, "24H TIME", "", ui::DIM);
        ui::button(g, ui::buttonRect(0, 2), "SAVE");
        ui::outlineButton(g, ui::buttonRect(1, 2), "CANCEL");
    }

    void setTimeTap(int x, int y)
    {
        namespace ui = console::ui;
        if (ui::rowRect(0).hit(x, y)) set_h = (set_h + 1) % 24;
        else if (ui::rowRect(1).hit(x, y)) set_m = (set_m + 1) % 60;
        else if (ui::rowRect(2).hit(x, y)) set_m = (set_m + 10) % 60;
        else if (ui::buttonRect(0, 2).hit(x, y)) {
            // Keep today's date; set the time of day (in local time, so undo the zone).
            timeval tv;
            gettimeofday(&tv, nullptr);
            time_t local = tv.tv_sec + tz_min * 60;
            if (local < 1700000000) local = 1757980800;   // an unknown clock starts on 16 Sep 2025
            tm tmv;
            gmtime_r(&local, &tmv);
            tmv.tm_hour = set_h;
            tmv.tm_min = set_m;
            tmv.tm_sec = 0;
            const time_t days = local - (local % 86400);
            const time_t utc = days + set_h * 3600 + set_m * 60 - tz_min * 60;
            (void)tmv;
            timeval nv = {utc, 0};
            settimeofday(&nv, nullptr);
            setting = false;
            return;
        } else if (ui::buttonRect(1, 2).hit(x, y)) {
            setting = false;
            return;
        }
        set_dirty = true;
    }

    // ------------------------------------------------------------------ frame
    void update(Engine &e, float dt)
    {
        const InputState &in = e.input();
        ges.update(in.touch);
        t += dt;
        // The network knows where we are: follow it (and remember it for offline days).
        int net_tz;
        if (tz_auto && net_tz_offset_min(&net_tz) && net_tz != tz_min) {
            tz_min = net_tz;
            save();
            menu.invalidate();
        }
        if (setting) {
            if (ges.tap) setTimeTap(ges.x, ges.y);
            if (ges.swipe_right || (in.clicked & BTN_B)) setting = false;
            return;
        }
        if (menu.isOpen()) {
            switch (menu.update(e, ges, in)) {
            case 0:
                face = (face + 1) % FACE_COUNT;
                save();
                break;
            case 1:
                h24 = !h24;
                save();
                break;
            case 2:
                nextTimeZone();
                break;
            case 3: {
                const Now nw = now();
                set_h = nw.h;
                set_m = nw.m;
                setting = true;
                set_dirty = true;
                menu.close();
                break;
            }
            }
            return;
        }
        if (ges.swipe_left) {
            menu.open();
            return;
        }
        if (ges.tap || (in.clicked & BTN_B)) {
            face = (face + 1) % FACE_COUNT;
            save();
            wc::audio::play({.f0 = 900, .f1 = 1200, .ms = 40, .wave = wc::audio::Wave::Triangle, .volume = 0.3f});
        }
    }

    void draw(Engine &e, Gfx &g)
    {
        if (setting) {
            if (set_dirty) {
                drawSetTime(g);
                set_dirty = false;
            }
            return;
        }
        if (menu.isOpen()) {
            drawMenu(g);
            return;
        }
        const Now nw = now();
        Canvas &c = canvas;
        switch (face) {
        case POCKET: drawPocket(c, nw); break;
        case LCD: drawLcd(c, nw); break;
        case SKY: drawSky(c, nw); break;
        case TACHO: drawTacho(c, nw); break;
        default: drawRings(c, nw); break;
        }
        if (!nw.known && face != LCD && face != SKY && face != RINGS)
            c.textCentered(int(C), CW - 22, "SET TIME IN THE MENU", c_red, 1, true);
        c.present(e.presenter());
    }
};

Clock::Clock() : s_(new State) {}
Clock::~Clock() { delete s_; }

void Clock::begin(Engine &e)
{
    if (!Polar::init()) ESP_LOGE(TAG, "polar tables alloc failed");
    if (!s_->loadAssets()) ESP_LOGE(TAG, "assets failed to load");
    s_->load();
    ESP_LOGI(TAG, "ready, face %s, tz %+d min", FACE_NAMES[s_->face], s_->tz_min);
}

void Clock::enter(Engine &e)
{
    s_->menu.close();
    s_->setting = false;
}

void Clock::update(Engine &e, float dt) { s_->update(e, std::min(dt, 0.1f)); }

void Clock::draw(Engine &e, Gfx &g)
{
    if (!s_->canvas.pixels()) return;
    s_->draw(e, g);
}

}  // namespace games
