// "First light" bring-up app: exercises display, touch, tilt, buttons and reports
// pipeline timings on screen and over serial.
//   BOOT (A): toggle vsync
//   PWR  (B): toggle full-screen stress test (worst-case frame throughput)
//   tilt:     roll the ball
//   touch:    drag the cyan dot
#pragma once

#include "engine/engine.h"

class BenchGame : public wc::Game {
public:
    void begin(wc::Engine &e) override;
    void update(wc::Engine &e, float dt) override;
    void draw(wc::Engine &e, wc::Gfx &g) override;

private:
    void drawStatic(wc::Gfx &g);
    void drawHud(wc::Engine &e, wc::Gfx &g);

    float bx_ = wc::Gfx::CX, by_ = wc::Gfx::CY + 60, bvx_ = 0, bvy_ = 0;
    int drawn_bx_ = -1000, drawn_by_ = -1000;
    int drawn_tx_ = -1000, drawn_ty_ = -1000;
    bool stress_ = false, need_static_ = true;
    uint32_t stress_hue_ = 0;
    int64_t hud_t_ = 0, log_t_ = 0;
    int64_t calib_start_us_ = INT64_MAX / 2;   // set when the calibration screen is first drawn
    bool calib_drawn_ = false;
};
