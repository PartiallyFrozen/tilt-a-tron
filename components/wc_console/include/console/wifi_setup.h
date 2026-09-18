// Pick a Wi-Fi network and type its password on screen (layout from PeakPal's
// setup, sized for the round 466 px panel). Used by Settings -> NETWORK and by
// the update screen. Not an app itself: its owner forwards update/draw.
#pragma once

#include <string>

#include "engine/engine.h"
#include "engine/gestures.h"
#include "net/net.h"

namespace console {

class WifiSetup {
public:
    enum class Result { Running, Connected, Cancelled };

    void start();   // scan, then show the list
    Result update(wc::Engine &e, const wc::Gestures &ges, float dt);
    void draw(wc::Gfx &g);
    const std::string &ssid() const { return ssid_; }

private:
    enum class St { Scanning, List, Keyboard, Joining, Joined };
    void go(St s);
    void updateList(const wc::Gestures &ges);
    void updateKeyboard(const wc::Gestures &ges);
    void drawList(wc::Gfx &g);
    void drawKeyboard(wc::Gfx &g);

    St st_ = St::Scanning;
    bool dirty_ = true;
    bool shown_ = false;   // current state has reached the screen (safe to block)
    bool cancel_ = false;
    float joined_t_ = 0;

    net_ap_t aps_[24];
    int ap_count_ = 0, first_ = 0;
    std::string note_, ssid_, pass_;
    bool upper_ = false, sym_ = false;
};

}  // namespace console
