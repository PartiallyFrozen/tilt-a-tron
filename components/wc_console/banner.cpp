#include "console/banner.h"

namespace console::ui {

void banner(wc::Canvas &c, int cx, int y, int w, const char *top, const char *mid,
            const char *bottom, uint8_t top_color, const BannerStyle &s)
{
    const int top_h = 8 * s.top_scale;
    const int h = 8 + top_h + (mid ? 10 : 0) + (bottom ? 9 : 0);
    const int x = cx - w / 2;

    c.fillRect(x, y, w, h, s.panel);
    if (s.bars) {
        c.fillRect(x, y, w, 2, s.border);
        c.fillRect(x, y + h - 2, w, 2, s.border);
    } else {
        c.rect(x, y, w, h, s.border);
    }

    int ty = y + 4 + top_h / 2;
    c.textCentered(cx, ty, top, top_color, s.top_scale, true);
    ty += top_h / 2 + 6;
    if (mid) {
        c.textCentered(cx, ty, mid, s.mid_color, 1, false);
        ty += 9;
    }
    if (bottom) c.textCentered(cx, ty, bottom, s.bottom_color, 1, s.bottom_bold);
}

}  // namespace console::ui
