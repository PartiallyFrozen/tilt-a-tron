// Shared bits of game presentation that aren't engine primitives and aren't console
// menus: the little panel a game drops over the play area to say something.
#pragma once

#include "engine/canvas.h"

namespace games::ui {

// LEVEL 3 / GRAVITY FLIP! / a final score. Three lines at most, centred, on a panel that
// is only as tall as the lines it holds. Four games had their own copy of this; two of
// them were the same code with a different rectangle.
struct BannerStyle {
    uint8_t panel;                 // the fill
    uint8_t border;                // outline, or the accent bars when `bars` is set
    uint8_t mid_color;             // the second line
    uint8_t bottom_color;          // the third line
    int top_scale = 1;             // 2 for a headline
    bool bars = false;             // accent bars above and below instead of an outline
    bool bottom_bold = false;
};

// `cx` is the centre, `y` the top edge, `w` the width - all in canvas pixels.
void banner(wc::Canvas &c, int cx, int y, int w, const char *top, const char *mid,
            const char *bottom, uint8_t top_color, const BannerStyle &style);

}  // namespace games::ui
