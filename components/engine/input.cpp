#include "engine/input.h"

#include "board/board.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace wc {

namespace {

portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
volatile bool s_pause_req = false;
volatile bool s_paused = false;
TiltCal s_cal;

struct Shared {
    uint32_t held = 0;
    uint32_t pressed_acc = 0;    // edges accumulated since last snapshot
    uint32_t released_acc = 0;
    uint32_t clicked_acc = 0;     // short press completed
    uint32_t long_acc = 0;        // held past kLongPressUs
    uint32_t double_acc = 0;      // second click within kDoubleClickUs
    bool touch_down = false;
    bool touch_pressed_acc = false;
    bool touch_released_acc = false;
    int tx = 0, ty = 0;
    int64_t t_us = 0;
    Tilt tilt;
    uint32_t touch_hz = 0;
    uint32_t imu_ok = 0, imu_err = 0;
} s;

void input_task(void *)
{
    const TickType_t period = pdMS_TO_TICKS(2);
    TickType_t wake = xTaskGetTickCount();
    uint32_t tick = 0;
    bool was_down = false;
    uint32_t touch_samples = 0;
    int64_t hz_t0 = esp_timer_get_time();

    // Debounce: a button edge is accepted immediately (no added latency), then
    // that button ignores further changes for a lockout window so contact
    // chatter can't register as extra presses or releases.
    constexpr int64_t kLockoutUs = 60000;
    constexpr uint32_t kButtons[] = {BTN_A, BTN_B};
    uint32_t debounced = 0;
    int64_t last_edge_us[2] = {0, 0};
    // Long press fires once while still held; a release before that is a click.
    constexpr int64_t kLongPressUs = 600000;
    int64_t press_start_us[2] = {0, 0};
    bool long_fired[2] = {false, false};
    uint32_t clicked = 0, longp = 0, dbl = 0;
    constexpr int64_t kDoubleClickUs = 400000;
    int64_t last_click_us[2] = {0, 0};

    for (;;) {
        if (s_pause_req) {
            s_paused = true;
            vTaskDelay(pdMS_TO_TICKS(5));
            wake = xTaskGetTickCount();
            continue;
        }
        s_paused = false;

        // --- buttons (every 2 ms)
        uint32_t raw_bits = buttons_read();
        uint32_t raw = 0;
        if (raw_bits & ::BTN_BOOT) raw |= BTN_A;
        if (raw_bits & ::BTN_PWR) raw |= BTN_B;
        const int64_t now_us = esp_timer_get_time();
        for (int i = 0; i < 2; i++) {
            const uint32_t bit = kButtons[i];
            if ((raw & bit) != (debounced & bit) && now_us - last_edge_us[i] >= kLockoutUs) {
                debounced ^= bit;
                last_edge_us[i] = now_us;
                if (debounced & bit) {
                    press_start_us[i] = now_us;
                    long_fired[i] = false;
                } else if (!long_fired[i]) {
                    clicked |= bit;
                    if (last_click_us[i] && now_us - last_click_us[i] < kDoubleClickUs) {
                        dbl |= bit;
                        last_click_us[i] = 0;
                        ESP_LOGI("input", "%s double-click", i ? "PWR" : "BOOT");
                    } else {
                        ESP_LOGD("input", "%s click, held %d ms, %d ms after the last", i ? "PWR" : "BOOT",
                                 int((now_us - press_start_us[i]) / 1000),
                                 last_click_us[i] ? int((now_us - last_click_us[i]) / 1000) : -1);
                        last_click_us[i] = now_us;
                    }
                }
            }
            if ((debounced & bit) && !long_fired[i] && now_us - press_start_us[i] >= kLongPressUs) {
                long_fired[i] = true;
                longp |= bit;
            }
        }
        uint32_t held = debounced;

        // --- touch: on interrupt, or keep polling while held so we see the release
        touch_point_t tp{};
        bool have_touch = false;
        if (touch_irq_pending() || (was_down && tick % 3 == 0)) {
            have_touch = touch_read(&tp) == ESP_OK;
            if (have_touch && tp.down) touch_samples++;
        }

        // --- IMU (every 4 ms)
        imu_sample_t imu{};
        bool have_imu = false;
        if (tick % 2 == 0) {
            have_imu = imu_read(&imu) == ESP_OK;
            have_imu ? s.imu_ok++ : s.imu_err++;
        }

        portENTER_CRITICAL(&s_mux);
        s.pressed_acc |= held & ~s.held;
        s.released_acc |= s.held & ~held;
        s.clicked_acc |= clicked;
        s.long_acc |= longp;
        s.double_acc |= dbl;
        clicked = longp = dbl = 0;
        s.held = held;
        if (have_touch) {
            if (tp.down && !s.touch_down) s.touch_pressed_acc = true;
            if (!tp.down && s.touch_down) s.touch_released_acc = true;
            s.touch_down = tp.down;
            if (tp.down) {
                s.tx = tp.x;
                s.ty = tp.y;
                s.t_us = tp.t_us;
            }
        }
        if (have_imu) {
            // Board axes -> screen axes (verify on hardware with the bench app)
            s.tilt = {imu.ay, imu.ax, imu.az, imu.gy, imu.gx, imu.gz};
        }
        portEXIT_CRITICAL(&s_mux);

        if (have_touch) was_down = tp.down;

        const int64_t now = esp_timer_get_time();
        if (now - hz_t0 >= 1000000) {
            s.touch_hz = touch_samples;
            touch_samples = 0;
            hz_t0 = now;
        }

        tick++;
        xTaskDelayUntil(&wake, period);
    }
}

}  // namespace

