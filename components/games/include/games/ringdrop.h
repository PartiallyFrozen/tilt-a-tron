// Ring Drop: radial Tetris. Wedge-shaped pieces fall inward from the rim; fill a
// whole ring and it clears. The pile is locked to the real world, so turning the
// watch spins the pile under the falling piece.
//
//   turn watch  - rotate the pile (the piece stays put at the top of the screen)
//   tap         - rotate the piece
//   hold        - soft drop
//   PWR         - hard drop
//   swipe left  - pause menu (sound, new game, best)
//   BOOT        - back to the home menu (console-wide)
#pragma once

#include "engine/engine.h"

namespace games {

class RingDrop : public wc::Game {
public:
    RingDrop();
    ~RingDrop() override;
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
