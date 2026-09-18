// Shared console UI: every app's menus use these so text size, row layout and
// buttons look and behave the same everywhere (see README "Shared console UX").
#pragma once

#include <cstdio>
#include <cstring>

#include "engine/gfx.h"

namespace console::ui {

using wc::Color;
using wc::Gfx;
using wc::rgb;

// Palette: set from the active theme's theme.json (these are the built-in values).
inline Color BG = rgb(0, 0, 0);
inline Color TEXT = rgb(255, 255, 255);
inline Color PANEL = rgb(16, 18, 26);   // solid fill behind rows/buttons, so busy
                                        // backgrounds don't show through the text
inline Color BOX = rgb(90, 90, 100);
inline Color SCRIM = rgb(30, 32, 40);   // solid backdrop behind a menu, so text always reads
inline Color LABEL = rgb(225, 225, 225);
inline Color VALUE = wc::colors::cyan;
inline Color GO = rgb(40, 200, 110);
inline Color DIM = rgb(150, 150, 150);
inline Color ACCENT = wc::colors::yellow;
inline Color DANGER = wc::colors::red;

// Theme background (image or color). Defined in theme.cpp.
// dx/dy shift the picture for a parallax effect on the home screen.
void clearScreen(Gfx &g, int dx = 0, int dy = 0);
// Menus sit on a solid backdrop, not the theme picture: a title over a busy background
// is unreadable, and every menu on the console should feel like the same screen.
inline void menuBackground(Gfx &g) { g.clear(SCRIM); }
inline void restoreMenuBg(Gfx &g, int x, int y, int w, int h) { g.fillRect(x, y, w, h, SCRIM); }
void restoreBg(Gfx &g, int x, int y, int w, int h, int dx = 0, int dy = 0);

struct Rect {
    int x, y, w, h;
    bool hit(int px, int py) const { return px >= x && px < x + w && py >= y && py < y + h; }
};

// Standard layout on the 466 px round screen: title, up to 4 rows, 1–2 buttons.
// Menus with more rows use ScrollList (same row style, scrolls between title and buttons).
constexpr int TITLE_Y = 62;
constexpr int ROW_X = 63, ROW_W = 340, ROW_H = 58;
constexpr int ROW_Y[4] = {104, 168, 232, 296};
constexpr int BUTTON_Y = 364, BUTTON_H = 54;

inline Rect rowRect(int i) { return {ROW_X, ROW_Y[i], ROW_W, ROW_H}; }
// Two buttons side by side, or one centered (index 0 of 1).
inline Rect buttonRect(int i, int count)
{
    if (count == 1) return {153, BUTTON_Y, 160, BUTTON_H};
    return {i == 0 ? 96 : 240, BUTTON_Y, 130, BUTTON_H};
}

// Text drawn straight onto the background gets a soft shadow so it stays readable
// over whatever picture the theme uses.
inline void shadowText(Gfx &g, int cx, int cy, const char *s, Color c, int scale)
{
    g.textCentered(cx + 2, cy + 2, s, rgb(0, 0, 0), scale, true);
    g.textCentered(cx, cy, s, c, scale, true);
}

inline void title(Gfx &g, const char *s, int y = TITLE_Y, Color c = TEXT)
{
    shadowText(g, Gfx::CX, y, s, c, 4);
}

inline void hint(Gfx &g, int y, const char *s, Color c = DIM) { shadowText(g, Gfx::CX, y, s, c, 2); }

// A settings row at any y (used directly by ScrollList).
inline void rowAt(Gfx &g, int y, const char *label, const char *value, Color vc = VALUE)
{
    const Rect r{ROW_X, y, ROW_W, ROW_H};
    g.fillRect(r.x, r.y, r.w, r.h, PANEL);
    g.rect(r.x, r.y, r.w, r.h, BOX);
    g.rect(r.x + 1, r.y + 1, r.w - 2, r.h - 2, BOX);
    g.text(r.x + 16, r.y + r.h / 2 - 7, label, LABEL, 2, true);
    // Value is right-aligned at scale 3; long values (network names) drop to
    // scale 2 and get truncated with "~" to fit beside the label.
    const int room = r.w - 48 - Gfx::textWidth(label, 2, true);
    char buf[40];
    snprintf(buf, sizeof(buf), "%s", value);
    const int scale = Gfx::textWidth(buf, 3, true) <= room ? 3 : 2;
    if (scale == 2) {
        for (size_t n = strlen(buf); n > 1 && Gfx::textWidth(buf, 2, true) > room; n--) {
            buf[n - 2] = '~';
            buf[n - 1] = 0;
        }
    }
    const int vw = Gfx::textWidth(buf, scale, true);
    g.text(r.x + r.w - 16 - vw, r.y + r.h / 2 - (scale == 3 ? 10 : 7), buf, vc, scale, true);
}

inline void row(Gfx &g, int i, const char *label, const char *value, Color vc = VALUE)
{
    rowAt(g, ROW_Y[i], label, value, vc);
}

inline void button(Gfx &g, const Rect &r, const char *label, Color fill = GO, Color text = wc::colors::black)
{
    g.fillRect(r.x, r.y, r.w, r.h, fill);
    const int scale = Gfx::textWidth(label, 3, true) <= r.w - 12 ? 3 : 2;
    g.textCentered(r.x + r.w / 2, r.y + r.h / 2, label, text, scale, true);
}

inline void outlineButton(Gfx &g, const Rect &r, const char *label, Color c = LABEL)
{
    g.fillRect(r.x, r.y, r.w, r.h, PANEL);
    g.rect(r.x, r.y, r.w, r.h, BOX);
    g.rect(r.x + 1, r.y + 1, r.w - 2, r.h - 2, BOX);
    const int scale = Gfx::textWidth(label, 3, true) <= r.w - 12 ? 3 : 2;
    g.textCentered(r.x + r.w / 2, r.y + r.h / 2, label, c, scale, true);
}

}  // namespace console::ui
