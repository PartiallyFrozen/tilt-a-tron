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
    virtual void update(Engine &, float dt) = 0;
    virtual void draw(Engine &, Gfx &) = 0;
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
    void goHome()
    {
        if (home_) pending_ = home_;
    }
    bool isHome() const { return game_ == home_; }

    // Light sleep until PWR is pressed. Takes effect after the current update();
    // on wake the active app gets enter() again. If still asleep after the auto-off
    // time, the console powers down (deep sleep; PWR cold-boots). Hooks let the
    // console pause and resume things the engine doesn't own (Wi-Fi).
    void sleep() { sleep_requested_ = true; }
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

    Gfx &gfx() { return gfx_; }
    Presenter &presenter() { return presenter_; }
    const InputState &input() const { return input_state_; }
    const FrameStats &stats() const { return stats_; }
    uint32_t frame() const { return frame_; }
    int64_t nowUs() const;

private:
    static void loopEntry(void *arg);
    void loop();
    void activate(Game &app);
    void doSleep();
    void irisOut();
    void quiesce();
    [[noreturn]] void powerOff();
    bool sawActivity() const;

    Gfx gfx_;
    Presenter presenter_;
    Input input_;
    InputState input_state_;
    FrameStats stats_;
    Game *game_ = nullptr;
    Game *home_ = nullptr;
    Game *pending_ = nullptr;
    bool sleep_requested_ = false;
    int auto_off_s_ = 120;
    int64_t last_activity_us_ = 0;
    bool dimmed_ = false;
    int64_t last_frame_us_ = 0;
    std::function<void()> before_sleep_, after_wake_;
    std::function<bool()> keep_awake_, busy_;
    uint32_t frame_ = 0;
};

}  // namespace wc
