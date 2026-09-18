#include "console/pause_menu.h"

#include "audio/audio.h"

namespace console::ui {

int PauseMenu::update(wc::Engine &e, const wc::Gestures &ges, const wc::InputState &in)
{
    // Swipe right is "back" everywhere on the console, and PWR is the secondary action:
    // in a pause menu both mean carry on playing.
    if (ges.swipe_right || (in.clicked & wc::BTN_B)) {
        close();
        return Closed;
    }
    if (!ges.tap) return None;

    if (buttonRect(0, 2).hit(ges.x, ges.y)) {
        close();
        return Closed;
    }
    if (buttonRect(1, 2).hit(ges.x, ges.y)) {
        close();
        e.goHome();
        return Closed;
    }
    for (int i = 0; i < 4; i++) {
        if (!rowRect(i).hit(ges.x, ges.y)) continue;
        dirty_ = true;   // a tapped row nearly always changes what it says
        return i;
    }
    return None;
}

void PauseMenu::draw(wc::Gfx &g, const Row *rows, int count, const char *title)
{
    if (!dirty_) return;
    dirty_ = false;
    g.clear(SCRIM);
    ui::title(g, title);
    for (int i = 0; i < count && i < 4; i++) row(g, i, rows[i].label, rows[i].value, rows[i].color);
    button(g, buttonRect(0, 2), "RESUME");
    outlineButton(g, buttonRect(1, 2), "HOME");
}

PauseMenu::Row PauseMenu::soundRow() const
{
    const bool on = wc::audio::volume() > 0;
    return {"SOUND", on ? "ON" : "OFF", on ? GO : DIM};
}

void PauseMenu::toggleSound()
{
    const bool was_on = wc::audio::volume() > 0;
    wc::audio::setVolume(was_on ? 0 : 2);
    // A blip so you hear that it came back on.
    if (!was_on) wc::audio::play({.f0 = 660, .f1 = 880, .ms = 90});
}

}  // namespace console::ui