bool Input::init()
{
    return xTaskCreatePinnedToCore(input_task, "input", 5120, nullptr, configMAX_PRIORITIES - 3, nullptr, 0) ==
           pdPASS;
}

void Input::pause(bool paused)
{
    s_pause_req = paused;
    if (paused)
        while (!s_paused) vTaskDelay(pdMS_TO_TICKS(2));
}

void Input::snapshot(InputState &out)
{
    portENTER_CRITICAL(&s_mux);
    out.held = s.held;
    out.pressed = s.pressed_acc;
    out.released = s.released_acc;
    out.clicked = s.clicked_acc;
    out.long_press = s.long_acc;
    out.double_clicked = s.double_acc;
    out.touch.down = s.touch_down;
    out.touch.pressed = s.touch_pressed_acc;
    out.touch.released = s.touch_released_acc;
    out.touch.x = s.tx;
    out.touch.y = s.ty;
    out.touch.t_us = s.t_us;
    out.tilt = s.tilt;
    // Offsets first, then the scale: the offset is an error in the reading, so it has to
    // come off before the reading is stretched to where 1 g really is.
    out.tilt.ax = (out.tilt.ax - s_cal.ax) * s_cal.a_scale;
    out.tilt.ay = (out.tilt.ay - s_cal.ay) * s_cal.a_scale;
    out.tilt.az *= s_cal.a_scale;
    out.tilt.gx -= s_cal.gx, out.tilt.gy -= s_cal.gy, out.tilt.gz -= s_cal.gz;
    out.touch_hz = s.touch_hz;
    out.imu_ok = s.imu_ok;
    out.imu_err = s.imu_err;
    s.pressed_acc = s.released_acc = s.clicked_acc = s.long_acc = s.double_acc = 0;
    s.touch_pressed_acc = s.touch_released_acc = false;
    portEXIT_CRITICAL(&s_mux);
}

void Input::setCalibration(const TiltCal &cal)
{
    portENTER_CRITICAL(&s_mux);
    s_cal = cal;
    portEXIT_CRITICAL(&s_mux);
}

TiltCal Input::calibration()
{
    portENTER_CRITICAL(&s_mux);
    const TiltCal cal = s_cal;
    portEXIT_CRITICAL(&s_mux);
    return cal;
}

}  // namespace wc
