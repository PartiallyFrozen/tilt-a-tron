// Shared touch gestures so every game and console screen reads taps and swipes
// the same way (console UX: swipe left = settings, swipe right = back).
#pragma once

#include <cstdint>
#include <cstdlib>

#include "engine/input.h"

namespace wc {

struct Gestures {
    // Set for exactly one frame when the finger lifts.
    bool tap = false;
    bool swipe_left = false;
    bool swipe_right = false;
    int x = 0, y = 0;   // where the gesture started

    void update(const Touch &t)
    {
        tap = swipe_left = swipe_right = false;
        if (t.pressed) {
            x0_ = t.x;
            y0_ = t.y;
            t0_ = t.t_us;
        }
        if (t.released) {
            const int dx = t.x - x0_, dy = t.y - y0_;
            x = x0_;
            y = y0_;
            if (t.t_us - t0_ < 700000 && std::abs(dx) > 90 && std::abs(dy) < std::abs(dx) * 0.6f) {
                swipe_left = dx < 0;
                swipe_right = dx > 0;
            }
            tap = std::abs(dx) < 25 && std::abs(dy) < 25;
        }
    }

private:
    int x0_ = 0, y0_ = 0;
    int64_t t0_ = 0;
};

}  // namespace wc
