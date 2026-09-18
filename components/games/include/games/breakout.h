// Round Breakout, ported from the "Round breakout prototype" artifact.
//
// Bricks sit in concentric rings around a core; the paddle rides an outer track.
// Controls:
//   touch  - drag anywhere to rotate the paddle (or follow finger / tilt, see PWR)
//   tap    - launch the ball
//   BOOT   - back to the home menu (console-wide)
//   PWR    - short press cycles control mode (tilt -> drag -> follow)
//   swipe left - pause menu (control, tilt direction, speed, home)
#pragma once

#include "engine/engine.h"

namespace games {

class Breakout : public wc::Game {
public:
    Breakout();
    ~Breakout() override;
    void begin(wc::Engine &e) override;
    void enter(wc::Engine &e) override;
    void update(wc::Engine &e, float dt) override;
    void draw(wc::Engine &e, wc::Gfx &g) override;
    bool keepAwake() const override;   // true while a ball is in play

private:
    struct State;
    State *s_;
};

}  // namespace games
