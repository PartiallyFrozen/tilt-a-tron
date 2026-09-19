#include "loader/packaged_game.h"

#include "console/ui.h"
#include "esp_log.h"

namespace tat {

namespace {
const char *TAG = "package";
}

PackagedGame::~PackagedGame()
{
    delete hosted_;
    loader_close(pkg_);
}

bool PackagedGame::load()
{
    if (hosted_) return true;
    if (failure_) return false;

    if (!entry_.runnable) {
        // Said plainly rather than hidden: an icon that simply vanished after a firmware
        // update would be a mystery, and the player can uninstall it from the app.
        failure_ = entry_.api_major > TAT_API_MAJOR || entry_.api_minor > TAT_API_MINOR
                       ? "NEEDS A NEWER TILT-A-TRON"
                       : "BUILT FOR AN OLDER TILT-A-TRON";
        return false;
    }
    pkg_ = loader_open(entry_.path);
    if (!pkg_) {
        failure_ = "THIS GAME WOULD NOT LOAD";
        return false;
    }
    const tat_game_t *desc = loader_descriptor(pkg_);
    if (!desc) {
        loader_close(pkg_);
        pkg_ = nullptr;
        failure_ = "THIS GAME WOULD NOT LOAD";
        return false;
    }
    hosted_ = new HostedGame(*desc);
    ESP_LOGI(TAG, "%s is ready to run", entry_.name);
    return true;
}

void PackagedGame::begin(wc::Engine &e)
{
    // The engine calls this the first time the app is shown, which is exactly when the
    // package should be read - not a moment earlier.
    if (load()) hosted_->begin(e);
}

void PackagedGame::enter(wc::Engine &e)
{
    drawn_failure_ = false;
    if (load()) hosted_->enter(e);
}

void PackagedGame::leave(wc::Engine &e)
{
    if (hosted_) hosted_->leave(e);
}

void PackagedGame::update(wc::Engine &e, float dt)
{
    if (hosted_) {
        hosted_->update(e, dt);
        return;
    }
    // Nothing to run. Let the player read the reason, then send them home rather than
    // leaving them on a screen with no way out.
    if (e.input().clicked || e.input().touch.released) e.goHome();
}

void PackagedGame::draw(wc::Engine &e, wc::Gfx &g)
{
    if (hosted_) {
        hosted_->draw(e, g);
        return;
    }
    if (drawn_failure_) return;
    drawn_failure_ = true;
    namespace ui = console::ui;
    ui::menuBackground(g);
    ui::title(g, entry_.name, 120);
    ui::hint(g, 200, failure_ ? failure_ : "THIS GAME WOULD NOT LOAD", ui::DANGER);
    ui::hint(g, 250, "TAP TO GO BACK");
}

void PackagedGame::redraw()
{
    drawn_failure_ = false;
    if (hosted_) hosted_->redraw();
}

bool PackagedGame::keepAwake() const { return hosted_ && hosted_->keepAwake(); }

}  // namespace tat
