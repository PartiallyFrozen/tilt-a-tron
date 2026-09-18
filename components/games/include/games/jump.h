// Sky Jump: tilt to steer Hopper up an endless tower of platforms. Bounce on
// springs, dodge the crumbling ledges, shoot or stomp the monsters, and climb
// from a sunny morning all the way up into the stars.
//
//   tilt        - steer left / right
//   tap         - start / shoot straight up
//   swipe left  - pause menu (tilt sensitivity, sound, new game)
//   BOOT        - back to the home menu (console-wide)
#pragma once

#include "engine/engine.h"

namespace games {

class Jump : public wc::Game {
public:
    Jump();
    ~Jump() override;
    void begin(wc::Engine &e) override;
    void enter(wc::Engine &e) override;
    void update(wc::Engine &e, float dt) override;
    void draw(wc::Engine &e, wc::Gfx &g) override;
    bool keepAwake() const override;   // true while a run is in progress

private:
    struct State;
    State *s_;
};

}  // namespace games
