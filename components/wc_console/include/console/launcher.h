// Home screen: a carousel of apps. Swipe (or click PWR) to browse, tap to launch,
// double-click PWR to sleep. BOOT in any app returns here.
#pragma once

#include <vector>

#include "console/console.h"
#include "console/theme.h"
#include "engine/gestures.h"

namespace console {

class Launcher : public wc::Game {
public:
    Launcher(const App *apps, int count) : apps_(apps), n_(count) {}
    void begin(wc::Engine &e) override;
    void enter(wc::Engine &e) override;
    void update(wc::Engine &e, float dt) override;
    void draw(wc::Engine &e, wc::Gfx &g) override;
    // Don't power off while a computer has the drive open.
    bool keepAwake() const override;

private:
    static constexpr int ICON_R = 105;
    static constexpr int SPACING = 290;   // neighbors peek in from the edges
    static constexpr int ICON_CY = wc::Gfx::CY - 28;

    void move(int dir);
    void drawIcons(wc::Gfx &g, int shift);
    const Image &iconFor(int app) const;

    const App *apps_;
    int n_;
    int sel_ = 0;
    std::vector<Image> builtin_icons_;   // drawn in code; a theme's icons/<id>.png wins
    uint32_t theme_gen_ = 0;
    wc::Gestures ges_;

    float anim_ = 0;   // horizontal offset of the carousel, eases to 0
    bool full_ = true;
    bool on_computer_ = false;   // the drive is open on a computer right now
    float hint_t_ = 0;
    float battery_t_ = 9;           // seconds since the battery was read
    int battery_pct_ = -1;
    bool battery_charging_ = false, battery_dirty_ = true;
    int hint_ = 0;
    bool hint_dirty_ = true;
    int drawn_shift_ = 1 << 30;
};

}  // namespace console
