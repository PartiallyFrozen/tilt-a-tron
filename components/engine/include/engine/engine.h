#pragma once

#include <cstdint>
#include <functional>

#include "engine/gfx.h"
#include "engine/input.h"
#include "engine/presenter.h"

namespace wc {

class Engine;

// An app on the console: a game, the home screen, settings. The engine calls
// update() then draw() once per frame on core 1. Draw only what changed;
// everything you touch is shipped.
class Game {
public:
    virtual ~Game() = default;
    virtual void begin(Engine &) {}   // once, the first time the app is shown
    virtual void enter(Engine &) {}   // every time the app becomes active (screen is black)
    virtual void leave(Engine &) {}   // another app is taking over, or the watch is going to sleep
    virtual void update(Engine &, float dt) = 0;
    virtual void draw(Engine &, Gfx &) = 0;
    // Apps that only draw when something changes: draw the next frame regardless.
    virtual void redraw() {}
    // Return true while something is going on that shouldn't be auto-powered-off
    // (a ball in play, a firmware download).
    virtual bool keepAwake() const { return false; }

private:
    friend class Engine;
    bool begun_ = false;
};

struct FrameStats {
    float fps = 0;
    uint32_t update_us = 0;
    uint32_t draw_us = 0;
    uint32_t frame_us = 0;
};

struct EngineConfig {
    uint32_t spi_hz = 40 * 1000 * 1000;
    bool vsync = true;
    uint8_t brightness = 200;
    int present_buffers = 2;   // DMA band buffers (31 KB internal RAM each)
};

class Engine {
public:
    bool init(const EngineConfig &cfg = {});
    void run(Game &first);   // spawns the app loop on core 1 and returns; call it last

    // Console navigation. BOOT anywhere returns to the home app.
    void setHome(Game &home) { home_ = &home; }
    void switchTo(Game &app) { pending_ = &app; }
    // Ask the running app for a fresh frame (GET /screen uses it). Safe from any task.
    void requestRedraw() { redraw_req_ = true; }
    void goHome()
    {
        if (home_) pending_ = home_;
    }
    bool isHome() const { return game_ == home_; }

    // The power menu. Holding PWR anywhere on the console brings `menu` up, and it is the
    // only way to it, so that a watch can always be told to sleep, shut down or restart
    // whatever is running. The menu is shown after POWER_PEEK_S, while PWR is still down,
    // so that the rest of the hold is not spent wondering whether anything is happening; it
    // offers its choices at POWER_HOLD_S, and letting go before that goes back (resume()).
    static constexpr float POWER_PEEK_S = 3.0f, POWER_HOLD_S = 6.0f;
    void setPowerMenu(Game &menu) { power_menu_ = &menu; }
    void resume() { pending_ = before_power_ ? before_power_ : home_; }   // back to what the power menu interrupted
    // Off at the power chip, so nothing is drawn from the battery; PWR turns it on again, from
    // cold. And a restart. Both take effect after the current update(), like sleep().
    void shutDown() { shutdown_requested_ = true; }
    void restart() { restart_requested_ = true; }
    // True while turning off would break something (a file is on its way to the drive).
    bool powerBusy() const { return busy_ && busy_(); }

    // Light sleep until PWR is pressed. Takes effect after the current update();
    // on wake the active app gets enter() again. If still asleep after the auto-off
    // time, the console powers down (deep sleep; PWR cold-boots). Hooks let the
    // console pause and resume things the engine doesn't own (Wi-Fi).
    //
    // On USB power it naps instead: screen and motion sensor off, everything else left
    // running, until PWR - or wake() - or the cable is pulled, when it goes on to sleep
    // properly. See doSleep() for why.
    void sleep() { sleep_requested_ = true; }
    void wake() { wake_requested_ = true; }   // ends a nap; safe from any task
    // Auto off: also powers down after this long with no input and no app keeping
    // it awake (the screen dims for the last 10 s as a warning). 0 = never.
    void setAutoOffSeconds(int seconds) { auto_off_s_ = seconds; }
    // Holds off the idle auto-off (e.g. while plugged in), but the user can still
    // ask for sleep with a double-click.
    void setKeepAwakeHook(std::function<bool()> hook) { keep_awake_ = std::move(hook); }
    // Blocks sleep outright: something would break if the console dozed off now
    // (a computer is copying files onto the drive).
    void setBusyHook(std::function<bool()> hook) { busy_ = std::move(hook); }
    void setSleepHooks(std::function<void()> before, std::function<void()> after)
    {
        before_sleep_ = std::move(before);
        after_wake_ = std::move(after);
    }

    // Debug remote control (driven by the /input endpoint): fake a touch that moves
    // from (x0, y0) to (x1, y1) over `ms`, hold buttons, or override the tilt
    // (pass NAN to go back to the real sensor). Applied on top of the real input.
    //
    // A faked touch or button lasts the milliseconds it was given. A faked tilt used to
    // last until something turned it off, which meant a test script that died - or forgot -
    // left the watch reading a frozen gravity, and every game silently stopped responding
    // to being turned. It expires on its own now; hold it by sending it again.
    void injectTouch(int x0, int y0, int x1, int y1, int ms);
    void injectButton(uint32_t mask, int ms);
    void injectTilt(float ax, float ay, float az);

    Gfx &gfx() { return gfx_; }
    Presenter &presenter() { return presenter_; }
    const InputState &input() const { return input_state_; }
    const FrameStats &stats() const { return stats_; }
    uint32_t frame() const { return frame_; }
    int64_t nowUs() const;

private:
    // tools/emu runs games on a PC against this same class, and has to be able to hand it
    // the input a frame would have had and see where a game asked to go. Nothing on the
    // watch defines this.
    friend struct EngineEmulator;

    static void loopEntry(void *arg);
    void loop();
    void activate(Game &app);
    void doSleep();
    void irisOut();
    void quiesce();
    [[noreturn]] void powerOff();
    [[noreturn]] void doShutDown();
    bool sawActivity() const;

    void applyInjected(int64_t now);

    Gfx gfx_;
    Presenter presenter_;
    Input input_;
    struct {
        int64_t touch_t0 = 0, touch_t1 = 0;
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        bool touch_was = false;
        int64_t btn_until = 0;
        uint32_t btn_mask = 0;
        bool btn_was = false;
        int64_t tilt_until = 0;
        float ax = 0, ay = 0, az = 1;
    } inj_;
    InputState input_state_;
    FrameStats stats_;
    Game *game_ = nullptr;
    Game *home_ = nullptr;
    Game *pending_ = nullptr;
    Game *power_menu_ = nullptr;
    Game *before_power_ = nullptr;
    int64_t pwr_held_since_us_ = 0;
    bool shutdown_requested_ = false, restart_requested_ = false;
    volatile bool redraw_req_ = false;
    bool sleep_requested_ = false;
    volatile bool wake_requested_ = false;
    int auto_off_s_ = 120;
    int64_t last_activity_us_ = 0;
    bool dimmed_ = false;
    int64_t last_frame_us_ = 0;
    std::function<void()> before_sleep_, after_wake_;
    std::function<bool()> keep_awake_, busy_;
    uint32_t frame_ = 0;
};

}  // namespace wc
