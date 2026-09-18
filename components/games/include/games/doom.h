// Doom. The real engine (components/doom), played by holding the watch like a
// steering wheel:
//
//   turn the watch   - turns you, like the wheel in Grand Prix; the picture counter-
//                      rotates so the world always stays upright
//   tip it forward   - walk (further = run); tip it back to back up. "Level" is however
//                      you were holding it when the game (re)started
//   tap / hold       - fire
//   PWR              - use: open doors, flip switches
//   swipe right      - next weapon
//   swipe left       - menu (skill, sound, walk sensitivity, new game)
//   BOOT             - back to the home menu (console-wide); Doom stays paused in memory
//
// It needs a WAD (game data): put DOOM1.WAD (the free shareware episode) or another
// Doom WAD on the Tilt-a-tron drive, in a folder called Doom. It's copied into flash
// the first time Doom is opened.
#pragma once

#include "engine/engine.h"

namespace games {

class Doom : public wc::Game {
public:
    Doom();
    ~Doom() override;
    void begin(wc::Engine &e) override;
    void enter(wc::Engine &e) override;
    void leave(wc::Engine &e) override;
    void update(wc::Engine &e, float dt) override;
    void draw(wc::Engine &e, wc::Gfx &g) override;
    void redraw() override;
    bool keepAwake() const override;

private:
    struct State;
    State *s_;
};

}  // namespace games
