// Software renderer: RGB565 framebuffer with dirty-band tracking.
//
// Every draw call marks the touched area dirty. present() only ships dirty
// pixels, clipped to the round visible area, so a game that moves a few sprites
// over a static background pushes a tiny fraction of the 434 KB frame.
#pragma once

#include <cstdint>

namespace wc {

// Colors are stored byte-swapped so the framebuffer can go to the panel as-is.
using Color = uint16_t;

constexpr Color rgb(uint8_t r, uint8_t g, uint8_t b)
{
    uint16_t c = uint16_t(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
    return uint16_t((c >> 8) | (c << 8));
}

namespace colors {
constexpr Color black = rgb(0, 0, 0);
constexpr Color white = rgb(255, 255, 255);
constexpr Color red = rgb(255, 40, 40);
constexpr Color green = rgb(40, 230, 90);
constexpr Color blue = rgb(40, 110, 255);
constexpr Color yellow = rgb(255, 220, 40);
constexpr Color cyan = rgb(40, 230, 230);
constexpr Color magenta = rgb(240, 60, 220);
constexpr Color orange = rgb(255, 140, 20);
constexpr Color gray = rgb(110, 110, 110);
constexpr Color dark = rgb(28, 28, 32);
}  // namespace colors

class Gfx {
public:
    static constexpr int W = 466;
    static constexpr int H = 466;
    static constexpr int CX = W / 2;   // center of the round panel
    static constexpr int CY = H / 2;
    static constexpr int R = W / 2;
    static constexpr int BAND_H = 16;
    static constexpr int BANDS = (H + BAND_H - 1) / BAND_H;

    bool init();

    // ---- drawing
    void clear(Color c);
    void pixel(int x, int y, Color c);
    void fillRect(int x, int y, int w, int h, Color c);
    void rect(int x, int y, int w, int h, Color c);
    void hline(int x, int y, int w, Color c) { fillRect(x, y, w, 1, c); }
    void vline(int x, int y, int h, Color c) { fillRect(x, y, 1, h, c); }
    void line(int x0, int y0, int x1, int y1, Color c);
    void fillCircle(int cx, int cy, int r, Color c);
    void circle(int cx, int cy, int r, Color c);
    // Blit a w*h RGB565 (byte-swapped) image. Pixels equal to `key` are skipped
    // when `use_key` is set.
    void blit(const Color *src, int w, int h, int x, int y, bool use_key = false, Color key = 0);
    // Blit with 8-bit alpha (0 = skip, 255 = copy, in between = blend with what's there).
    void blitAlpha(const Color *src, const uint8_t *alpha, int w, int h, int x, int y);
    // 5x7 bitmap font, `scale` >= 1. `bold` thickens strokes horizontally by
    // max(1, scale/2) px. Returns the x after the last glyph.
    int text(int x, int y, const char *s, Color fg, int scale = 1, bool bold = false);
    int textf(int x, int y, Color fg, int scale, const char *fmt, ...);
    static int textWidth(const char *s, int scale = 1, bool bold = false);
    static int textHeight(int scale = 1) { return 7 * scale; }
    // Centered on (cx, cy); returns the bounding box via out params if non-null.
    void textCentered(int cx, int cy, const char *s, Color fg, int scale, bool bold, int *bx = nullptr,
                      int *by = nullptr, int *bw = nullptr, int *bh = nullptr);

    // ---- clipping: every draw call is limited to this rectangle (default: whole screen)
    void setClip(int x, int y, int w, int h)
    {
        cx0_ = x < 0 ? 0 : x;
        cy0_ = y < 0 ? 0 : y;
        cx1_ = x + w > W ? W : x + w;
        cy1_ = y + h > H ? H : y + h;
    }
    void clearClip() { cx0_ = 0, cy0_ = 0, cx1_ = W, cy1_ = H; }

    // ---- raw access
    Color *pixels() { return fb_; }
    void markDirty(int x, int y, int w, int h);
    void markAllDirty();

    // ---- used by the presenter
    struct Band { int16_t x0, x1; };           // dirty span, x1 exclusive; x0 >= x1 means clean
    const Band &band(int i) const { return bands_[i]; }
    void clearDirty();
    // Visible (inside-circle) span for a band, even-aligned.
    static void visibleSpan(int band, int &x0, int &x1);

private:
    bool clip(int &x, int &y, int &w, int &h) const;

    Color *fb_ = nullptr;
    int cx0_ = 0, cy0_ = 0, cx1_ = W, cy1_ = H;
    Band bands_[BANDS];
};

}  // namespace wc
