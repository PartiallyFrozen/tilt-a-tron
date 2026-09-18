// Per-pixel polar coordinates for the round panel, measured from the true center
// (233.0, 233.0) to each pixel's center. Round games shade rings, arcs and sectors
// with two table lookups instead of atan2/sqrt per pixel.
#pragma once

#include <cstdint>

namespace wc {

class Polar {
public:
    static bool init();   // ~870 KB in PSRAM, built once
    // Angle 0..65535 maps to 0..2π, atan2(dy, dx) with +y pointing down the screen.
    static uint16_t angle(int idx) { return ang_[idx]; }
    // Distance from center in 1/16 px.
    static uint16_t radius16(int idx) { return rad_[idx]; }

    // The whole tables, for a game reached through tat_api: it shades a pixel at a time
    // across the screen, so it has to index these itself. A call per pixel per table would
    // be a few million indirect calls a second for nothing.
    static const uint16_t *angles() { return ang_; }
    static const uint16_t *radii() { return rad_; }

private:
    static inline uint16_t *ang_ = nullptr;
    static inline uint16_t *rad_ = nullptr;
};

}  // namespace wc
