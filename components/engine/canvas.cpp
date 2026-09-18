#include "engine/canvas.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "engine/presenter.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lodepng.h"

extern const uint8_t kFont5x7[][5];

namespace wc {

static const char *TAG = "canvas";

void Sheet::release()
{
    heap_caps_free(px);
    px = nullptr;
    w = h = fw = fh = 0;
}

bool Canvas::init(int scale)
{
    scale_ = std::max(2, std::min(3, scale));
    w_ = h_ = (Gfx::W + scale_ - 1) / scale_;
    // Internal RAM is scarce (Wi-Fi needs it); the canvas is small enough that
    // reading it from PSRAM is cheap because the scaler walks it in order.
    px_ = static_cast<uint8_t *>(heap_caps_malloc(w_ * h_, MALLOC_CAP_SPIRAM));
    if (!px_) {
        ESP_LOGE(TAG, "no memory");
        return false;
    }
    std::memset(px_, 0, w_ * h_);
    pal_[0] = colors::black;
    used_ = 1;
    ESP_LOGI(TAG, "%dx%d at %dx", w_, h_, scale_);
    return true;
}

// ------------------------------------------------------------------ palette
uint8_t Canvas::color(Color c)
{
    for (int i = 1; i < used_; i++)
        if (pal_[i] == c) return uint8_t(i);
    if (used_ < 256) {
        pal_[used_] = c;
        return uint8_t(used_++);
    }
    // Full: nearest entry. (Colours are byte-swapped RGB565; unswap to compare.)
    const uint16_t cc = uint16_t((c >> 8) | (c << 8));
    const int r = cc >> 11, g = (cc >> 5) & 63, b = cc & 31;
    int best = 1, best_d = 1 << 30;
    for (int i = 1; i < 256; i++) {
        const uint16_t p = uint16_t((pal_[i] >> 8) | (pal_[i] << 8));
        const int dr = (p >> 11) - r, dg = ((p >> 5) & 63) - g, db = (p & 31) - b;
        const int d = dr * dr * 2 + dg * dg + db * db * 2;
        if (d < best_d) best_d = d, best = i;
    }
    return uint8_t(best);
}

uint8_t Canvas::reserve(int n)
{
    if (used_ + n > 256) {
        ESP_LOGW(TAG, "palette full (%d + %d)", used_, n);
        return 0;
    }
    const uint8_t first = uint8_t(used_);
    used_ += n;
    return first;
}

// ------------------------------------------------------------------ drawing
void Canvas::clear(uint8_t c) { std::memset(px_, c, w_ * h_); }

void Canvas::pixel(int x, int y, uint8_t c)
{
    if (x < 0 || y < 0 || x >= w_ || y >= h_) return;
    px_[y * w_ + x] = c;
}

void Canvas::fillRect(int x, int y, int w, int h, uint8_t c)
{
    if (x < 0) w += x, x = 0;
    if (y < 0) h += y, y = 0;
    if (x + w > w_) w = w_ - x;
    if (y + h > h_) h = h_ - y;
    if (w <= 0 || h <= 0) return;
    uint8_t *p = px_ + y * w_ + x;
    for (int r = 0; r < h; r++, p += w_) std::memset(p, c, w);
}

void Canvas::rect(int x, int y, int w, int h, uint8_t c)
{
    fillRect(x, y, w, 1, c);
    fillRect(x, y + h - 1, w, 1, c);
    fillRect(x, y, 1, h, c);
    fillRect(x + w - 1, y, 1, h, c);
}

void Canvas::line(int x0, int y0, int x1, int y1, uint8_t c)
{
    const int dx = std::abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    const int dy = -std::abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        pixel(x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        const int e2 = 2 * err;
        if (e2 >= dy) err += dy, x0 += sx;
        if (e2 <= dx) err += dx, y0 += sy;
    }
}

void Canvas::fillCircle(int cx, int cy, int r, uint8_t c)
{
    if (r < 0) return;
    for (int dy = -r; dy <= r; dy++) {
        const int half = int(std::sqrt(float(r * r - dy * dy)) + 0.5f);
        fillRect(cx - half, cy + dy, 2 * half + 1, 1, c);
    }
}

static inline int boldPx(int scale) { return scale / 2 > 1 ? scale / 2 : 1; }

int Canvas::text(int x, int y, const char *s, uint8_t c, int scale, bool bold)
{
    const int extra = bold ? boldPx(scale) : 0;
    for (; *s; s++) {
        unsigned ch = static_cast<unsigned char>(*s);
        if (ch < 32 || ch > 126) ch = '?';
        const uint8_t *g = kFont5x7[ch - 32];
        for (int row = 0; row < 7; row++) {
            for (int col = 0; col < 5;) {
                if (!(g[col] & (1 << row))) { col++; continue; }
                int end = col;
                while (end < 5 && (g[end] & (1 << row))) end++;
                fillRect(x + col * scale, y + row * scale, (end - col) * scale + extra, scale, c);
                col = end;
            }
        }
        x += 6 * scale + extra;
    }
    return x;
}

void Canvas::textCentered(int cx, int cy, const char *s, uint8_t c, int scale, bool bold)
{
    const int w = Gfx::textWidth(s, scale, bold), h = Gfx::textHeight(scale);
    text(cx - w / 2, cy - h / 2, s, c, scale, bold);
}

// ------------------------------------------------------------------ sprites
bool Canvas::loadSheet(Sheet &out, const uint8_t *png, size_t len, int fw, int fh)
{
    out.release();
    unsigned char *rgba = nullptr;
    unsigned w = 0, h = 0;
    const unsigned err = lodepng_decode32(&rgba, &w, &h, png, len);
    if (err) {
        ESP_LOGW(TAG, "sheet: %s", lodepng_error_text(err));
        free(rgba);
        return false;
    }
    out.px = static_cast<uint8_t *>(heap_caps_malloc(w * h, MALLOC_CAP_SPIRAM));
    if (!out.px) {
        free(rgba);
        return false;
    }
    out.w = int(w);
    out.h = int(h);
    out.fw = fw > 0 ? fw : int(w);
    out.fh = fh > 0 ? fh : int(h);
    // Colours repeat a lot, so remember the last one: most pixels hit it.
    uint32_t last_rgba = 0xFFFFFFFF;
    uint8_t last_idx = 0;
    for (unsigned i = 0; i < w * h; i++) {
        const unsigned char *p = rgba + i * 4;
        uint32_t key;
        std::memcpy(&key, p, 4);
        if (key != last_rgba) {
            last_rgba = key;
            last_idx = p[3] < 128 ? 0 : color(rgb(p[0], p[1], p[2]));
        }
        out.px[i] = last_idx;
    }
    free(rgba);
    ESP_LOGI(TAG, "sheet %ux%u, %d frames of %dx%d, palette now %d", w, h, out.frames(), out.fw, out.fh, used_);
    return true;
}

void Canvas::sprite(const Sheet &s, int frame, int x, int y, bool flip_x)
{
    if (!s.valid() || s.frames() == 0) return;
    frame %= s.frames();
    const int fx = (frame % s.cols()) * s.fw, fy = (frame / s.cols()) * s.fh;
    for (int r = 0; r < s.fh; r++) {
        const int dy = y + r;
        if (dy < 0 || dy >= h_) continue;
        const uint8_t *src = s.px + (fy + r) * s.w + fx;
        uint8_t *dst = px_ + dy * w_;
        for (int c = 0; c < s.fw; c++) {
            const int dx = x + (flip_x ? s.fw - 1 - c : c);
            if (dx < 0 || dx >= w_) continue;
            const uint8_t v = src[c];
            if (v) dst[dx] = v;
        }
    }
}

void Canvas::spriteScaled(const Sheet &s, int frame, float x, float y, float sx, float sy, bool flip_x)
{
    if (!s.valid() || s.frames() == 0) return;
    if (std::fabs(sx - 1) < 0.02f && std::fabs(sy - 1) < 0.02f) {
        sprite(s, frame, int(std::floor(x - s.fw / 2.0f + 0.5f)), int(std::floor(y - s.fh + 0.5f)), flip_x);
        return;
    }
    frame %= s.frames();
    const int fx = (frame % s.cols()) * s.fw, fy = (frame / s.cols()) * s.fh;
    const int dw = std::max(1, int(s.fw * sx + 0.5f)), dh = std::max(1, int(s.fh * sy + 0.5f));
    const int x0 = int(std::floor(x - dw / 2.0f + 0.5f)), y0 = int(std::floor(y - dh + 0.5f));
    for (int r = 0; r < dh; r++) {
        const int dy = y0 + r;
        if (dy < 0 || dy >= h_) continue;
        const int sr = std::min(s.fh - 1, r * s.fh / dh);
        const uint8_t *src = s.px + (fy + sr) * s.w + fx;
        uint8_t *dst = px_ + dy * w_;
        for (int c = 0; c < dw; c++) {
            const int dx = x0 + c;
            if (dx < 0 || dx >= w_) continue;
            int sc = std::min(s.fw - 1, c * s.fw / dw);
            if (flip_x) sc = s.fw - 1 - sc;
            const uint8_t v = src[sc];
            if (v) dst[dx] = v;
        }
    }
}

// ------------------------------------------------------------------ output
void Canvas::present(Presenter &p)
{
    p.presentBands([this](int y, int rows, int x0, int w, Color *dst) {
        Color pal[256];   // on the stack: internal RAM, fast
        std::memcpy(pal, pal_, sizeof(pal));
        const int s = scale_;
        for (int r = 0; r < rows; r++) {
            const int sy = (y + r) / s;
            const uint8_t *src = px_ + std::min(sy, h_ - 1) * w_;
            Color *out = dst + r * w;
            // Walk canvas pixels, repeating each one `s` times; the first may be partial.
            int i = x0 / s, k = x0 - i * s;
            int x = 0;
            while (x < w) {
                const Color c = pal[src[i < w_ ? i : w_ - 1]];
                int n = s - k;
                if (n > w - x) n = w - x;
                for (int j = 0; j < n; j++) out[x + j] = c;
                x += n;
                k = 0;
                i++;
            }
        }
    });
}

}  // namespace wc
