#include "bench_game.h"

#include "esp_system.h"

#include <cmath>

#include "board/board.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

using namespace wc;

static const char *TAG = "bench";
static constexpr int BALL_R = 22;
static constexpr int DOT_R = 20;
static constexpr int ARENA_R = Gfx::R - 34;   // ball stays inside this

void BenchGame::begin(Engine &e)
{
    ESP_LOGI(TAG, "bench start: BOOT=vsync toggle, PWR=stress toggle");
}

void BenchGame::update(Engine &e, float dt)
{
    const auto &in = e.input();

    // Holding either button leaves the bench (restarts into the console).
    if (in.long_press & (BTN_A | BTN_B)) esp_restart();
    if (in.pressed & BTN_A) e.presenter().setVsync(!e.presenter().vsync());
    if (in.pressed & BTN_B) {
        stress_ = !stress_;
        if (!stress_) need_static_ = true;
    }

    // Tilt -> acceleration (px/s^2), light damping, bounce off the round arena.
    const float k = 2200.0f;
    bvx_ += in.tilt.ax * k * dt;
    bvy_ += in.tilt.ay * k * dt;
    bvx_ *= 1.0f - 0.6f * dt;
    bvy_ *= 1.0f - 0.6f * dt;
    bx_ += bvx_ * dt;
    by_ += bvy_ * dt;

    float dx = bx_ - Gfx::CX, dy = by_ - Gfx::CY;
    float d = std::sqrt(dx * dx + dy * dy);
    const float lim = ARENA_R - BALL_R;
    if (d > lim) {
        float nx = dx / d, ny = dy / d;
        bx_ = Gfx::CX + nx * lim;
        by_ = Gfx::CY + ny * lim;
        float vn = bvx_ * nx + bvy_ * ny;
        if (vn > 0) {
            bvx_ -= 1.7f * vn * nx;
            bvy_ -= 1.7f * vn * ny;
        }
    }

    const int64_t now = e.nowUs();
    if (now - log_t_ > 1000000) {
        log_t_ = now;
        const auto &ps = e.presenter().stats();
        ESP_LOGI(TAG,
                 "fps %.1f frame %.1fms (upd %.2f draw %.2f) slot %.1fms copy %.1fms wire %.1fms vwait %.1fms "
                 "%uKB vsync=%d miss=%u | touch %d,%d %s %uHz | tilt %.2f %.2f %.2f | btn 0x%x pwr_raw=%d | "
                 "int %uKB psram %uKB",
                 e.stats().fps, e.stats().frame_us / 1000.0f, e.stats().update_us / 1000.0f,
                 e.stats().draw_us / 1000.0f, ps.last_wait_us / 1000.0f, ps.last_copy_us / 1000.0f, ps.last_xfer_us / 1000.0f,
                 ps.last_vsync_wait_us / 1000.0f, unsigned(ps.last_bytes / 1024), e.presenter().vsync(),
                 unsigned(ps.vsync_misses), in.touch.x, in.touch.y, in.touch.down ? "DOWN" : "up",
                 unsigned(in.touch_hz), in.tilt.ax, in.tilt.ay, in.tilt.az, unsigned(in.held),
                 gpio_get_level(PIN_KEY_PWR),
                 unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                 unsigned(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    }
}

void BenchGame::drawStatic(Gfx &g)
{
    g.clear(colors::black);
    g.circle(Gfx::CX, Gfx::CY, ARENA_R, colors::gray);
    g.circle(Gfx::CX, Gfx::CY, ARENA_R + 1, colors::gray);
    const char *title = "TILT-A-TRON";
    g.text(Gfx::CX - Gfx::textWidth(title, 3) / 2, 40, title, colors::yellow, 3);
    drawn_bx_ = drawn_by_ = drawn_tx_ = drawn_ty_ = -1000;
    hud_t_ = 0;
}

void BenchGame::drawHud(Engine &e, Gfx &g)
{
    const auto &ps = e.presenter().stats();
    const auto &in = e.input();
    const int x = 118, y = 150, lh = 20;
    g.fillRect(x, y, 230, lh * 5, colors::black);
    g.textf(x, y, colors::white, 2, "FPS %4.1f", e.stats().fps);
    g.textf(x, y + lh, colors::white, 2, "wire %4.1fms", ps.last_xfer_us / 1000.0f);
    g.textf(x, y + lh * 2, e.presenter().vsync() ? colors::green : colors::orange, 2, "vsync %s",
            e.presenter().vsync() ? "ON" : "OFF");
    g.textf(x, y + lh * 3, colors::cyan, 2, "touch %uHz", unsigned(in.touch_hz));
    g.textf(x, y + lh * 4, colors::gray, 2, "A%c B%c", (in.held & BTN_A) ? '*' : '-',
            (in.held & BTN_B) ? '*' : '-');
}

// Edge calibration: everything here touches the true pixel limits of the panel.
//  - white circle at r=232 (outermost ring that fits 466 px), red at r=230, green at r=226
//  - yellow 1-px lines on column 0 / 465 and row 0 / 465 through the center
//  - magenta tick marks every 10 px inward from each edge along the axes
static void drawCalibration(Gfx &g)
{
    g.clear(colors::black);
    const int c = Gfx::W / 2;   // 233; pixel centers span 0..465, true center is 232.5
    g.circle(c, c, 232, colors::white);
    g.circle(c - 1, c - 1, 232, colors::white);   // pair covers the half-pixel center
    g.circle(c, c, 229, colors::red);
    g.circle(c, c, 225, colors::green);
    g.vline(0, c - 30, 60, colors::yellow);
    g.vline(Gfx::W - 1, c - 30, 60, colors::yellow);
    g.hline(c - 30, 0, 60, colors::yellow);
    g.hline(c - 30, Gfx::H - 1, 60, colors::yellow);
    for (int i = 0; i <= 40; i += 10) {
        g.hline(i, c - 50, 1, colors::magenta), g.vline(i, c - 50, 12, colors::magenta);
        g.vline(Gfx::W - 1 - i, c + 40, 12, colors::magenta);
        g.hline(c - 50, i, 12, colors::magenta);
        g.hline(c + 40, Gfx::H - 1 - i, 12, colors::magenta);
    }
    g.hline(c - 15, c, 31, colors::white);
    g.vline(c, c - 15, 31, colors::white);
    const char *t = "CALIBRATION";
    g.text(c - Gfx::textWidth(t, 2) / 2, c + 40, t, colors::white, 2);
}

void BenchGame::draw(Engine &e, Gfx &g)
{
    if (e.nowUs() < 8000000 + calib_start_us_) {
        if (!calib_drawn_) {
            calib_start_us_ = e.nowUs();
            drawCalibration(g);
            calib_drawn_ = true;
        }
        return;
    }
    if (stress_) {
        // Worst case: every visible pixel changes every frame.
        stress_hue_ += 9;
        uint8_t r = (std::sin(stress_hue_ * 0.010f) * 0.5f + 0.5f) * 255;
        uint8_t gg = (std::sin(stress_hue_ * 0.013f + 2) * 0.5f + 0.5f) * 255;
        uint8_t b = (std::sin(stress_hue_ * 0.017f + 4) * 0.5f + 0.5f) * 255;
        g.clear(rgb(r, gg, b));
        g.textf(Gfx::CX - 90, Gfx::CY - 10, colors::black, 3, "%4.1f FPS", e.stats().fps);
        return;
    }
    if (need_static_) {
        drawStatic(g);
        need_static_ = false;
    }

    // Ball: erase old, draw new
    const int bx = int(bx_), by = int(by_);
    if (bx != drawn_bx_ || by != drawn_by_) {
        g.fillCircle(drawn_bx_, drawn_by_, BALL_R, colors::black);
        g.fillCircle(bx, by, BALL_R, colors::orange);
        g.fillCircle(bx - 7, by - 7, 6, colors::yellow);
        drawn_bx_ = bx;
        drawn_by_ = by;
    }

    // Touch dot
    const auto &t = e.input().touch;
    const int tx = t.down ? t.x : -1000, ty = t.down ? t.y : -1000;
    if (tx != drawn_tx_ || ty != drawn_ty_) {
        g.fillCircle(drawn_tx_, drawn_ty_, DOT_R, colors::black);
        if (t.down) g.fillCircle(tx, ty, DOT_R, colors::cyan);
        drawn_tx_ = tx;
        drawn_ty_ = ty;
    }

    const int64_t now = e.nowUs();
    if (now - hud_t_ > 250000) {
        hud_t_ = now;
        drawHud(e, g);
    }
}
