#include "console/wifi_setup.h"

#include <algorithm>
#include <cstring>

#include "console/ui.h"

namespace console {

using wc::Gfx;

static constexpr int C = Gfx::CX;

// ---- on-screen keyboard geometry
static constexpr int KEY_W = 40, KEY_H = 44, KEY_GAP = 2, KB_Y = 132, KB_ROW = KEY_H + 6;
static const char *const ROWS_LOWER[] = {"1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm"};
static const char *const ROWS_UPPER[] = {"1234567890", "QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"};
static const char *const ROWS_SYM[] = {"!@#$%^&*()", "-_=+[]{}\\|", ";:'\",.<>/", "?~`"};

// ---- network list geometry
static constexpr int NL_Y = 106, NL_H = 50, NL_ROWS = 5, NL_BTN_Y = 366;

static ui::Rect keyRect(int row, int col, int n)
{
    int x0 = C - (n * (KEY_W + KEY_GAP)) / 2;
    if (row == 3) x0 = C - ((n + 2) * (KEY_W + KEY_GAP)) / 2 + (KEY_W + KEY_GAP);
    return {x0 + col * (KEY_W + KEY_GAP), KB_Y + row * KB_ROW, KEY_W, KEY_H};
}

static void key(Gfx &g, const ui::Rect &r, const char *label, bool active = false)
{
    g.fillRect(r.x, r.y, r.w, r.h, active ? ui::GO : wc::rgb(38, 42, 52));
    g.textCentered(r.x + r.w / 2, r.y + r.h / 2, label, active ? wc::colors::black : ui::TEXT, 2, true);
}

void WifiSetup::start()
{
    note_.clear();
    cancel_ = false;
    go(St::Scanning);
}

void WifiSetup::go(St s)
{
    st_ = s;
    dirty_ = true;
    shown_ = false;
}

WifiSetup::Result WifiSetup::update(wc::Engine &e, const wc::Gestures &ges, float dt)
{
    switch (st_) {
    case St::Scanning:
        if (!shown_) break;
        ap_count_ = net_scan(aps_, 24);
        first_ = 0;
        go(St::List);
        break;
    case St::List: updateList(ges); break;
    case St::Keyboard: updateKeyboard(ges); break;
    case St::Joining:
        if (!shown_) break;
        if (net_join(ssid_.c_str(), pass_.c_str(), 15000)) {
            joined_t_ = 0;
            go(St::Joined);
        } else {
            note_ = "COULDN'T JOIN";
            go(St::List);
        }
        break;
    case St::Joined:
        joined_t_ += dt;
        if (joined_t_ > 1.2f) return Result::Connected;
        break;
    }
    if (cancel_) {
        cancel_ = false;
        return Result::Cancelled;
    }
    return Result::Running;
}

void WifiSetup::updateList(const wc::Gestures &ges)
{
    if (ges.swipe_right) {
        cancel_ = true;
        return;
    }
    if (!ges.tap) return;
    const int x = ges.x, y = ges.y;
    if (y >= NL_BTN_Y - 4 && y < NL_BTN_Y + KEY_H + 4) {
        if (x < C - 76) {
            if (first_ > 0) first_ = std::max(0, first_ - NL_ROWS);
        } else if (x < C + 76) {
            note_.clear();
            go(St::Scanning);
            return;
        } else if (first_ + NL_ROWS < ap_count_) {
            first_ += NL_ROWS;
        }
        dirty_ = true;
        return;
    }
    const int i = (y - NL_Y) / NL_H;
    if (y >= NL_Y && i >= 0 && i < NL_ROWS && first_ + i < ap_count_ && x > 50 && x < Gfx::W - 50) {
        ssid_ = aps_[first_ + i].ssid;
        pass_.clear();
        upper_ = sym_ = false;
        go(aps_[first_ + i].secure ? St::Keyboard : St::Joining);
    }
}

void WifiSetup::updateKeyboard(const wc::Gestures &ges)
{
    if (ges.swipe_right) {
        go(St::List);
        return;
    }
    if (!ges.tap) return;
    const int x = ges.x, y = ges.y;
    const char *const *rows = sym_ ? ROWS_SYM : upper_ ? ROWS_UPPER : ROWS_LOWER;

    const int bottom = KB_Y + 4 * KB_ROW;
    if (y >= bottom - 4 && y < bottom + KEY_H + 4) {
        if (x >= C - 150 && x < C - 80) {
            sym_ = !sym_;
            upper_ = false;
        } else if (x >= C - 74 && x < C + 74) {
            pass_ += ' ';
        } else if (x >= C + 80 && x < C + 150) {
            go(St::Joining);
            return;
        }
        dirty_ = true;
        return;
    }
    for (int r = 0; r < 4; r++) {
        const int n = std::strlen(rows[r]);
        const ui::Rect first = keyRect(r, 0, n);
        if (y < first.y - 3 || y >= first.y + KEY_H + 3) continue;
        if (r == 3 && x < first.x) {
            if (sym_) sym_ = false;
            else upper_ = !upper_;
        } else if (r == 3 && x >= first.x + n * (KEY_W + KEY_GAP)) {
            if (!pass_.empty()) pass_.pop_back();
        } else {
            const int c = (x - first.x) / (KEY_W + KEY_GAP);
            if (x < first.x || c < 0 || c >= n) return;
            pass_ += rows[r][c];
            if (upper_ && !sym_) upper_ = false;
        }
        dirty_ = true;
        return;
    }
}

// ------------------------------------------------------------------ drawing

void WifiSetup::drawList(Gfx &g)
{
    g.textCentered(C, 62, "CHOOSE WI-FI", ui::TEXT, 3, true);
    if (!note_.empty()) g.textCentered(C, 88, note_.c_str(), ui::DANGER, 2, true);
    if (ap_count_ == 0) g.textCentered(C, NL_Y + 80, "NO NETWORKS", ui::DIM, 2, true);
    for (int i = 0; i < NL_ROWS && first_ + i < ap_count_; i++) {
        const net_ap_t &ap = aps_[first_ + i];
        const int y = NL_Y + i * NL_H;
        g.fillRect(60, y, Gfx::W - 120, NL_H - 6, wc::rgb(30, 34, 44));
        std::string name = ap.ssid;
        if (name.size() > 18) name = name.substr(0, 17) + "~";
        g.text(76, y + (NL_H - 6) / 2 - 7, name.c_str(), ui::TEXT, 2, true);
        const int bars = std::clamp((ap.rssi + 100) / 12, 1, 4);
        for (int b = 0; b < 4; b++)
            g.fillRect(Gfx::W - 104 + b * 8, y + 32 - b * 6, 6, 6 + b * 6, b < bars ? ui::VALUE : ui::BOX);
    }
    key(g, {C - 150, NL_BTN_Y, 70, KEY_H}, "<");
    key(g, {C - 74, NL_BTN_Y, 148, KEY_H}, "RESCAN");
    key(g, {C + 80, NL_BTN_Y, 70, KEY_H}, ">");
    g.textCentered(C, 432, "SWIPE > BACK", ui::DIM, 2, true);
}

void WifiSetup::drawKeyboard(Gfx &g)
{
    std::string title = ssid_.size() > 20 ? ssid_.substr(0, 19) + "~" : ssid_;
    g.textCentered(C, 52, title.c_str(), ui::DIM, 2, true);
    g.fillRect(60, 76, Gfx::W - 120, 42, wc::rgb(30, 34, 44));
    std::string shown = pass_.size() > 22 ? pass_.substr(pass_.size() - 22) : pass_;
    shown += "_";
    g.text(74, 90, shown.c_str(), ui::TEXT, 2, true);

    const char *const *rows = sym_ ? ROWS_SYM : upper_ ? ROWS_UPPER : ROWS_LOWER;
    for (int r = 0; r < 4; r++) {
        const int n = std::strlen(rows[r]);
        for (int c = 0; c < n; c++) {
            const char label[2] = {rows[r][c], 0};
            key(g, keyRect(r, c, n), label);
        }
        if (r == 3) {
            const ui::Rect first = keyRect(r, 0, n);
            key(g, {first.x - (KEY_W + KEY_GAP), first.y, KEY_W, KEY_H}, sym_ ? "ab" : "^", upper_ && !sym_);
            key(g, {first.x + n * (KEY_W + KEY_GAP), first.y, KEY_W, KEY_H}, "<");
        }
    }
    const int y = KB_Y + 4 * KB_ROW;
    key(g, {C - 150, y, 70, KEY_H}, sym_ ? "abc" : "#+=");
    key(g, {C - 74, y, 148, KEY_H}, "SPACE");
    key(g, {C + 80, y, 70, KEY_H}, "OK", true);
    g.textCentered(C, 408, "SWIPE > BACK", ui::DIM, 2, true);
}

void WifiSetup::draw(Gfx &g)
{
    if (!dirty_) {
        shown_ = true;
        return;
    }
    dirty_ = false;
    ui::clearScreen(g);
    switch (st_) {
    case St::Scanning:
        g.textCentered(C, C, "SCANNING", ui::TEXT, 4, true);
        break;
    case St::List: drawList(g); break;
    case St::Keyboard: drawKeyboard(g); break;
    case St::Joining:
        g.textCentered(C, C - 10, "JOINING", ui::TEXT, 4, true);
        g.textCentered(C, C + 34, ssid_.substr(0, 20).c_str(), ui::DIM, 2, true);
        break;
    case St::Joined:
        g.textCentered(C, C - 10, "CONNECTED", ui::GO, 4, true);
        g.textCentered(C, C + 34, net_ip(), ui::DIM, 2, true);
        break;
    }
}

}  // namespace console
