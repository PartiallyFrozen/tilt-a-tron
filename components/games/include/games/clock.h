// Pocket Watch: a clock with a handful of watch faces. Time comes from the
// network when Wi-Fi is on (NTP), or is set by hand in the menu.
//
//   tap         - next face
//   swipe left  - menu (face, 12/24 h, time zone, set time)
//   BOOT        - back to the home menu (console-wide)
#pragma once

#include "engine/engine.h"

namespace games {

class Clock : public wc::Game {
public:
    Clock();
    ~Clock() override;
    void begin(wc::Engine &e) override;
    void enter(wc::Engine &e) override;
    void update(wc::Engine &e, float dt) override;
    void draw(wc::Engine &e, wc::Gfx &g) override;
    bool keepAwake() const override { return false; }

private:
    struct State;
    State *s_;
};

}  // namespace games
