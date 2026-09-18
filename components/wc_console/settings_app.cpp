#include "console/settings_app.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>

#include "console/console.h"
#include "audio/audio.h"
#include "console/theme.h"
#include "esp_system.h"
#include "storage/storage.h"
#include "console/ui.h"
#include "net/net.h"

namespace console {

void WifiApp::update(wc::Engine &e, float dt)
{
    ges_.update(e.input().touch);
    const WifiSetup::Result r = setup_.update(e, ges_, dt);
    if (r == WifiSetup::Result::Running) return;
    // Scanning powers the radio; if the user backed out with Wi-Fi off, power it down again.
    if (r == WifiSetup::Result::Cancelled && !net_enabled()) net_set_enabled(false);
    if (back_) e.switchTo(*back_);
}

void SettingsApp::activate(wc::Engine &e, int item)
{
    switch (item) {
    case BRIGHTNESS: setBrightnessLevel(brightnessLevel() + 1); break;
    case VOLUME:
        wc::audio::setVolume(wc::audio::volume() + 1);
        wc::audio::play({.f0 = 660, .f1 = 880, .ms = 90});   // so you hear the new level
        break;
    case THEME: {
        // Cycle through the theme folders on the drive.
        const auto names = Theme::get().available();
        if (names.empty()) return;
        auto it = std::find(names.begin(), names.end(), Theme::get().name());
        const size_t next = it == names.end() ? 0 : (size_t(it - names.begin()) + 1) % names.size();
        Theme::get().setActive(names[next]);
        return;   // generation bump repaints the whole screen
    }
    case DEBUG_MODE:
        // Switches what the USB port is (flashing/logs vs. the theme drive): needs a restart.
        storage_set_debug_mode(!storage_debug_mode());
        esp_restart();
        return;
    case WIFI: {
        const bool on = !net_enabled();
        char ssid[33];
        if (on && !net_saved_ssid(ssid, sizeof(ssid)) && wifi_) {
            e.switchTo(*wifi_);   // nothing to connect to yet: set up a network (turns Wi-Fi on)
            return;
        }
        net_set_enabled(on);
        break;
    }
    case NETWORK:
        if (wifi_) e.switchTo(*wifi_);
        return;
    case AUTO_OFF:
        setAutoOffIndex(autoOffIndex() + 1);
        e.setAutoOffSeconds(autoOffSeconds());
        break;
    case UPDATE:
        if (net_state() == NET_CONNECTED && updater_) e.switchTo(*updater_);
        else net_request_update_mode();   // reboots into the update screen
        return;
    default: return;
    }
    list_.invalidate();
}

void SettingsApp::update(wc::Engine &e, float dt)
{
    // Wi-Fi status changes in the background; keep the row current.
    if (int(net_state()) != drawn_net_state_) list_.invalidate();
    Theme::get().poll();
    if (Theme::get().generation() != theme_gen_) {
        theme_gen_ = Theme::get().generation();
        full_ = true;
    }

    const auto &touch = e.input().touch;
    ges_.update(touch);
    list_.setCount(ITEM_COUNT);
    const int picked = list_.update(touch, ges_, dt);
    if (picked >= 0) {
        activate(e, picked);
        return;
    }
    if (ges_.swipe_right && !list_.dragging()) {
        e.goHome();
        return;
    }
    if (ges_.tap && ui::buttonRect(0, 1).hit(ges_.x, ges_.y)) e.goHome();
}

void SettingsApp::drawRow(wc::Gfx &g, int item, int y)
{
    switch (item) {
    case BRIGHTNESS: ui::rowAt(g, y, "BRIGHTNESS", BRIGHTNESS_NAMES[brightnessLevel()]); break;
    case VOLUME: ui::rowAt(g, y, "VOLUME", wc::audio::LEVEL_NAMES[wc::audio::volume()]); break;
    case THEME: {
        std::string name = Theme::get().name();
        for (auto &ch : name) ch = char(std::toupper(ch));
        ui::rowAt(g, y, "THEME", name.c_str());
        break;
    }
    case DEBUG_MODE:
        ui::rowAt(g, y, "DEBUG MODE", storage_debug_mode() ? "ON" : "OFF",
                  storage_debug_mode() ? ui::ACCENT : ui::DIM);
        break;
    case WIFI: {
        const char *value = "OFF";
        wc::Color color = ui::DIM;
        switch (net_state()) {
        case NET_CONNECTED: value = "ON"; color = ui::GO; break;
        case NET_CONNECTING:
        case NET_NO_NETWORK: value = "ON"; color = ui::ACCENT; break;
        case NET_FAILED: value = "ERROR"; color = wc::colors::red; break;
        case NET_OFF: break;
        }
        ui::rowAt(g, y, "WI-FI", value, color);
        break;
    }
    case NETWORK: {
        char ssid[33];
        ui::rowAt(g, y, "NETWORK", net_saved_ssid(ssid, sizeof(ssid)) ? ssid : "SET UP", ui::LABEL);
        break;
    }
    case AUTO_OFF: ui::rowAt(g, y, "AUTO OFF", AUTO_OFF_NAMES[autoOffIndex()]); break;
    case UPDATE: ui::rowAt(g, y, "UPDATE", "GO", ui::ACCENT); break;
    case VERSION: ui::rowAt(g, y, "VERSION", firmwareVersion(), ui::DIM); break;
    }
}

void SettingsApp::draw(wc::Engine &e, wc::Gfx &g)
{
    if (full_) {
        ui::clearScreen(g);
        ui::title(g, "SETTINGS");
        ui::outlineButton(g, ui::buttonRect(0, 1), "HOME");
        list_.invalidate();
        full_ = false;
    }
    drawn_net_state_ = int(net_state());
    list_.draw(g, [this](wc::Gfx &gg, int item, int y) { drawRow(gg, item, y); });
}

}  // namespace console
