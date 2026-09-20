// Plays the boot animation on a watch, while the console loads. See boot_anim.h.
#include <cstdio>

#include "audio/audio.h"
#include "console/boot_anim.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace console {

static const char *TAG = "splash";
static portMUX_TYPE s_status_mux = portMUX_INITIALIZER_UNLOCKED;

void BootSplash::start(wc::Engine &e)
{
    engine_ = &e;
    if (!anim_.init()) return;   // no memory for it: a black screen for a few seconds, as before
    done_ = xSemaphoreCreateBinary();
    TaskHandle_t task = nullptr;
    // Core 1, which is idle until the engine's loop starts on it; loading is on core 0.
    if (!done_ || xTaskCreatePinnedToCore(entry, "splash", 6144, this, configMAX_PRIORITIES - 4, &task, 1) != pdPASS) {
        ESP_LOGW(TAG, "could not start");
        return;
    }
    task_ = task;
}

void BootSplash::status(const char *title, const char *detail, int done, int total)
{
    portENTER_CRITICAL(&s_status_mux);
    snprintf(status_.title, sizeof(status_.title), "%s", title);
    snprintf(status_.detail, sizeof(status_.detail), "%s", detail);
    status_.done = done;
    status_.total = total;
    portEXIT_CRITICAL(&s_status_mux);
}

void BootSplash::finish()
{
    if (!task_) return;
    finish_ = true;
    xSemaphoreTake(static_cast<SemaphoreHandle_t>(done_), portMAX_DELAY);
    vSemaphoreDelete(static_cast<SemaphoreHandle_t>(done_));
    task_ = done_ = nullptr;
}

void BootSplash::entry(void *arg)
{
    static_cast<BootSplash *>(arg)->play();
    vTaskDelete(nullptr);
}

void BootSplash::play()
{
    wc::Presenter &p = engine_->presenter();
    float t = 0;
    int64_t last = esp_timer_get_time();
    for (;;) {
        const int64_t now = esp_timer_get_time();
        const float dt = float(now - last) / 1e6f;
        last = now;

        portENTER_CRITICAL(&s_status_mux);
        const auto st = status_;
        portEXIT_CRITICAL(&s_status_mux);
        if (st.total > 0) {
            // A job is being reported: stand still and show it. The spin starts over after.
            anim_.waiting(p, st.title, st.detail, st.done, st.total);
            t = 0;
            vTaskDelay(pdMS_TO_TICKS(30));
            continue;
        }
        t += dt;
        if (finish_ && BootAnim::atRest(t)) break;
        anim_.spin(p, t);
    }
    // Upright and still, to the pixel, for whatever time is left before the home screen.
    anim_.spin(p, 0);
    wc::audio::play({.f0 = 520, .f1 = 1040, .ms = 140, .wave = wc::audio::Wave::Triangle, .volume = 0.5f});
    // The animation streamed its frames past the framebuffer. Tell the presenter that frame
    // is over, or it swallows the first thing the home screen draws.
    p.present(engine_->gfx());
    p.flush();
    xSemaphoreGive(static_cast<SemaphoreHandle_t>(done_));
}

}  // namespace console
