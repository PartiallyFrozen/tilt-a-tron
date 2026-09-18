// Marble Maze: tilt the watch to roll a steel ball through a randomly generated
// labyrinth. Dead ends may hide a hole; fall in and you lose a ball.
//
//   tilt        - roll the ball ("level" is however you hold it when you tap start)
//   tap         - start / play again
//   swipe left  - pause menu (recenter, sound, home)
//   BOOT        - back to the home menu (console-wide)
#pragma once

#include "engine/engine.h"

namespace games {

class Maze : public wc::Game {
public:
    Maze();
    ~Maze() override;
    void begin(wc::Engine &e) override;
    void enter(wc::Engine &e) override;
    void update(wc::Engine &e, float dt) override;
    void draw(wc::Engine &e, wc::Gfx &g) override;
    bool keepAwake() const override;   // true while the ball is actually rolling

private:
    struct State;
    State *s_;
};

}  // namespace games
