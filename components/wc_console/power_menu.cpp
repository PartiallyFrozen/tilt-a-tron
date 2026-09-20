#include "console/power_menu.h"

#include <algorithm>
#include <cmath>

#include "audio/audio.h"

namespace console {

using namespace ui;

namespace {

constexpr int kRingDots = 60;
constexpr float kGoodbyeS = 1.4f;   // long enough to read two lines, short enough not to be a wait

struct Entry {
    const char *label;
    const char *doing;      // said while it happens
    const char *way_back;   // and how to undo it
};
constexpr Entry kEntries[] = {
    {"SLEEP", "SLEEPING", "PRESS PWR TO WAKE"},
    {"SHUT DOWN", "SHUTTING DOWN", "PRESS PWR TO TURN ON"},
    {"RESTART", "RESTARTING", "BACK IN A MOMENT"},
};

Color entryColor(int i) { return i == 0 ? VALUE : i == 1 ? DANGER : ACCENT; }

}  // namespace

void PowerMenu::enter(wc::Engine &e)
{
    // Brought up by the hold, PWR is still down and there is the rest of the hold to go.
    // Anything else that shows this screen gets the choices straight away.
    phase_ = (e.input().held & wc::BTN_B) ? HOLDING : MENU;
    t_ = 0;
    fresh_ = true;
    busy_ = false;
    dirty_ = true;
    ges_ = wc::Gestures{};
}

void PowerMenu::choose(wc::Engine &e, Choice c)
{
    // A restart in the middle of a file being written is how files are lost.
    if (c != SLEEP && e.powerBusy()) {
        busy_ = true;
        dirty_ = true;
        return;
    }
    choice_ = c;
    phase_ = GOODBYE;
    t_ = 0;
    dirty_ = true;
    wc::audio::play({.f0 = 700, .f1 = 180, .ms = 260, .wave = wc::audio::Wave::Triangle, .volume = 0.6f});
}

void PowerMenu::update(wc::Engine &e, float dt)
{
    const wc::InputState &in = e.input();
    ges_.update(in.touch);
    if (fresh_) {
        fresh_ = false;
        return;
    }
    t_ += dt;

    switch (phase_) {
    case HOLDING:
        if (!(in.held & wc::BTN_B)) {
            e.resume();
        } else if (t_ >= wc::Engine::POWER_HOLD_S - wc::Engine::POWER_PEEK_S) {
            phase_ = MENU;
            t_ = 0;
            dirty_ = true;
            wc::audio::play({.f0 = 520, .f1 = 780, .ms = 120});
        }
        break;

    case MENU:
        // Back is back, here as everywhere: swipe right, or PWR.
        if (ges_.swipe_right || (in.clicked & wc::BTN_B)) {
            e.resume();
            break;
        }
        if (!ges_.tap) break;
        if (buttonRect(0, 1).hit(ges_.x, ges_.y)) {
            e.resume();
            break;
        }
        for (int i = 0; i < CHOICES; i++)
            if (rowRect(i).hit(ges_.x, ges_.y)) choose(e, Choice(i));
        break;

    case GOODBYE:
        if (t_ < kGoodbyeS) break;
        t_ = -1000;   // once
        if (choice_ == SLEEP) {
            e.resume();   // wake into what was running, not into this menu
            e.sleep();
        } else if (choice_ == SHUT_DOWN) {
            e.shutDown();
        } else {
            e.restart();
        }
        break;
    }
}

void PowerMenu::draw(wc::Engine &, wc::Gfx &g)
{
    if (dirty_) {
        dirty_ = false;
        ring_lit_ = -1;
        if (phase_ == GOODBYE) {
            g.clear(wc::colors::black);
            g.textCentered(Gfx::CX, 208, kEntries[choice_].doing, entryColor(choice_), 4, true);
            g.textCentered(Gfx::CX, 268, kEntries[choice_].way_back, LABEL, 2, true);
        } else if (phase_ == HOLDING) {
            g.clear(wc::colors::black);
            g.textCentered(Gfx::CX, 196, "POWER", TEXT, 5, true);
            g.textCentered(Gfx::CX, 256, "KEEP HOLDING", ACCENT, 3, true);
            g.textCentered(Gfx::CX, 300, "LET GO TO CANCEL", DIM, 2, true);
        } else {
            menuBackground(g);
            title(g, "POWER");
            for (int i = 0; i < CHOICES; i++)
                button(g, rowRect(i), kEntries[i].label, entryColor(i), i == 1 ? TEXT : wc::colors::black);
            if (busy_) hint(g, ROW_Y[3] + 26, "BUSY COPYING FILES - TRY AGAIN", DANGER);
            outlineButton(g, buttonRect(0, 1), "CANCEL");
        }
    }
    if (phase_ != HOLDING) return;

    // The ring: the whole hold, so it is already part full when this screen appears. Every
    // dot is drawn once, and after that only the ones that have just lit.
    const float done = (wc::Engine::POWER_PEEK_S + t_) / wc::Engine::POWER_HOLD_S;
    const int lit = std::min(kRingDots, int(done * kRingDots));
    for (int i = ring_lit_ < 0 ? 0 : ring_lit_; i < (ring_lit_ < 0 ? kRingDots : lit); i++) {
        const float a = (float(i) / kRingDots) * 6.2831853f - 1.5707963f;
        const int x = Gfx::CX + int(std::lround(std::cos(a) * 212)), y = Gfx::CY + int(std::lround(std::sin(a) * 212));
        g.fillCircle(x, y, 6, i < lit ? ACCENT : PANEL);
    }
    ring_lit_ = lit;
}

}  // namespace console
