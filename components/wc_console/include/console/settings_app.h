// Console settings (scrollable): brightness, theme, Wi-Fi on/off, network, auto off,
// debug mode, firmware update, version.
#pragma once

#include "console/scroll_list.h"
#include "console/wifi_setup.h"
#include "engine/engine.h"
#include "engine/gestures.h"

namespace console {

// Settings -> NETWORK: pick a network and type its password, then return.
class WifiApp : public wc::Game {
public:
    explicit WifiApp(wc::Game *back) : back_(back) {}
    void enter(wc::Engine &e) override { setup_.start(); }
    void update(wc::Engine &e, float dt) override;
    void draw(wc::Engine &e, wc::Gfx &g) override { setup_.draw(g); }

private:
    wc::Game *back_;
    WifiSetup setup_;
    wc::Gestures ges_;
};

class SettingsApp : public wc::Game {
public:
    void setScreens(wc::Game *wifi, wc::Game *updater)
    {
        wifi_ = wifi;
        updater_ = updater;
    }
    void enter(wc::Engine &e) override { full_ = true; }
    void update(wc::Engine &e, float dt) override;
    void draw(wc::Engine &e, wc::Gfx &g) override;

private:
    enum Item { BRIGHTNESS, VOLUME, THEME, WIFI, NETWORK, AUTO_OFF, DEBUG_MODE, UPDATE, VERSION, ITEM_COUNT };
    void activate(wc::Engine &e, int item);
    void drawRow(wc::Gfx &g, int item, int y);

    wc::Game *wifi_ = nullptr;
    wc::Game *updater_ = nullptr;
    wc::Gestures ges_;
    ui::ScrollList list_;
    bool full_ = true;
    int drawn_net_state_ = -1;
    uint32_t theme_gen_ = 0;
};

}  // namespace console
