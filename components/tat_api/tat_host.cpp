#include "tat/tat_host.h"

#include <cstdarg>
#include <cstring>
#include <vector>

#include "audio/audio.h"
#include "console/banner.h"
#include "console/pause_menu.h"
#include "console/ui.h"
#include "engine/canvas.h"
#include "engine/gestures.h"
#include "engine/polar.h"
#include "engine/store.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"

namespace tat {

namespace {

const char *TAG = "tat";

// Everything the api functions need that they are not passed. A game never sees any of
// this; it exists because the C table has nowhere to carry state.
struct Host {
    wc::Engine *engine = nullptr;
    const tat_game_t *game = nullptr;
    wc::Gestures gestures;
    console::ui::PauseMenu menu;
    tat_input_t input{};
    tat_gestures_t ges{};

    // What the running game has taken, so unloading it gives everything back.
    std::vector<void *> allocations;
    std::vector<wc::Canvas *> canvases;
    std::vector<wc::Sheet *> sheets;
};

Host s;

// ---- the api, function by function

tat_color_t api_rgb(uint8_t r, uint8_t g, uint8_t b) { return wc::rgb(r, g, b); }

void api_go_home()
{
    if (s.engine) s.engine->goHome();
}

void api_log(const char *fmt, ...)
{
    char line[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    ESP_LOGI(s.game ? s.game->id : TAG, "%s", line);
}

int64_t api_now_us() { return esp_timer_get_time(); }
uint32_t api_random() { return esp_random(); }
const tat_input_t *api_input() { return &s.input; }
const tat_gestures_t *api_gestures() { return &s.ges; }

void *api_alloc(size_t bytes)
{
    void *p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    if (p) s.allocations.push_back(p);
    return p;
}

void api_free(void *p)
{
    if (!p) return;
    for (auto it = s.allocations.begin(); it != s.allocations.end(); ++it) {
        if (*it == p) {
            s.allocations.erase(it);
            break;
        }
    }
    heap_caps_free(p);
}

tat_canvas_t *api_canvas_create(int scale, int size)
{
    auto *c = new wc::Canvas();
    if (!c->init(scale, size)) {
        delete c;
        return nullptr;
    }
    s.canvases.push_back(c);
    return reinterpret_cast<tat_canvas_t *>(c);
}

wc::Canvas *cv(tat_canvas_t *c) { return reinterpret_cast<wc::Canvas *>(c); }
wc::Sheet *sh(tat_sheet_t *x) { return reinterpret_cast<wc::Sheet *>(x); }

void api_canvas_destroy(tat_canvas_t *c)
{
    for (auto it = s.canvases.begin(); it != s.canvases.end(); ++it) {
        if (*it == cv(c)) {
            s.canvases.erase(it);
            break;
        }
    }
    delete cv(c);
}

int api_canvas_width(tat_canvas_t *c) { return cv(c)->width(); }
uint8_t *api_canvas_pixels(tat_canvas_t *c) { return cv(c)->pixels(); }
uint8_t api_canvas_color(tat_canvas_t *c, tat_color_t rgb) { return cv(c)->color(rgb); }
uint8_t api_canvas_reserve(tat_canvas_t *c, int n) { return cv(c)->reserve(n); }
void api_canvas_set_color(tat_canvas_t *c, uint8_t i, tat_color_t rgb) { cv(c)->setColor(i, rgb); }
void api_canvas_clear(tat_canvas_t *c, uint8_t i) { cv(c)->clear(i); }
void api_canvas_pixel(tat_canvas_t *c, int x, int y, uint8_t i) { cv(c)->pixel(x, y, i); }
void api_canvas_fill_rect(tat_canvas_t *c, int x, int y, int w, int h, uint8_t i) { cv(c)->fillRect(x, y, w, h, i); }
void api_canvas_rect(tat_canvas_t *c, int x, int y, int w, int h, uint8_t i) { cv(c)->rect(x, y, w, h, i); }
void api_canvas_line(tat_canvas_t *c, int x0, int y0, int x1, int y1, uint8_t i) { cv(c)->line(x0, y0, x1, y1, i); }
void api_canvas_fill_circle(tat_canvas_t *c, int x, int y, int r, uint8_t i) { cv(c)->fillCircle(x, y, r, i); }

int api_canvas_text(tat_canvas_t *c, int x, int y, const char *t, uint8_t i, int scale, bool bold)
{
    return cv(c)->text(x, y, t, i, scale, bold);
}

void api_canvas_text_centered(tat_canvas_t *c, int x, int y, const char *t, uint8_t i, int scale, bool bold)
{
    cv(c)->textCentered(x, y, t, i, scale, bold);
}

tat_sheet_t *api_sheet_load(tat_canvas_t *c, const void *png, size_t len, int fw, int fh)
{
    auto *sheet = new wc::Sheet();
    if (!cv(c)->loadSheet(*sheet, static_cast<const uint8_t *>(png), len, fw, fh)) {
        delete sheet;
        return nullptr;
    }
    s.sheets.push_back(sheet);
    return reinterpret_cast<tat_sheet_t *>(sheet);
}

void api_canvas_sprite(tat_canvas_t *c, tat_sheet_t *x, int frame, int px, int py, bool flip)
{
    cv(c)->sprite(*sh(x), frame, px, py, flip);
}

void api_canvas_sprite_scaled(tat_canvas_t *c, tat_sheet_t *x, int frame, float px, float py, float sx,
                              float sy, bool flip)
{
    cv(c)->spriteScaled(*sh(x), frame, px, py, sx, sy, flip);
}

void api_canvas_present(tat_canvas_t *c)
{
    if (s.engine) cv(c)->present(s.engine->presenter());
}

void api_canvas_present_rotated(tat_canvas_t *c, float radians)
{
    if (s.engine) cv(c)->presentRotated(s.engine->presenter(), radians);
}

void api_menu_open() { s.menu.open(); }
void api_menu_close() { s.menu.close(); }
bool api_menu_is_open() { return s.menu.isOpen(); }
void api_menu_invalidate() { s.menu.invalidate(); }

int api_menu_update()
{
    if (!s.engine) return TAT_MENU_NONE;
    const int r = s.menu.update(*s.engine, s.gestures, s.engine->input());
    // The console's values happen to line up with the game-facing ones; say so out loud
    // rather than relying on it.
    static_assert(int(console::ui::PauseMenu::None) == TAT_MENU_NONE, "menu None must match");
    static_assert(int(console::ui::PauseMenu::Closed) == TAT_MENU_CLOSED, "menu Closed must match");
    return r;
}

void api_menu_draw(const tat_menu_row_t *rows, int count, const char *title)
{
    console::ui::PauseMenu::Row out[4];
    if (count > 4) count = 4;
    for (int i = 0; i < count; i++) {
        out[i].label = rows[i].label;
        out[i].value = rows[i].value;
        out[i].color = rows[i].color ? rows[i].color : console::ui::VALUE;
    }
    if (s.engine) s.menu.draw(s.engine->gfx(), out, count, title ? title : "PAUSED");
}

tat_menu_row_t api_menu_sound_row()
{
    const auto r = s.menu.soundRow();
    return {r.label, r.value, r.color};
}

void api_menu_toggle_sound() { s.menu.toggleSound(); }

tat_color_t api_ui_color(int which)
{
    namespace ui = console::ui;
    switch (which) {
    case TAT_UI_LABEL: return ui::LABEL;
    case TAT_UI_DIM: return ui::DIM;
    case TAT_UI_VALUE: return ui::VALUE;
    case TAT_UI_ACCENT: return ui::ACCENT;
    case TAT_UI_GO: return ui::GO;
    case TAT_UI_DANGER: return ui::DANGER;
    default: return ui::TEXT;
    }
}

void api_tone(const tat_tone_t *t)
{
    wc::audio::play({.f0 = t->f0,
                     .f1 = t->f1,
                     .ms = t->ms,
                     .wave = wc::audio::Wave(t->wave),
                     .volume = t->volume,
                     .delay_ms = t->delay_ms});
}

int api_volume() { return wc::audio::volume(); }

void api_save_get(const char *key, int *value, int limit)
{
    if (!s.game) return;
    wc::Store store(s.game->id);
    store.get(key, *value, limit);
}

void api_save_set(const char *key, int value)
{
    if (!s.game) return;
    wc::Store store(s.game->id, wc::Store::Write);
    store.set(key, value);
}

const void *api_asset(const char *name, size_t *len)
{
    if (!s.game) return nullptr;
    for (int i = 0; i < s.game->asset_count; i++) {
        const tat_asset_t &a = s.game->assets[i];
        if (std::strcmp(a.name, name) == 0) {
            if (len) *len = size_t(a.end - a.data);
            return a.data;
        }
    }
    if (len) *len = 0;
    return nullptr;
}

// Built on first use rather than at boot: it is the better part of a megabyte, and a
// console whose games never ask for it should not be carrying it.
bool polar_ready()
{
    static bool tried = false, ok = false;
    if (!tried) {
        tried = true;
        ok = wc::Polar::init();
        if (!ok) ESP_LOGE(TAG, "no memory for the polar tables");
    }
    return ok;
}

const uint16_t *api_polar_angles() { return polar_ready() ? wc::Polar::angles() : nullptr; }
const uint16_t *api_polar_radii() { return polar_ready() ? wc::Polar::radii() : nullptr; }

int api_text_width(const char *s, int scale, bool bold) { return wc::Gfx::textWidth(s, scale, bold); }

void api_canvas_set_palette(tat_canvas_t *c, const tat_color_t *pal, int n) { cv(c)->setPalette(pal, n); }

const uint8_t *api_sheet_pixels(tat_sheet_t *x) { return sh(x)->px; }

void api_sheet_info(tat_sheet_t *x, int *w, int *h, int *fw, int *fh)
{
    const wc::Sheet *s = sh(x);
    if (w) *w = s->w;
    if (h) *h = s->h;
    if (fw) *fw = s->fw;
    if (fh) *fh = s->fh;
}

void api_canvas_banner(tat_canvas_t *c, int cx, int y, int w, const tat_banner_t *b)
{
    console::ui::BannerStyle style{b->panel, b->border, b->mid_color, b->bottom_color};
    style.top_scale = b->top_scale ? b->top_scale : 1;
    style.bars = b->bars;
    style.bottom_bold = b->bottom_bold;
    console::ui::banner(*cv(c), cx, y, w, b->top, b->mid, b->bottom, b->top_color, style);
}

tat_api_t s_api = {
    TAT_API_MAJOR, TAT_API_MINOR,
    api_rgb, api_go_home, api_log, api_now_us, api_random,
    api_input, api_gestures,
    api_alloc, api_free,
    api_canvas_create, api_canvas_destroy, api_canvas_width, api_canvas_pixels,
    api_canvas_color, api_canvas_reserve, api_canvas_set_color,
    api_canvas_clear, api_canvas_pixel, api_canvas_fill_rect, api_canvas_rect,
    api_canvas_line, api_canvas_fill_circle, api_canvas_text, api_canvas_text_centered,
    api_sheet_load, api_canvas_sprite, api_canvas_sprite_scaled,
    api_canvas_present, api_canvas_present_rotated,
    api_menu_open, api_menu_close, api_menu_is_open, api_menu_invalidate,
    api_menu_update, api_menu_draw, api_menu_sound_row, api_menu_toggle_sound, api_ui_color,
    api_tone, api_volume,
    api_save_get, api_save_set,
    api_asset,
    api_canvas_banner,
    api_polar_angles, api_polar_radii,
    api_text_width,
    api_sheet_pixels, api_sheet_info,
    api_canvas_set_palette,
};

// The input the game sees is a copy: it must not be able to reach into the engine's.
void snapshot(wc::Engine &e)
{
    const wc::InputState &in = e.input();
    s.gestures.update(in.touch);
    s.input.held = in.held;
    s.input.pressed = in.pressed;
    s.input.released = in.released;
    s.input.clicked = in.clicked;
    s.input.long_press = in.long_press;
    s.input.double_clicked = in.double_clicked;
    s.input.touch = {uint8_t(in.touch.down), uint8_t(in.touch.pressed), uint8_t(in.touch.released), in.touch.x,
                     in.touch.y};
    s.input.tilt = {in.tilt.ax, in.tilt.ay, in.tilt.az, in.tilt.gx, in.tilt.gy, in.tilt.gz};
    s.ges = {uint8_t(s.gestures.tap), uint8_t(s.gestures.swipe_left), uint8_t(s.gestures.swipe_right),
             s.gestures.x, s.gestures.y};
}

}  // namespace

const tat_api_t *api() { return &s_api; }

void HostedGame::begin(wc::Engine &e)
{
    s.engine = &e;
    s.game = &g_;
    if (g_.magic != TAT_GAME_MAGIC) {
        ESP_LOGE(TAG, "%s is not a game", g_.id ? g_.id : "?");
        return;
    }
    if (g_.api_major != TAT_API_MAJOR || g_.api_minor > TAT_API_MINOR) {
        ESP_LOGE(TAG, "%s wants API %u.%u; this console has %u.%u", g_.id, g_.api_major, g_.api_minor,
                 TAT_API_MAJOR, TAT_API_MINOR);
        return;
    }
    ESP_LOGI(TAG, "%s: begin=%p enter=%p update=%p draw=%p", g_.id ? g_.id : "?", (void *)g_.begin,
             (void *)g_.enter, (void *)g_.update, (void *)g_.draw);
    if (g_.begin) g_.begin(&s_api);
}

void HostedGame::enter(wc::Engine &e)
{
    s.engine = &e;
    s.game = &g_;
    s.menu.close();
    if (g_.enter) g_.enter();
}

void HostedGame::leave(wc::Engine &e)
{
    if (g_.leave) g_.leave();
}

void HostedGame::update(wc::Engine &e, float dt)
{
    s.engine = &e;
    s.game = &g_;
    snapshot(e);
    if (g_.update) g_.update(dt);
}

void HostedGame::draw(wc::Engine &e, wc::Gfx &g)
{
    s.engine = &e;
    if (g_.draw) g_.draw();
}

void HostedGame::redraw()
{
    s.menu.invalidate();
    if (g_.redraw) g_.redraw();
}

bool HostedGame::keepAwake() const { return g_.keep_awake ? g_.keep_awake() : false; }

}  // namespace tat
