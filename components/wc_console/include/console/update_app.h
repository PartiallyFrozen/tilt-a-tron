// Wi-Fi firmware update screen.
//
// Two ways in:
//  - From Settings while Wi-Fi is connected: opens directly (`back` = Settings).
//  - Update mode: Settings reboots into it when Wi-Fi is off, so a game session
//    never pays for Wi-Fi it didn't ask for (`back` = nullptr, exit reboots).
#pragma once

#include <vector>

#include "console/wifi_setup.h"
#include "engine/engine.h"
#include "engine/gestures.h"
#include "net/net.h"

namespace console {

class UpdateApp : public wc::Game {
public:
    explicit UpdateApp(wc::Game *back = nullptr) : back_(back) {}
    void begin(wc::Engine &e) override;
    void enter(wc::Engine &e) override;
    void update(wc::Engine &e, float dt) override;
    void draw(wc::Engine &e, wc::Gfx &g) override;
    // Waiting for or installing firmware: don't power off underneath it.
    bool keepAwake() const override { return st_ == St::Ready || st_ == St::Flashing || st_ == St::Done; }

private:
    enum class St { JoinSaved, Setup, Ready, Flashing, Done, Failed };
    void go(St s);
    void exit(wc::Engine &e);
    void drawReady(wc::Gfx &g);
    void drawProgress(wc::Gfx &g, bool full);

    wc::Game *back_;
    St st_ = St::JoinSaved;
    bool dirty_ = true;
    bool shown_ = false;
    wc::Gestures ges_;
    WifiSetup setup_;
    std::string ssid_;

    ota_status_t ota_{};
    int drawn_pct_ = -1;
    float done_t_ = 0;
    bool eject_note_ = false;   // "eject the drive" shown on the Done screen
    std::vector<uint32_t> ring_px_;   // rim pixels for the progress ring
    std::vector<uint16_t> ring_ang_;  // their angle from 12 o'clock, clockwise
    uint16_t ring_drawn_ = 0;
};

}  // namespace console
