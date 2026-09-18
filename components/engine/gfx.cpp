#include "engine/gfx.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"

namespace wc {

extern const uint8_t kFont5x7[][5];

static const char *TAG = "gfx";

bool Gfx::init()
{
    fb_ = static_cast<Color *>(heap_caps_malloc(W * H * sizeof(Color), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!fb_) {
        ESP_LOGE(TAG, "framebuffer alloc failed");
        return false;
    }
    clear(colors::black);
    return true;
}

// ------------------------------------------------------------------ dirty tracking

void Gfx::markDirty(int x, int y, int w, int h)
{
    // Screen bounds, not the clip rect: callers mark areas they wrote directly.
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > W) w = W - x;
    if (y + h > H) h = H - y;
    if (w <= 0 || h <= 0) return;
    int b0 = y / BAND_H, b1 = (y + h - 1) / BAND_H;
    for (int b = b0; b <= b1; b++) {
        Band &d = bands_[b];
        if (d.x0 >= d.x1) {
            d.x0 = int16_t(x);
            d.x1 = int16_t(x + w);
        } else {
            d.x0 = int16_t(std::min<int>(d.x0, x));
            d.x1 = int16_t(std::max<int>(d.x1, x + w));
        }
    }
}

void Gfx::markAllDirty()
{
    for (auto &b : bands_) b = {0, W};
}

void Gfx::clearDirty()
{
    for (auto &b : bands_) b = {0, 0};
}

void Gfx::visibleSpan(int band, int &x0, int &x1)
{
    static int16_t lut[BANDS][2];
    static bool built = false;
    if (!built) {
        for (int b = 0; b < BANDS; b++) {
            int y0 = b * BAND_H, y1 = std::min(H, y0 + BAND_H) - 1;
            // Row in this band closest to the center is the widest chord.
            double cy = (H - 1) / 2.0;
            double dy = (y0 > cy) ? (y0 - cy) : (y1 < cy) ? (cy - y1) : 0.0;
            double r = W / 2.0;
            double half = dy >= r ? 0 : std::sqrt(r * r - dy * dy);
            int a = int(std::floor((W - 1) / 2.0 - half)) - 1;
            int z = int(std::ceil((W - 1) / 2.0 + half)) + 2;
            a = std::max(0, a) & ~1;
            z = std::min(W, (z + 1) & ~1);
            lut[b][0] = int16_t(a);
            lut[b][1] = int16_t(z);
        }
        built = true;
    }
    x0 = lut[band][0];
    x1 = lut[band][1];
}

bool Gfx::clip(int &x, int &y, int &w, int &h) const
{
    if (x < cx0_) { w -= cx0_ - x; x = cx0_; }
    if (y < cy0_) { h -= cy0_ - y; y = cy0_; }
    if (x + w > cx1_) w = cx1_ - x;
    if (y + h > cy1_) h = cy1_ - y;
    return w > 0 && h > 0;
}

// ------------------------------------------------------------------ primitives

void Gfx::clear(Color c)
{
    fillRect(0, 0, W, H, c);
}

void Gfx::pixel(int x, int y, Color c)
{
    if (x < cx0_ || x >= cx1_ || y < cy0_ || y >= cy1_) return;
    fb_[y * W + x] = c;
    markDirty(x, y, 1, 1);
}

void Gfx::fillRect(int x, int y, int w, int h, Color c)
{
    if (!clip(x, y, w, h)) return;
    Color *row = fb_ + y * W + x;
    if (c == 0) {
        for (int j = 0; j < h; j++, row += W) std::memset(row, 0, w * sizeof(Color));
    } else {
        std::fill_n(row, w, c);
        const Color *first = row;
        row += W;
        for (int j = 1; j < h; j++, row += W) std::memcpy(row, first, w * sizeof(Color));
    }
    markDirty(x, y, w, h);
}

void Gfx::rect(int x, int y, int w, int h, Color c)
{
    hline(x, y, w, c);
    hline(x, y + h - 1, w, c);
    vline(x, y, h, c);
    vline(x + w - 1, y, h, c);
}

void Gfx::line(int x0, int y0, int x1, int y1, Color c)
{
    if (y0 == y1) { if (x1 < x0) std::swap(x0, x1); hline(x0, y0, x1 - x0 + 1, c); return; }
    if (x0 == x1) { if (y1 < y0) std::swap(y0, y1); vline(x0, y0, y1 - y0 + 1, c); return; }
    int dx = std::abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -std::abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    markDirty(std::min(x0, x1), std::min(y0, y1), dx + 1, -dy + 1);
    for (;;) {
        if (x0 >= cx0_ && x0 < cx1_ && y0 >= cy0_ && y0 < cy1_) fb_[y0 * W + x0] = c;
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void Gfx::fillCircle(int cx, int cy, int r, Color c)
{
    for (int dy = -r; dy <= r; dy++) {
        int half = int(std::sqrt(float(r * r - dy * dy)) + 0.5f);
        fillRect(cx - half, cy + dy, 2 * half + 1, 1, c);
    }
}

void Gfx::circle(int cx, int cy, int r, Color c)
{
    int x = r, y = 0, err = 1 - r;
    while (x >= y) {
        pixel(cx + x, cy + y, c); pixel(cx + y, cy + x, c);
        pixel(cx - y, cy + x, c); pixel(cx - x, cy + y, c);
        pixel(cx - x, cy - y, c); pixel(cx - y, cy - x, c);
        pixel(cx + y, cy - x, c); pixel(cx + x, cy - y, c);
        y++;
        if (err < 0) err += 2 * y + 1;
        else { x--; err += 2 * (y - x) + 1; }
    }
}

void Gfx::blit(const Color *src, int w, int h, int x, int y, bool use_key, Color key)
{
    int sx = 0, sy = 0, cw = w, ch = h;
    if (x < cx0_) { sx = cx0_ - x; cw -= sx; x = cx0_; }
    if (y < cy0_) { sy = cy0_ - y; ch -= sy; y = cy0_; }
    if (x + cw > cx1_) cw = cx1_ - x;
    if (y + ch > cy1_) ch = cy1_ - y;
    if (cw <= 0 || ch <= 0) return;

    for (int j = 0; j < ch; j++) {
        const Color *s = src + (sy + j) * w + sx;
        Color *d = fb_ + (y + j) * W + x;
        if (!use_key) {
            std::memcpy(d, s, cw * sizeof(Color));
        } else {
            for (int i = 0; i < cw; i++)
                if (s[i] != key) d[i] = s[i];
        }
    }
    markDirty(x, y, cw, ch);
}

void Gfx::blitAlpha(const Color *src, const uint8_t *alpha, int w, int h, int x, int y)
{
    int sx = 0, sy = 0, cw = w, ch = h;
    if (x < cx0_) { sx = cx0_ - x; cw -= sx; x = cx0_; }
    if (y < cy0_) { sy = cy0_ - y; ch -= sy; y = cy0_; }
    if (x + cw > cx1_) cw = cx1_ - x;
    if (y + ch > cy1_) ch = cy1_ - y;
    if (cw <= 0 || ch <= 0) return;

    for (int j = 0; j < ch; j++) {
        const Color *s = src + (sy + j) * w + sx;
        const uint8_t *a = alpha + (sy + j) * w + sx;
        Color *d = fb_ + (y + j) * W + x;
        for (int i = 0; i < cw; i++) {
            const unsigned k = a[i];
            if (k == 0) continue;
            if (k == 255) {
                d[i] = s[i];
                continue;
            }
            // Colors are byte-swapped RGB565: unswap, blend per channel, swap back.
            const unsigned fs = uint16_t(s[i] >> 8 | s[i] << 8), bs = uint16_t(d[i] >> 8 | d[i] << 8);
            const unsigned r = (((fs >> 11) & 31) * k + ((bs >> 11) & 31) * (255 - k)) / 255;
            const unsigned g = (((fs >> 5) & 63) * k + ((bs >> 5) & 63) * (255 - k)) / 255;
            const unsigned b = ((fs & 31) * k + (bs & 31) * (255 - k)) / 255;
            const uint16_t o = uint16_t(r << 11 | g << 5 | b);
            d[i] = uint16_t(o >> 8 | o << 8);
        }
    }
    markDirty(x, y, cw, ch);
}

// ------------------------------------------------------------------ text

static inline int boldPx(int scale) { return scale / 2 > 1 ? scale / 2 : 1; }

int Gfx::text(int x, int y, const char *s, Color fg, int scale, bool bold)
{
    const int extra = bold ? boldPx(scale) : 0;
    for (; *s; s++) {
        unsigned ch = static_cast<unsigned char>(*s);
        if (ch < 32 || ch > 126) ch = '?';
        const uint8_t *g = kFont5x7[ch - 32];
        // One fillRect per horizontal run of lit pixels in each glyph row.
        for (int row = 0; row < 7; row++) {
            for (int col = 0; col < 5;) {
                if (!(g[col] & (1 << row))) { col++; continue; }
                int end = col;
                while (end < 5 && (g[end] & (1 << row))) end++;
                fillRect(x + col * scale, y + row * scale, (end - col) * scale + extra, scale, fg);
                col = end;
            }
        }
        x += 6 * scale + extra;
    }
    return x;
}

void Gfx::textCentered(int cx, int cy, const char *s, Color fg, int scale, bool bold, int *bx, int *by, int *bw,
                       int *bh)
{
    const int w = textWidth(s, scale, bold), h = textHeight(scale);
    const int x = cx - w / 2, y = cy - h / 2;
    text(x, y, s, fg, scale, bold);
    if (bx) *bx = x;
    if (by) *by = y;
    if (bw) *bw = w;
    if (bh) *bh = h;
}

int Gfx::textf(int x, int y, Color fg, int scale, const char *fmt, ...)
{
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return text(x, y, buf, fg, scale);
}

int Gfx::textWidth(const char *s, int scale, bool bold)
{
    int n = std::strlen(s);
    const int extra = bold ? boldPx(scale) : 0;
    return n ? (n * 6 - 1) * scale + n * extra : 0;
}

}  // namespace wc
