// Tilt-a-tris: radial Tetris. Wedge-shaped pieces fall inward from the rim; fill a
// whole ring and it clears. The pile is part of the watch and turns with it; the
// falling piece hangs from the real world's "up", so turning the watch moves the
// piece around the rim relative to the pile.
//
//   turn watch  - aim: the piece stays "up" while the pile turns beneath it
//   tap         - rotate the piece
//   hold        - soft drop
//   PWR         - hard drop
//   swipe left  - pause menu (sound, new game, best)
//   BOOT        - back to the home menu (console-wide)
#pragma once

#include "engine/engine.h"

namespace games {

class Tiltatris : public wc::Game {
public:
    Tiltatris();
    ~Tiltatris() override;
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
