#include "loader/packaged_game.h"

#include "console/ui.h"
#include "esp_log.h"

namespace tat {

namespace {
const char *TAG = "package";

// The one package that is in memory. A game is its code, a canvas or two, its sprite
// sheets and whatever else it asked for - the best part of a megabyte for some - and the
// first version of this kept every game that had been opened until the watch restarted.
// Ten games in, the ninth would not load. So: opening a game lets go of the last one.
// Going home and coming back to the same game still finds it exactly as it was left.
PackagedGame *resident = nullptr;
}

PackagedGame::~PackagedGame() { unload(); }

void PackagedGame::unload()
{
    if (hosted_) ESP_LOGI(TAG, "%s unloaded", entry_.name);
    delete hosted_;   // tells the game, then takes back everything it was given
    hosted_ = nullptr;
    loader_close(pkg_);
    pkg_ = nullptr;
    begun_hosted_ = false;
    if (resident == this) resident = nullptr;
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
    if (resident && resident != this) resident->unload();
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
    resident = this;
    ESP_LOGI(TAG, "%s is ready to run", entry_.name);
    return true;
}

// Loaded, and begun. The engine calls begin() once in an app's life, but a package can be
// let go and read back in many times, and each time its game starts from nothing.
bool PackagedGame::ready(wc::Engine &e)
{
    if (!load()) return false;
    if (!begun_hosted_) {
        begun_hosted_ = true;
        hosted_->begin(e);
    }
    return true;
}

void PackagedGame::begin(wc::Engine &e)
{
    // The engine calls this the first time the app is shown, which is exactly when the
    // package should be read - not a moment earlier.
    ready(e);
}

void PackagedGame::enter(wc::Engine &e)
{
    drawn_failure_ = false;
    failure_ = nullptr;   // whatever stopped it last time may have passed; try again
    if (ready(e)) hosted_->enter(e);
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
