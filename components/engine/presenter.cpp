#include "engine/presenter.h"

#include <algorithm>
#include <cstring>

#include "board/board.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace wc {

static const char *TAG = "present";

namespace {
enum : uint8_t { FRAME_START = 1, STREAM = 2, FULL_FRAME = 4 };
struct Item {
    Color *px;
    int16_t x, y, w, h;
    uint8_t flags;
};
constexpr Item *kFrameEnd = nullptr;
int s_pool_size = 0;
}  // namespace

#define FREE_Q (static_cast<QueueHandle_t>(free_q_))
#define WORK_Q (static_cast<QueueHandle_t>(work_q_))

bool Presenter::init(int pool_buffers, int buffer_rows)
{
    buf_pixels_ = Gfx::W * buffer_rows;
    // At most one frame may wait for the wire. present() blocks here until the
    // previous frame starts transmitting, so the game always renders from fresh
    // input instead of piling up stale frames (latency) behind vsync.
    idle_sem_ = xSemaphoreCreateBinary();
    xSemaphoreGive(static_cast<SemaphoreHandle_t>(idle_sem_));
    free_q_ = xQueueCreate(pool_buffers, sizeof(Item *));
    work_q_ = xQueueCreate(pool_buffers + 8, sizeof(Item *));

    for (int i = 0; i < pool_buffers; i++) {
        auto *it = new Item{};
        it->px = static_cast<Color *>(
            heap_caps_malloc(buf_pixels_ * sizeof(Color), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
        if (!it->px) {
            ESP_LOGW(TAG, "only %d DMA buffers fit", i);
            delete it;
            break;
        }
        xQueueSend(FREE_Q, &it, 0);
        s_pool_size++;
    }
    if (s_pool_size == 0) return false;

    ESP_LOGI(TAG, "%d x %d KB DMA band buffers, internal free %u KB", s_pool_size,
             buf_pixels_ * 2 / 1024, unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
    xTaskCreatePinnedToCore(taskEntry, "present", 4096, this, configMAX_PRIORITIES - 2, nullptr, 0);
    return true;
}

void Presenter::emitRect(Gfx &gfx, int x0, int x1, int y0, int y1, bool &first)
{
    const int w = x1 - x0;
    // CO5300 needs even-aligned windows in both axes; rect heights are band
    // multiples (even), so keep chunk heights even too.
    const int max_rows = std::max(2, (buf_pixels_ / w) & ~1);
    const Color *fb = gfx.pixels();

    for (int y = y0; y < y1;) {
        const int rows = std::min(max_rows, y1 - y);
        Item *it;
        xQueueReceive(FREE_Q, &it, portMAX_DELAY);   // backpressure when the wire is busy
        Color *dst = it->px;
        for (int r = 0; r < rows; r++, dst += w)
            std::memcpy(dst, fb + (y + r) * Gfx::W + x0, w * sizeof(Color));
        it->x = int16_t(x0);
        it->y = int16_t(y);
        it->w = int16_t(w);
        it->h = int16_t(rows);
        it->flags = first ? FRAME_START : 0;
        first = false;
        xQueueSend(WORK_Q, &it, portMAX_DELAY);
        stats_.last_bytes += w * rows * 2;
        y += rows;
    }
}

void Presenter::present(Gfx &gfx)
{
    const int64_t t0 = esp_timer_get_time();
    bool first = true;

    bool any = false;
    for (int b = 0; b < Gfx::BANDS && !any; b++) any = gfx.band(b).x0 < gfx.band(b).x1;
    if (!any) {
        // A game that streamed its frame with presentBands() has already sent pixels;
        // keep its byte count so the engine doesn't think the screen was idle.
        if (streamed_) streamed_ = false;
        else stats_.last_bytes = 0;
        stats_.last_copy_us = 0;
        return;
    }
    stats_.last_bytes = 0;
    xSemaphoreTake(static_cast<SemaphoreHandle_t>(idle_sem_), portMAX_DELAY);
    const int64_t t_slot = esp_timer_get_time();
    stats_.last_wait_us = uint32_t(t_slot - t0);

    // Walk dirty bands top to bottom, clip to the round panel, and merge vertically
    // adjacent bands into one rectangle when that doesn't waste much area.
    int rx0 = 0, rx1 = 0, ry0 = 0, ry1 = 0;
    bool run = false;
    for (int b = 0; b < Gfx::BANDS; b++) {
        const auto &d = gfx.band(b);
        int vx0, vx1;
        Gfx::visibleSpan(b, vx0, vx1);
        int x0 = std::max<int>(d.x0 & ~1, vx0);
        int x1 = std::min<int>((d.x1 + 1) & ~1, vx1);
        const int by0 = b * Gfx::BAND_H, by1 = std::min(Gfx::H, by0 + Gfx::BAND_H);
        if (d.x0 >= d.x1 || x0 >= x1) {
            if (run) emitRect(gfx, rx0, rx1, ry0, ry1, first);
            run = false;
            continue;
        }
        if (run && ry1 == by0) {
            const int ux0 = std::min(rx0, x0), ux1 = std::max(rx1, x1);
            const int separate = (rx1 - rx0) * (ry1 - ry0) + (x1 - x0) * (by1 - by0);
            const int merged = (ux1 - ux0) * (by1 - ry0);
            if (merged * 10 <= separate * 13) {
                rx0 = ux0; rx1 = ux1; ry1 = by1;
                continue;
            }
            emitRect(gfx, rx0, rx1, ry0, ry1, first);
        }
        rx0 = x0; rx1 = x1; ry0 = by0; ry1 = by1;
        run = true;
    }
    if (run) emitRect(gfx, rx0, rx1, ry0, ry1, first);

    if (first) {
        // Everything dirty was outside the round panel; nothing to send.
        xSemaphoreGive(static_cast<SemaphoreHandle_t>(idle_sem_));
    } else {
        Item *end = kFrameEnd;
        xQueueSend(WORK_Q, &end, portMAX_DELAY);
    }
    gfx.clearDirty();
    stats_.last_copy_us = uint32_t(esp_timer_get_time() - t_slot);
}

void Presenter::presentBands(const BandFill &fill)
{
    const int64_t t0 = esp_timer_get_time();
    xSemaphoreTake(static_cast<SemaphoreHandle_t>(idle_sem_), portMAX_DELAY);
    stats_.last_wait_us = uint32_t(esp_timer_get_time() - t0);
    stats_.last_bytes = 0;

    // The wire is the bottleneck for a full-screen frame, so send as few bytes as
    // possible: each band is only as wide as the round panel is at that height.
    // (Measured: addressing each band costs far less than the corner pixels it saves.)
    const int rows_max = std::max(2, (buf_pixels_ / Gfx::W) & ~1);
    bool first = true;
    for (int y = 0; y < Gfx::H; y += rows_max) {
        const int rows = std::min(rows_max, Gfx::H - y);
        int x0 = Gfx::W, x1 = 0;
        for (int b = y / Gfx::BAND_H; b <= (y + rows - 1) / Gfx::BAND_H; b++) {
            int vx0, vx1;
            Gfx::visibleSpan(b, vx0, vx1);
            x0 = std::min(x0, vx0);
            x1 = std::max(x1, vx1);
        }
        if (x1 <= x0) continue;

        Item *it;
        xQueueReceive(FREE_Q, &it, portMAX_DELAY);   // waits here while the wire is busy
        fill(y, rows, x0, x1 - x0, it->px);
        if (snap_) {
            Color *fb = snap_->pixels();
            for (int r = 0; r < rows; r++)
                std::memcpy(fb + (y + r) * Gfx::W + x0, it->px + r * (x1 - x0), (x1 - x0) * sizeof(Color));
        }
        it->x = int16_t(x0);
        it->y = int16_t(y);
        it->w = int16_t(x1 - x0);
        it->h = int16_t(rows);
        it->flags = uint8_t(FULL_FRAME | (first ? FRAME_START : 0));
        first = false;
        xQueueSend(WORK_Q, &it, portMAX_DELAY);
        stats_.last_bytes += (x1 - x0) * rows * 2;
    }
    Item *end = kFrameEnd;
    xQueueSend(WORK_Q, &end, portMAX_DELAY);
    streamed_ = true;
    snap_ = nullptr;
    stats_.last_copy_us = uint32_t(esp_timer_get_time() - t0) - stats_.last_wait_us;
}

void Presenter::flush()
{
    while (uxQueueMessagesWaiting(WORK_Q) > 0 || int(uxQueueMessagesWaiting(FREE_Q)) < s_pool_size)
        vTaskDelay(1);
}

void Presenter::taskEntry(void *arg) { static_cast<Presenter *>(arg)->taskLoop(); }

void Presenter::taskLoop()
{
    int64_t frame_t0 = 0;
    for (;;) {
        Item *it;
        xQueueReceive(WORK_Q, &it, portMAX_DELAY);
        if (it == kFrameEnd) {
            stats_.last_xfer_us = uint32_t(esp_timer_get_time() - frame_t0);
            stats_.frames++;
            continue;
        }
        if (it->flags & FRAME_START) {
            // A full-screen stream is written top to bottom about as fast as the panel
            // scans, so starting a few ms after the blank can't overtake the scan (no
            // tear). Waiting for the *next* blank instead would halve the frame rate.
            const bool just_missed =
                (it->flags & FULL_FRAME) && esp_timer_get_time() - display_last_vsync_us() < 4000;
            if (vsync_ && !just_missed) {
                const int64_t w0 = esp_timer_get_time();
                if (!display_wait_vsync(40)) stats_.vsync_misses++;
                stats_.last_vsync_wait_us = uint32_t(esp_timer_get_time() - w0);
            } else {
                stats_.last_vsync_wait_us = 0;
            }
            frame_t0 = esp_timer_get_time();
            xSemaphoreGive(static_cast<SemaphoreHandle_t>(idle_sem_));   // let the game queue the next one
        }
        if (it->flags & STREAM) display_stream(it->px, it->w * it->h, it->flags & FRAME_START);
        else display_write(it->x, it->y, it->w, it->h, it->px);
        xQueueSend(FREE_Q, &it, portMAX_DELAY);
    }
}

}  // namespace wc
