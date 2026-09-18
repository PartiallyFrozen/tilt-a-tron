// Settings > CALIBRATE: a short walkthrough that measures this watch's motion sensor
// at rest (what it reads lying level, and how much the gyro drifts when still) and
// saves the correction for every game.
//
//   1. lay it flat, hands off      -> first reading
//   2. spin it half a turn, flat   -> second reading; averaging the two cancels out a
//                                     table that isn't level, leaving only the sensor's error
//   3. a live bubble level shows the result; SAVE or REDO
#pragma once

#include "engine/engine.h"
#include "engine/gestures.h"

namespace console {

class CalibrateApp : public wc::Game {
public:
    explicit CalibrateApp(wc::Game *back) : back_(back) {}
    void enter(wc::Engine &e) override;
    void update(wc::Engine &e, float dt) override;
    void draw(wc::Engine &e, wc::Gfx &g) override;
    bool keepAwake() const override { return phase_ == FLAT1 || phase_ == TURN || phase_ == FLAT2; }

private:
    enum Phase { INTRO, FLAT1, TURN, FLAT2, RESULT, FAIL };
    // A run of samples taken while the watch isn't moving.
    struct Window {
        int n = 0;
        float t = 0;
        float a[3] = {0, 0, 0}, g[3] = {0, 0, 0};   // sums
        void reset() { *this = Window{}; }
    };

    void setPhase(Phase p);
    bool sample(const wc::Tilt &raw, float dt);   // true once the window is full
    void finish(bool two_readings);
    void leave(wc::Engine &e);

    wc::Game *back_;
    wc::Gestures ges_;
    Phase phase_ = INTRO;
    bool full_ = true;
    int msg_ = 0, drawn_msg_ = -1;
    Window win_;
    float first_a_[3] = {0, 0, 0}, first_g_[3] = {0, 0, 0};
    float turned_ = 0;         // degrees since the first reading
    const char *fail_line1_ = nullptr, *fail_line2_ = nullptr;   // which check said no
    wc::Tilt raw_;             // latest uncorrected sample
    wc::TiltCal found_;
};

}  // namespace console
