// Vertically scrolling list of menu rows for the round screen: drag with
// momentum, tap to pick, scrollbar when there's more than fits. The title above
// and buttons below stay fixed; rows are clipped to the viewport between them.
#pragma once

#include <functional>

#include "console/ui.h"
#include "engine/gestures.h"
#include "engine/input.h"

namespace console::ui {

class ScrollList {
public:
    static constexpr int TOP = 96, BOTTOM = 354;   // viewport (4 rows visible)
    static constexpr int PITCH = ROW_H + 6;

    void setCount(int n) { count_ = n; }
    void reset()
    {
        offset_ = vel_ = 0;
        tracking_ = dragging_ = false;
        invalidate();
    }
    void invalidate() { drawn_offset_ = -1; }

    // Returns the tapped row index, or -1.
    int update(const wc::Touch &t, const wc::Gestures &g, float dt);
    // Redraws only when scrolled or invalidated. drawRow(g, index, y) draws one row at y.
    void draw(Gfx &g, const std::function<void(Gfx &, int, int)> &drawRow);
    bool dragging() const { return dragging_; }

private:
    float maxOffset() const;

    int count_ = 0;
    float offset_ = 0, vel_ = 0;
    bool tracking_ = false, dragging_ = false;
    int y0_ = 0, last_y_ = 0;
    float off0_ = 0;
    int drawn_offset_ = -1;
};

}  // namespace console::ui
