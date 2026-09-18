// The little panel a game drops over the play area to say something: LEVEL 3, GAME OVER,
// a final score. Shared for the same reason the pause menu is - a console where every
// game announces itself differently doesn't feel like one console.
//
// This draws on a Canvas rather than the framebuffer, because that is where a pixel-art
// game's frame is built. It lives here, beside the pause menu, so that both the built-in
// games and tat_api's canvas_banner() can reach it.
#pragma once

#include "engine/canvas.h"

namespace console::ui {

struct BannerStyle {
    uint8_t panel;                 // the fill
    uint8_t border;                // outline, or the accent bars when `bars` is set
    uint8_t mid_color;             // the second line
    uint8_t bottom_color;          // the third line
    int top_scale = 1;             // 2 for a headline
    bool bars = false;             // accent bars above and below instead of an outline
    bool bottom_bold = false;
};

// Three lines at most, centred, on a panel only as tall as the lines it holds. `cx` is
// the centre, `y` the top edge, `w` the width - all in canvas pixels.
void banner(wc::Canvas &c, int cx, int y, int w, const char *top, const char *mid,
            const char *bottom, uint8_t top_color, const BannerStyle &style);

}  // namespace console::ui
