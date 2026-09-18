// Frame pipeline: the game core (1) copies dirty regions out of the PSRAM
// framebuffer into a small pool of DMA buffers and queues them; a display task
// on core 0 waits for the panel's vblank and streams them over QSPI. The game
// starts its next frame while the previous one is still on the wire.
#pragma once

#include <cstdint>
#include <functional>
#include "engine/gfx.h"

namespace wc {

struct PresentStats {
    uint32_t frames = 0;
    uint32_t last_bytes = 0;       // pixel bytes queued by the last present()
    uint32_t last_wait_us = 0;     // present() blocked waiting for the previous frame to go out
    uint32_t last_copy_us = 0;     // copying dirty pixels into DMA buffers
    uint32_t last_xfer_us = 0;     // wire time of the most recently completed frame
    uint32_t last_vsync_wait_us = 0;
    uint32_t vsync_misses = 0;     // TE never arrived within timeout
};

class Presenter {
public:
    // Each transfer to the panel has a fixed ~0.4 ms cost, and anything over 32 KB is
    // split in two by the SPI driver, so bands sit just under that: 34 full-width rows.
    bool init(int pool_buffers = 2, int buffer_rows = 34);
    void present(Gfx &gfx);
    // For games that repaint every pixel every frame: skip the framebuffer and
    // generate the picture straight into the display's transfer buffers, band by
    // band. `fill(y, rows, x0, w, dst)` must write w*rows pixels (row-major) for the
    // screen rectangle starting at (x0, y). Filling and sending overlap across cores.
    using BandFill = std::function<void(int y, int rows, int x0, int w, Color *dst)>;
    void presentBands(const BandFill &fill);
    void setVsync(bool on) { vsync_ = on; }
    bool vsync() const { return vsync_; }
    const PresentStats &stats() const { return stats_; }
    // Block until everything queued has hit the panel.
    void flush();
    // Debug: copy the next presentBands() frame into `gfx` as well, so a screenshot
    // of a game that bypasses the framebuffer can be taken from it.
    void snapshotInto(Gfx *gfx) { snap_ = gfx; }

private:
    static void taskEntry(void *arg);
    void taskLoop();
    void emitRect(Gfx &gfx, int x0, int x1, int y0, int y1, bool &first);

    PresentStats stats_;
    bool vsync_ = true;
    bool streamed_ = false;   // presentBands() ran this frame
    Gfx *snap_ = nullptr;
    int buf_pixels_ = 0;
    void *free_q_ = nullptr;   // QueueHandle_t of Item*
    void *work_q_ = nullptr;   // QueueHandle_t of Item*
    void *idle_sem_ = nullptr;
};

}  // namespace wc
