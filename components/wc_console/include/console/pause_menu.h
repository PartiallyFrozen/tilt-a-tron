// The pause screen. Every game shows the same one: swipe left to open it, the same title,
// the same rows, the same RESUME and HOME buttons, and swipe right or PWR to get back to
// playing. It owns the open/closed state and the "only redraw when something changed" rule,
// so a game just says what its rows are.
//
//   update():
//       if (ges.swipe_left) menu_.open();
//       if (menu_.isOpen()) {
//           switch (menu_.update(e, ges, in)) {
//           case 0: tilt_sens = (tilt_sens + 1) % 3; save(); break;
//           case 1: menu_.toggleSound(); break;
//           case 2: newGame(); menu_.close(); break;
//           }
//           return;
//       }
//
//   draw():
//       if (menu_.isOpen()) {
//           const ui::PauseMenu::Row rows[] = {{"TILT", kSens[tilt_sens]}, menu_.soundRow(),
//                                              {"RESTART GAME", "GO", ui::ACCENT}};
//           menu_.draw(g, rows, 3);
//           return;
//       }
#pragma once

#include "console/ui.h"
#include "engine/engine.h"
#include "engine/gestures.h"

namespace console::ui {

class PauseMenu {
public:
    // What update() gives back: a row index, or one of these.
    enum { None = -1, Closed = -2 };

    struct Row {
        const char *label;
        const char *value;
        Color color = VALUE;
    };

    void open()
    {
        open_ = true;
        dirty_ = true;
    }
    void close() { open_ = false; }
    bool isOpen() const { return open_; }
    // Ask for a repaint. A row tap does this for you.
    void invalidate() { dirty_ = true; }

    // Handles RESUME, HOME, swipe right and PWR itself; a row tap comes back as its index.
    int update(wc::Engine &e, const wc::Gestures &ges, const wc::InputState &in);

    // Draws only when something changed, so a paused game sends nothing to the panel.
    void draw(wc::Gfx &g, const Row *rows, int count, const char *title = "PAUSED");

    // The SOUND row, which nearly every game has, spelled the same way in all of them.
    Row soundRow() const;
    void toggleSound();

private:
    bool open_ = false, dirty_ = false;
};

}  // namespace console::ui
