// Pixel canvas: a small 8-bit palette picture that the presenter scales up by a
// whole number straight into the display's transfer buffers.
//
// For pixel-art games this beats drawing on the 466x466 framebuffer: at 3x the
// canvas is 155x155 (24 KB), so a full redraw is cheap, every sprite pixel is a
// crisp 3x3 block, and the frame never touches the big PSRAM framebuffer at all.
// Palette entries can change every frame (sky gradients, tinted clouds) for free,
// because the lookup happens while the frame is being sent.
//
// Sprite sheets are ordinary PNGs (frames side by side); colours are mapped into
// the palette when the sheet is loaded, alpha < 128 is transparent.
#pragma once

#include <cstddef>
#include <cstdint>

#include "engine/gfx.h"

namespace wc {

class Presenter;

struct Sheet {
    int w = 0, h = 0;      // whole sheet, in pixels
    int fw = 0, fh = 0;    // one frame
    uint8_t *px = nullptr; // palette indices, 0 = transparent
    int frames() const { return fw ? (w / fw) * (h / fh) : 0; }
    int cols() const { return fw ? w / fw : 0; }
    bool valid() const { return px != nullptr; }
    void release();
};

class Canvas {
public:
    static constexpr int MAX_W = Gfx::W / 2 + 1;   // at 2x the canvas is 233 wide

    Canvas() = default;
    ~Canvas();
    // A canvas owns its pixels, so copying one would double-free them. Games hold
    // canvases by value and never copy them; say so rather than hope.
    Canvas(const Canvas &) = delete;
    Canvas &operator=(const Canvas &) = delete;

    // `scale` is 2 or 3. By default the canvas covers the screen exactly
    // (ceil(466 / scale) square); a bigger `size` is centred on the screen with
    // the edges cut off, which gives a rotated canvas (presentRotated) something
    // to show in the corners.
    bool init(int scale, int size = 0);
    int width() const { return w_; }
    int height() const { return h_; }
    int scale() const { return scale_; }
    uint8_t *pixels() { return px_; }

    // ---- palette
    // Index of `c`, allocating a new entry the first time. Index 0 is reserved for
    // "transparent" in sprites and draws as black.
    uint8_t color(Color c);
    // Reserve `n` entries the game will change itself (gradients, tints).
    uint8_t reserve(int n);
    void setColor(uint8_t i, Color c) { pal_[i] = c; }
    // Install a whole palette (index 0 stays black); later color() calls append.
    void setPalette(const Color *pal, int n);
    Color getColor(uint8_t i) const { return pal_[i]; }
    int used() const { return used_; }

    // ---- drawing, in canvas pixels, clipped to the canvas
    void clear(uint8_t c);
    void pixel(int x, int y, uint8_t c);
    void fillRect(int x, int y, int w, int h, uint8_t c);
    void rect(int x, int y, int w, int h, uint8_t c);
    void hline(int x, int y, int w, uint8_t c) { fillRect(x, y, w, 1, c); }
    void vline(int x, int y, int h, uint8_t c) { fillRect(x, y, 1, h, c); }
    void line(int x0, int y0, int x1, int y1, uint8_t c);
    void fillCircle(int cx, int cy, int r, uint8_t c);
    // 5x7 font, like Gfx::text. Returns the x after the last glyph.
    int text(int x, int y, const char *s, uint8_t c, int scale = 1, bool bold = false);
    void textCentered(int cx, int cy, const char *s, uint8_t c, int scale = 1, bool bold = false);

    // ---- sprites
    // Load a PNG (from flash or a file already read into memory) as a sheet of
    // fw x fh frames. Colours join the palette; returns false if it isn't a PNG or
    // memory runs out.
    bool loadSheet(Sheet &out, const uint8_t *png, size_t len, int fw, int fh);
    // Draw a frame with its top-left at (x, y).
    void sprite(const Sheet &s, int frame, int x, int y, bool flip_x = false);
    // Draw a frame scaled by (sx, sy), bottom-centre at (x, y). Nearest-neighbour,
    // so 1.3 x 0.7 is a squash and 0.8 x 1.2 a stretch.
    void spriteScaled(const Sheet &s, int frame, float x, float y, float sx, float sy, bool flip_x = false);

    // ---- output: scale up and send the whole screen
    void present(Presenter &p);
    // Same, but rotated by `angle` (radians, clockwise) about the centre. Samples
    // once per two screen pixels, which is invisible at 2x and halves the reads.
    void presentRotated(Presenter &p, float angle);

private:
    int scale_ = 3, w_ = 0, h_ = 0;
    uint8_t *px_ = nullptr;
    Color pal_[256] = {};
    int used_ = 1;
};

}  // namespace wc
