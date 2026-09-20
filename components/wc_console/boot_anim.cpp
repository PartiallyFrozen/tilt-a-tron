#include "console/boot_anim.h"

#include <cmath>

namespace console {

namespace {

constexpr float kTau = 6.2831853f;
constexpr float kRockFrom = 0.55f;   // of a spin: where it has slowed enough to start rocking

float sq(float v) { return v * v; }

}  // namespace

bool BootAnim::init()
{
    if (!cv_.init(2)) return false;
    ink_ = cv_.color(wc::rgb(255, 255, 255));
    rim_ = cv_.color(wc::rgb(255, 220, 40));
    dim_ = cv_.color(wc::rgb(150, 150, 150));
    go_ = cv_.color(wc::rgb(40, 200, 110));
    const wc::Color pips[6] = {wc::rgb(255, 90, 70),  wc::rgb(255, 160, 40), wc::rgb(90, 220, 120),
                               wc::rgb(60, 200, 235), wc::rgb(120, 130, 255), wc::rgb(235, 90, 200)};
    for (int i = 0; i < 6; i++) pip_[i] = cv_.color(pips[i]);
    return true;
}

bool BootAnim::atRest(float t)
{
    const float u = std::fmod(t, SPIN_S) / SPIN_S;
    return t >= SPIN_S * MIN_SPINS && (u < 0.02f || u > 0.97f);
}

// The top, seen from above: a rim, a ring of pips that makes the turning visible, and the
// name across it. One pip is bigger than the rest, so that a turn reads as a turn and not as
// a twelfth of one.
void BootAnim::disc(float cx, float cy, int name_dy)
{
    cv_.clear(0);
    const int x = int(std::lround(cx)), y = int(std::lround(cy));
    cv_.fillCircle(x, y, 111, rim_);
    cv_.fillCircle(x, y, 108, 0);
    for (int i = 0; i < 12; i++) {
        const float a = kTau * float(i) / 12 - kTau / 4;
        const int px = int(std::lround(cx + std::cos(a) * 95)), py = int(std::lround(cy + std::sin(a) * 95));
        if (i % 6 == 3) continue;   // the name runs through where these two would be
        cv_.fillCircle(px, py, i == 0 ? 7 : 4, i == 0 ? ink_ : pip_[i % 6]);
    }
    cv_.textCentered(x, y - 2 + name_dy, "TILT-A-TRON", ink_, 3, true);
    cv_.fillRect(x - 40, y + 16 + name_dy, 80, 3, rim_);
}

void BootAnim::spin(wc::Presenter &p, float t)
{
    const int n = int(t / SPIN_S);
    const float u = (t - n * SPIN_S) / SPIN_S;
    // Whole turns, so that every spin starts and ends upright; the second flick is gentler.
    const float turns = (n % 2 == 0) ? 3.0f : 2.0f;
    float angle = kTau * turns * (1 - u) * (1 - u) * (1 - u);
    float lean = 0;
    if (u > kRockFrom) {
        // Slow enough to fall over, it rocks instead, less each time, and leans as it goes.
        const float s = (u - kRockFrom) / (1 - kRockFrom);
        angle += 0.40f * std::sin(kTau * 2.5f * s) * sq(1 - s);
        lean = 13.0f * std::sin(3.14159f * s) * (1 - s);
    } else {
        lean = 3.0f * u / kRockFrom;
    }
    // The lean goes round the screen at its own pace, not the disc's: undo the turn the
    // canvas is about to be given.
    const float phase = kTau * 3.2f * u;
    const float sx = lean * std::cos(phase), sy = lean * std::sin(phase);
    const float c = std::cos(angle), s = std::sin(angle);
    const float mid = cv_.width() / 2.0f;
    disc(mid + sx * c + sy * s, mid - sx * s + sy * c);
    cv_.presentRotated(p, angle);
}

void BootAnim::waiting(wc::Presenter &p, const char *title, const char *detail, int done, int total)
{
    const int mid = cv_.width() / 2;
    disc(float(mid), float(mid), -30);
    cv_.textCentered(mid, mid + 24, title, dim_, 2, true);
    cv_.textCentered(mid, mid + 44, detail, rim_, 2, true);
    const int w = 110, x = mid - w / 2, y = mid + 58;
    cv_.rect(x, y, w, 8, dim_);
    if (total > 0) cv_.fillRect(x + 2, y + 2, (w - 4) * done / total, 4, go_);
    cv_.present(p);
}

}  // namespace console
