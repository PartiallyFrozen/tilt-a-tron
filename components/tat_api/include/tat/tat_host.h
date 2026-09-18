// The console's side of the game contract: wraps a tat_game_t so the launcher can treat
// it like any other app. Built-in games are registered at startup; installed ones will
// arrive the same way once the loader lands, which is the point of doing it like this.
#pragma once

#include "engine/engine.h"
#include "tat/tat_api.h"

namespace tat {

// Fills in the api table a game is handed. Safe to call more than once.
const tat_api_t *api();

class HostedGame : public wc::Game {
public:
    explicit HostedGame(const tat_game_t &game) : g_(game) {}

    void begin(wc::Engine &e) override;
    void enter(wc::Engine &e) override;
    void leave(wc::Engine &e) override;
    void update(wc::Engine &e, float dt) override;
    void draw(wc::Engine &e, wc::Gfx &g) override;
    void redraw() override;
    bool keepAwake() const override;

    const tat_game_t &desc() const { return g_; }

private:
    const tat_game_t &g_;
};

}  // namespace tat
