// Input is sampled on core 0 independently of the frame rate (buttons ~500 Hz,
// IMU ~250 Hz, touch on interrupt). Games read a per-frame snapshot; short taps
// that start and end between two frames still produce pressed/released edges.
#pragma once

#include <cstdint>

namespace wc {

enum Button : uint32_t {
    BTN_A = 1 << 0,   // BOOT key (by the USB port): console "home"
    BTN_B = 1 << 1,   // PWR key: app's secondary action
};

struct Touch {
    bool down = false;
    bool pressed = false;    // went down since last frame
    bool released = false;   // went up since last frame
    int x = 0, y = 0;        // last known position (kept after release)
    int64_t t_us = 0;        // sample time of x/y
};

struct Tilt {
    float ax = 0, ay = 0, az = 1;   // g, screen-aligned: +x right, +y down
    float gx = 0, gy = 0, gz = 0;   // deg/s
};

// Resting error of this watch's motion sensor, measured by Settings > CALIBRATE and
// taken off every sample, so "flat" and "still" mean the same thing in every game.
struct TiltCal {
    float ax = 0, ay = 0;           // g read with the watch lying truly level
    float gx = 0, gy = 0, gz = 0;   // deg/s read with the watch still
};

struct InputState {
    uint32_t held = 0;
    uint32_t pressed = 0;      // went down (instant; use for action buttons)
    uint32_t released = 0;
    uint32_t clicked = 0;      // short press, reported on release
    uint32_t long_press = 0;   // held 600 ms (reported once, while still held)
    uint32_t double_clicked = 0;   // second click within 400 ms (the clicks are reported too)
    Touch touch;
    Tilt tilt;
    uint32_t touch_hz = 0;   // measured touch sample rate while finger is down
    uint32_t imu_ok = 0, imu_err = 0;   // cumulative IMU read results
};

class Input {
public:
    bool init();
    // Called once per frame by the engine.
    void snapshot(InputState &out);
    // Stop touching I2C (before light sleep). Blocks until the input task is idle.
    void pause(bool paused);
    // Applies from the next snapshot on.
    static void setCalibration(const TiltCal &cal);
    static TiltCal calibration();
};

}  // namespace wc
