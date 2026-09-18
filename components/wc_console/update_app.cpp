#include "console/update_app.h"

#include <algorithm>

#include "console/console.h"
#include "console/ui.h"
#include "engine/polar.h"
#include "esp_system.h"

namespace console {

using wc::Engine;
using wc::Gfx;

static constexpr int C = Gfx::CX;

void UpdateApp::begin(Engine &e)
{
    wc::Polar::init();
    for (int i = 0; i < Gfx::W * Gfx::H; i++) {
        const uint16_t r16 = wc::Polar::radius16(i);
        if (r16 >= 219 * 16 && r16 <= 231 * 16) {
            ring_px_.push_back(i);
            ring_ang_.push_back(uint16_t(wc::Polar::angle(i) + 16384));   // 0 at 12 o'clock
        }
    }
}

void UpdateApp::enter(Engine &e)
{
    char ssid[33];
    const bool saved = net_saved_ssid(ssid, sizeof(ssid));
    if (saved) ssid_ = ssid;

    if (net_state() == NET_CONNECTED) {
        ota_server_start();
        go(St::Ready);
    } else if (saved) {
        go(St::JoinSaved);
    } else {
        setup_.start();
        go(St::Setup);
    }
}

void UpdateApp::go(St s)
{
    st_ = s;
    dirty_ = true;
    shown_ = false;
}

void UpdateApp::exit(Engine &e)
{
    if (back_) e.switchTo(*back_);
    else esp_restart();   // update mode: reboot into the console
}

void UpdateApp::update(Engine &e, float dt)
{
    const auto &in = e.input();
    ges_.update(in.touch);
    const bool busy = st_ == St::Flashing || st_ == St::Done;

    // BOOT is "back to menu" everywhere. The engine handles it when there's a home
    // app; update mode has none, so leave by rebooting into the console.
    if (!back_ && (in.pressed & wc::BTN_A) && !busy) esp_restart();

    switch (st_) {
    case St::JoinSaved:
        if (!shown_) break;   // let "JOINING" reach the screen before blocking
        if (net_join_saved(15000)) {
            ota_server_start();
            go(St::Ready);
        } else {
            setup_.start();
            go(St::Setup);
        }
        break;

    case St::Setup:
        switch (setup_.update(e, ges_, dt)) {
        case WifiSetup::Result::Connected:
            ssid_ = setup_.ssid();
            ota_server_start();
            go(St::Ready);
            break;
        case WifiSetup::Result::Cancelled: exit(e); break;
        default: break;
        }
        break;

    case St::Ready:
        ota_get_status(&ota_);
        if (ota_.state == OTA_RECEIVING) {
            go(St::Flashing);
        } else if (ges_.tap && ui::buttonRect(0, 2).hit(ges_.x, ges_.y)) {
            setup_.start();
            go(St::Setup);
        } else if ((ges_.tap && ui::buttonRect(1, 2).hit(ges_.x, ges_.y)) || ges_.swipe_right) {
            exit(e);
        }
        break;

    case St::Flashing:
        ota_get_status(&ota_);
        if (ota_.state == OTA_DONE) go(St::Done);
        if (ota_.state == OTA_FAILED) go(St::Failed);
        break;

    case St::Done:
        done_t_ += dt;
        if (done_t_ > 1.5f) esp_restart();
        break;

    case St::Failed:
        if (ges_.tap) go(St::Ready);
        break;
    }
}

// ------------------------------------------------------------------ drawing

void UpdateApp::drawReady(Gfx &g)
{
    g.textCentered(C, 88, safeMode() ? "SAFE MODE" : "WI-FI UPDATE", safeMode() ? ui::DANGER : ui::ACCENT, 3, true);
    g.textCentered(C, 148, "READY", ui::GO, 5, true);
    g.textCentered(C, 204, NET_HOSTNAME ".local", ui::VALUE, 2, true);
    g.textCentered(C, 232, net_ip(), ui::TEXT, 2, true);
    std::string s = ssid_.size() > 20 ? ssid_.substr(0, 19) + "~" : ssid_;
    g.textCentered(C, 260, s.c_str(), ui::DIM, 2, true);
    g.textCentered(C, 312, "RUN OTA.PS1 ON PC", ui::DIM, 2, true);
    ui::outlineButton(g, ui::buttonRect(0, 2), "WI-FI");
    ui::outlineButton(g, ui::buttonRect(1, 2), "EXIT");
}

void UpdateApp::drawProgress(Gfx &g, bool full)
{
    if (full) {
        ui::clearScreen(g);
        for (uint32_t i : ring_px_) g.pixels()[i] = wc::rgb(40, 44, 54);
        g.markAllDirty();
        g.textCentered(C, C - 40, "UPDATING", ui::TEXT, 3, true);
        ring_drawn_ = 0;
        drawn_pct_ = -1;
    }
    const int pct = ota_.total ? int(uint64_t(ota_.received) * 100 / ota_.total) : 0;
    if (pct == drawn_pct_) return;

    // Extend the ring from the last drawn angle to the new one; mark only that arc dirty.
    const uint16_t to = uint16_t(std::min<uint32_t>(65535, uint32_t(pct) * 65535 / 100));
    int x0 = Gfx::W, y0 = Gfx::H, x1 = -1, y1 = -1;
    for (size_t k = 0; k < ring_px_.size(); k++) {
        const uint16_t a = ring_ang_[k];
        if (a < ring_drawn_ || a > to) continue;
        const uint32_t i = ring_px_[k];
        g.pixels()[i] = ui::GO;
        const int x = i % Gfx::W, y = i / Gfx::W;
        x0 = std::min(x0, x);
        x1 = std::max(x1, x);
        y0 = std::min(y0, y);
        y1 = std::max(y1, y);
    }
    if (x1 >= 0) g.markDirty(x0, y0, x1 - x0 + 1, y1 - y0 + 1);
    ring_drawn_ = to;

    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", pct);
    ui::restoreBg(g, C - 90, C, 180, 50);
    g.textCentered(C, C + 24, buf, ui::GO, 6, true);
    drawn_pct_ = pct;
}

void UpdateApp::draw(Engine &e, Gfx &g)
{
    if (st_ == St::Setup) {
        setup_.draw(g);
        return;
    }
    if (st_ == St::Flashing) {
        drawProgress(g, dirty_);
        dirty_ = false;
        return;
    }
    if (!dirty_) {
        shown_ = true;
        return;
    }
    dirty_ = false;
    ui::clearScreen(g);

    switch (st_) {
    case St::JoinSaved:
        g.textCentered(C, 150, "WI-FI UPDATE", ui::ACCENT, 3, true);
        g.textCentered(C, C, "JOINING", ui::TEXT, 4, true);
        g.textCentered(C, C + 44, ssid_.substr(0, 20).c_str(), ui::DIM, 2, true);
        break;
    case St::Ready: drawReady(g); break;
    case St::Done:
        g.textCentered(C, C - 20, "UPDATED!", ui::GO, 5, true);
        g.textCentered(C, C + 30, "RESTARTING", ui::DIM, 2, true);
        break;
    case St::Failed:
        g.textCentered(C, C - 40, "UPDATE FAILED", ui::DANGER, 3, true);
        g.textCentered(C, C, ota_.error, ui::TEXT, 2, true);
        g.textCentered(C, C + 50, "TAP TO CONTINUE", ui::DIM, 2, true);
        break;
    default: break;
    }
}

}  // namespace console
