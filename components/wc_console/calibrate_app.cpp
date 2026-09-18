#include "console/calibrate_app.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "audio/audio.h"
#include "console/console.h"
#include "console/ui.h"
#include "esp_log.h"

namespace console {

namespace {

const char *TAG = "calibrate";

constexpr int DISH_X = wc::Gfx::CX, DISH_Y = 206, DISH_R = 72, RING_R = 86;
constexpr int LINE1_Y = 312, LINE2_Y = 338;
constexpr float WINDOW_S = 1.5f;          // how long it has to lie still for a reading
constexpr float STILL_DPS = 2.5f;         // more gyro wobble than this restarts the reading
constexpr float STILL_G = 0.03f;
constexpr float FLAT_Z = 0.9f;            // |az| above this = lying flat
constexpr float TURN_MIN = 150.0f, TURN_MAX = 210.0f;
constexpr float MAX_TILT_G = 0.2f;        // bigger "corrections" than these mean the
constexpr float MAX_DRIFT_DPS = 15.0f;    // table wasn't flat or the watch was moving

enum Msg {
    M_INTRO, M_HANDS_OFF, M_NOT_FLAT, M_TURN, M_TURN_MORE, M_TURN_BACK, M_RESULT, M_FAIL,
};

const char *const LINES[][2] = {
    {"LAY THE WATCH FLAT ON", "A TABLE, SCREEN UP"},
    {"HANDS OFF...", "READING THE SENSOR"},
    {"LAY IT FLAT,", "SCREEN UP"},
    {"SPIN IT HALF A TURN", "KEEP IT FLAT ON THE TABLE"},
    {"A BIT MORE...", "HALF A TURN, THEN LET GO"},
    {"TOO FAR - TURN IT BACK", "TO THE MARK"},
    {"", "BUBBLE IN THE MIDDLE?"},
    {"THAT LOOKED WRONG", "USE A STEADY, FLAT TABLE"},
};

void ringDots(wc::Gfx &g, float from_deg, float to_deg, wc::Color c)
{
    if (to_deg < from_deg) std::swap(from_deg, to_deg);
    for (float a = from_deg; a <= to_deg; a += 4.0f) {
        const float r = (a - 90.0f) * float(M_PI) / 180.0f;
        g.fillCircle(DISH_X + int(std::lround(std::cos(r) * RING_R)), DISH_Y + int(std::lround(std::sin(r) * RING_R)), 5, c);
    }
}

}  // namespace

void CalibrateApp::enter(wc::Engine &e) { setPhase(INTRO); }

void CalibrateApp::setPhase(Phase p)
{
    phase_ = p;
    full_ = true;
    win_.reset();
    switch (p) {
    case INTRO: msg_ = M_INTRO; break;
    case FLAT1:
    case FLAT2: msg_ = M_HANDS_OFF; break;
    case TURN: msg_ = M_TURN, turned_ = 0; break;
    case RESULT: msg_ = M_RESULT; break;
    case FAIL: msg_ = M_FAIL; break;
    }
}

bool CalibrateApp::sample(const wc::Tilt &r, float dt)
{
    const float a[3] = {r.ax, r.ay, r.az}, gy[3] = {r.gx, r.gy, r.gz};
    // Anything that moves it (a finger lifting off, a bumped table) starts the reading over.
    if (win_.n >= 5) {
        for (int i = 0; i < 3; i++) {
            if (std::fabs(a[i] - win_.a[i] / win_.n) > STILL_G || std::fabs(gy[i] - win_.g[i] / win_.n) > STILL_DPS) {
                win_.reset();
                break;
            }
        }
    }
    for (int i = 0; i < 3; i++) win_.a[i] += a[i], win_.g[i] += gy[i];
    win_.n++;
    win_.t += dt;
    return win_.t >= WINDOW_S && win_.n >= 30;
}

void CalibrateApp::finish(bool two_readings)
{
    const float n = float(win_.n);
    const float k = two_readings ? 0.5f : 1.0f;
    found_ = wc::TiltCal{};
    if (two_readings) {
        // Half a turn flips the table's slope but not the sensor's own error.
        found_.ax = (first_a_[0] + win_.a[0] / n) * k;
        found_.ay = (first_a_[1] + win_.a[1] / n) * k;
        found_.gx = (first_g_[0] + win_.g[0] / n) * k;
        found_.gy = (first_g_[1] + win_.g[1] / n) * k;
        found_.gz = (first_g_[2] + win_.g[2] / n) * k;
    } else {
        found_.ax = first_a_[0], found_.ay = first_a_[1];
        found_.gx = first_g_[0], found_.gy = first_g_[1], found_.gz = first_g_[2];
    }
    const float tilt = std::hypot(found_.ax, found_.ay);
    const float drift = std::sqrt(found_.gx * found_.gx + found_.gy * found_.gy + found_.gz * found_.gz);
    ESP_LOGI(TAG, "found: level %.4f,%.4f g  drift %.2f,%.2f,%.2f dps (%s)", found_.ax, found_.ay, found_.gx,
             found_.gy, found_.gz, two_readings ? "two readings" : "one reading");
    const bool ok = tilt <= MAX_TILT_G && drift <= MAX_DRIFT_DPS;
    wc::audio::play(ok ? wc::audio::Tone{.f0 = 660, .f1 = 990, .ms = 140} : wc::audio::Tone{.f0 = 300, .f1 = 180, .ms = 220});
    setPhase(ok ? RESULT : FAIL);
}

void CalibrateApp::leave(wc::Engine &e)
{
    if (back_) e.switchTo(*back_);
    else e.goHome();
}

void CalibrateApp::update(wc::Engine &e, float dt)
{
    const auto &in = e.input();
    ges_.update(in.touch);
    // This screen measures the sensor itself, so undo the correction that's in force.
    const wc::TiltCal cur = wc::Input::calibration();
    raw_ = in.tilt;
    raw_.ax += cur.ax, raw_.ay += cur.ay;
    raw_.gx += cur.gx, raw_.gy += cur.gy, raw_.gz += cur.gz;

    const bool left = ges_.tap && ui::buttonRect(0, 2).hit(ges_.x, ges_.y);
    const bool right = ges_.tap && ui::buttonRect(1, 2).hit(ges_.x, ges_.y);
    const bool single = ges_.tap && ui::buttonRect(0, 1).hit(ges_.x, ges_.y);
    const bool flat = std::fabs(raw_.az) > FLAT_Z;

    switch (phase_) {
    case INTRO:
        if (left || ges_.swipe_right) return leave(e);
        if (right) return setPhase(FLAT1);
        if ((in.clicked & wc::BTN_B) && tiltCalibrated()) {
            clearTiltCalibration();
            wc::audio::play({.f0 = 500, .f1 = 300, .ms = 120});
            full_ = true;
        }
        break;
    case FLAT1:
    case FLAT2:
        if (single) return setPhase(INTRO);
        if (!flat) {
            win_.reset();
            msg_ = M_NOT_FLAT;
        } else {
            msg_ = M_HANDS_OFF;
            if (sample(raw_, dt)) {
                if (phase_ == FLAT2) return finish(true);
                const float n = float(win_.n);
                first_a_[0] = win_.a[0] / n, first_a_[1] = win_.a[1] / n;
                for (int i = 0; i < 3; i++) first_g_[i] = win_.g[i] / n;
                wc::audio::play({.f0 = 660, .f1 = 880, .ms = 90});
                setPhase(TURN);
            }
        }
        break;
    case TURN: {
        if (left) return setPhase(INTRO);
        if (right) return finish(false);   // SKIP: trust the table
        turned_ += (raw_.gz - first_g_[2]) * dt;
        const float t = std::fabs(turned_);
        msg_ = t < 20 ? M_TURN : t < TURN_MIN ? M_TURN_MORE : t > TURN_MAX ? M_TURN_BACK : M_TURN;
        if (t >= TURN_MIN && t <= TURN_MAX && flat) {
            // Far enough; take the second reading as soon as it's let go of.
            if (sample(raw_, dt)) return finish(true);
            if (win_.t > 0.35f) msg_ = M_HANDS_OFF;
        } else {
            win_.reset();
        }
        break;
    }
    case RESULT:
        if (left) return setPhase(FLAT1);
        if (right) {
            saveTiltCalibration(found_);
            wc::audio::play({.f0 = 880, .f1 = 1320, .ms = 160});
            return leave(e);
        }
        break;
    case FAIL:
        if (left) return leave(e);
        if (right) return setPhase(FLAT1);
        break;
    }
    if ((in.clicked & wc::BTN_B) && phase_ != INTRO) setPhase(INTRO);
}

void CalibrateApp::draw(wc::Engine &e, wc::Gfx &g)
{
    if (msg_ != drawn_msg_) full_ = true;
    if (full_) {
        ui::clearScreen(g);
        ui::title(g, "CALIBRATE");
        drawn_msg_ = msg_;
        const char *l1 = LINES[msg_][0];
        char buf[40];
        if (phase_ == RESULT) {
            const float deg = std::asin(std::min(1.0f, std::hypot(found_.ax, found_.ay))) * 180.0f / float(M_PI);
            const float drift = std::sqrt(found_.gx * found_.gx + found_.gy * found_.gy + found_.gz * found_.gz);
            snprintf(buf, sizeof(buf), "TILT %.1f DEG  DRIFT %.1f/S", deg, drift);
            l1 = buf;
        }
        ui::hint(g, LINE1_Y, l1, ui::TEXT);
        ui::hint(g, LINE2_Y, LINES[msg_][1], phase_ == FAIL ? ui::DANGER : ui::DIM);
        if (phase_ == INTRO && tiltCalibrated()) ui::hint(g, 96, "SAVED - PWR CLEARS IT", ui::GO);
        switch (phase_) {
        case INTRO:
            ui::outlineButton(g, ui::buttonRect(0, 2), "BACK");
            ui::button(g, ui::buttonRect(1, 2), "START");
            break;
        case FLAT1:
        case FLAT2: ui::outlineButton(g, ui::buttonRect(0, 1), "CANCEL"); break;
        case TURN:
            ui::outlineButton(g, ui::buttonRect(0, 2), "CANCEL");
            ui::outlineButton(g, ui::buttonRect(1, 2), "SKIP");
            break;
        case RESULT:
            ui::outlineButton(g, ui::buttonRect(0, 2), "REDO");
            ui::button(g, ui::buttonRect(1, 2), "SAVE");
            break;
        case FAIL:
            ui::outlineButton(g, ui::buttonRect(0, 2), "BACK");
            ui::button(g, ui::buttonRect(1, 2), "REDO");
            break;
        }
        full_ = false;
    } else {
        const int r = RING_R + 8;
        ui::restoreBg(g, DISH_X - r, DISH_Y - r, r * 2, r * 2);
    }

    // The live part: a bubble level in a dish, with a progress ring around it.
    g.fillCircle(DISH_X, DISH_Y, DISH_R, ui::PANEL);
    g.circle(DISH_X, DISH_Y, DISH_R, ui::BOX);
    g.circle(DISH_X, DISH_Y, DISH_R - 1, ui::BOX);
    g.circle(DISH_X, DISH_Y, 18, ui::BOX);
    g.hline(DISH_X - DISH_R + 6, DISH_Y, DISH_R * 2 - 12, ui::BOX);
    g.vline(DISH_X, DISH_Y - DISH_R + 6, DISH_R * 2 - 12, ui::BOX);

    // What the games would see: the saved correction, or on the result screen the new one.
    const wc::TiltCal cal = phase_ == RESULT ? found_ : wc::Input::calibration();
    const float bx = -(raw_.ax - cal.ax), by = -(raw_.ay - cal.ay);   // a bubble rises to the high side
    constexpr float PX_PER_G = 420.0f;   // about 7 px per degree
    float px = bx * PX_PER_G, py = by * PX_PER_G;
    const float d = std::hypot(px, py), lim = float(DISH_R - 16);
    if (d > lim) px *= lim / d, py *= lim / d;
    const bool centred = d < 9.0f;
    g.fillCircle(DISH_X + int(px), DISH_Y + int(py), 13, centred ? ui::GO : ui::ACCENT);
    g.fillCircle(DISH_X + int(px) - 4, DISH_Y + int(py) - 4, 3, ui::TEXT);

    if (phase_ == TURN) {
        ringDots(g, 0, 360, ui::BOX);
        const float sign = turned_ < 0 ? -1.0f : 1.0f;
        ringDots(g, 0, sign * std::min(std::fabs(turned_), 360.0f), ui::VALUE);
        ringDots(g, sign * 178, sign * 182, ui::GO);   // the half-turn mark
        if (win_.t > 0.35f) ringDots(g, 0, 360 * std::min(1.0f, win_.t / WINDOW_S), ui::GO);
    } else if (phase_ == FLAT1 || phase_ == FLAT2) {
        ringDots(g, 0, 360, ui::BOX);
        if (win_.t > 0) ringDots(g, 0, 360 * std::min(1.0f, win_.t / WINDOW_S), ui::GO);
    }
}

}  // namespace console
