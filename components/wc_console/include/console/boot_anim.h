// The boot animation: TILT-A-TRON spun like a top. It spins, slows, wobbles and settles
// upright; then it is flicked again.
//
// It is what says "this watch has just been turned on", as against woken from sleep, which
// resumes where it was with no ceremony at all. It costs next to no time, because it plays
// while the console loads rather than before it: BootSplash runs it on the core the games
// will use, while app_main gets on with storage, the theme and Wi-Fi on the other.
//
// BootAnim is the picture and nothing else, so tools/emu can show it. BootSplash is the task
// that plays it on a watch.
#pragma once

#include "engine/canvas.h"
#include "engine/engine.h"

namespace console {

class BootAnim {
public:
    static constexpr float SPIN_S = 1.5f;   // one spin, from the flick to upright and still
    static constexpr int MIN_SPINS = 2;

    bool init();
    // The frame for `t` seconds in.
    void spin(wc::Presenter &p, float t);
    // Standing still, with what is being waited for underneath: the first start of a new
    // watch copies its games into place, which takes a while and should say so.
    void waiting(wc::Presenter &p, const char *title, const char *detail, int done, int total);
    // True at the moments the top is upright and still, which are the only ones to stop on.
    static bool atRest(float t);

private:
    void disc(float cx, float cy, int name_dy = 0);

    wc::Canvas cv_;
    uint8_t ink_ = 0, rim_ = 0, dim_ = 0, go_ = 0, pip_[6] = {};
};

class BootSplash {
public:
    // Starts playing. Returns at once.
    void start(wc::Engine &e);
    bool running() const { return task_ != nullptr; }
    // Shows a job under the logo instead of spinning; total = 0 goes back to spinning.
    void status(const char *title, const char *detail, int done, int total);
    // Lets the animation reach its next rest (and its minimum length), then returns. The
    // screen belongs to the caller again after this.
    void finish();

private:
    static void entry(void *arg);
    void play();

    wc::Engine *engine_ = nullptr;
    BootAnim anim_;
    void *task_ = nullptr;
    void *done_ = nullptr;   // semaphore given by the task on its way out
    volatile bool finish_ = false;
    struct {
        char title[24], detail[32];
        int done, total;
    } status_ = {};
};

}  // namespace console
