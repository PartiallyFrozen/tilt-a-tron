#include "console/scroll_list.h"

#include <algorithm>
#include <cmath>

namespace console::ui {

float ScrollList::maxOffset() const
{
    return std::max(0.0f, float(count_ * PITCH - 6 - (BOTTOM - TOP)));
}

int ScrollList::update(const wc::Touch &t, const wc::Gestures &g, float dt)
{
    if (t.pressed && t.y >= TOP && t.y < BOTTOM) {
        tracking_ = true;
        dragging_ = false;
        y0_ = last_y_ = t.y;
        off0_ = offset_;
        vel_ = 0;
    }
    if (tracking_ && t.down) {
        if (std::abs(t.y - y0_) > 10) dragging_ = true;
        if (dragging_) {
            const float prev = offset_;
            offset_ = std::clamp(off0_ - float(t.y - y0_), 0.0f, maxOffset());
            if (dt > 0) vel_ = vel_ * 0.5f + ((offset_ - prev) / dt) * 0.5f;   // smoothed fling speed
        }
        last_y_ = t.y;
    }

    int picked = -1;
    if (t.released) {
        if (!dragging_ && g.tap && g.y >= TOP && g.y < BOTTOM && g.x >= ROW_X && g.x < ROW_X + ROW_W) {
            const int local = g.y - TOP + int(offset_);
            const int i = local / PITCH;
            if (i >= 0 && i < count_ && local % PITCH < ROW_H) picked = i;
        }
        if (!dragging_) vel_ = 0;
        tracking_ = dragging_ = false;
    }

    // Momentum after a fling.
    if (!tracking_ && std::fabs(vel_) > 5) {
        offset_ += vel_ * dt;
        vel_ *= std::exp(-dt * 5.0f);
        if (offset_ <= 0 || offset_ >= maxOffset()) {
            offset_ = std::clamp(offset_, 0.0f, maxOffset());
            vel_ = 0;
        }
    }
    return picked;
}

void ScrollList::draw(Gfx &g, const std::function<void(Gfx &, int, int)> &drawRow)
{
    const int off = int(std::lround(offset_));
    if (off == drawn_offset_) return;
    drawn_offset_ = off;

    restoreBg(g, 0, TOP, Gfx::W, BOTTOM - TOP);
    g.setClip(0, TOP, Gfx::W, BOTTOM - TOP);
    for (int i = 0; i < count_; i++) {
        const int y = TOP + i * PITCH - off;
        if (y + ROW_H <= TOP || y >= BOTTOM) continue;
        drawRow(g, i, y);
    }
    g.clearClip();

    // Scrollbar hugging the right edge, only when the list overflows.
    const float max = maxOffset();
    if (max > 0) {
        const int track_h = BOTTOM - TOP - 20;
        const int thumb_h = std::max(24, int(track_h * float(BOTTOM - TOP) / float(count_ * PITCH)));
        const int thumb_y = TOP + 10 + int((track_h - thumb_h) * (offset_ / max));
        g.fillRect(412, TOP + 10, 4, track_h, rgb(40, 40, 48));
        g.fillRect(412, thumb_y, 4, thumb_h, DIM);
    }
}

}  // namespace console::ui
