// Grand Prix: the whole watch is the steering wheel. Turn it and the picture
// counter-rotates so the horizon stays level, which only works because the screen
// is round.
//
//   turn the watch - steer
//   hold the screen - brake (throttle is automatic)
//   tap            - start / race again
//   swipe left     - pause menu (mirror steering, sound, home)
//   BOOT           - back to the home menu (console-wide)
#pragma once

#include "engine/engine.h"

namespace games {

class Racer : public wc::Game {
public:
    Racer();
    ~Racer() override;
    void begin(wc::Engine &e) override;
    void enter(wc::Engine &e) override;
    void update(wc::Engine &e, float dt) override;
    void draw(wc::Engine &e, wc::Gfx &g) override;
    bool keepAwake() const override;

private:
    struct State;
    State *s_;
};

}  // namespace games
