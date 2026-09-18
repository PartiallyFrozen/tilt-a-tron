// Sleepy Star: a laser falls straight down onto rings of walls with gaps, mirrors
// and splitters cut into them. Line them up so the light reaches the star's door.
//
//   turn watch  - free: the laser always comes from real-world "up", so turning the
//                 watch moves where it enters (8 notches around the rim)
//   tap a ring  - clicks it round one notch; the ring inside it turns the other way.
//                 Taps are what's counted, against the level's par
//   PWR         - start the level over
//   swipe left  - menu (level, beam colour, sound, reset)
//   BOOT        - back to the home menu (console-wide)
#pragma once

#include "engine/engine.h"

namespace games {

class Star : public wc::Game {
public:
    Star();
    ~Star() override;
    void begin(wc::Engine &e) override;
    void enter(wc::Engine &e) override;
    void update(wc::Engine &e, float dt) override;
    void draw(wc::Engine &e, wc::Gfx &g) override;
    void redraw() override;
    bool keepAwake() const override { return false; }   // a puzzle: no need to hold off sleep

private:
    struct State;
    State *s_;
};

}  // namespace games
