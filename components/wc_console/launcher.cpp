#include "console/launcher.h"

#include <algorithm>
#include <cmath>

#include "audio/audio.h"
#include "console/ui.h"
#include "storage/storage.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "nvs.h"

namespace console {

using wc::Color;
using wc::Engine;
using wc::Gfx;

static const char *TAG = "launcher";

void Launcher::begin(Engine &e)
{
    const int size = 2 * ICON_R;
    for (int i = 0; i < n_; i++) {
        Image img;
        if (Theme::builtinIcon(apps_[i].id, img)) {
            builtin_icons_.push_back(img);
            continue;
        }
        img.w = img.h = size;
        img.px = static_cast<Color *>(heap_caps_malloc(size * size * sizeof(Color), MALLOC_CAP_SPIRAM));
        img.alpha = static_cast<uint8_t *>(heap_caps_malloc(size * size, MALLOC_CAP_SPIRAM));
        if (img.px && img.alpha) {
            apps_[i].icon(img.px, ICON_R);
            for (int p = 0; p < size * size; p++) img.alpha[p] = img.px[p] ? 255 : 0;
        } else {
            img.release();
        }
        builtin_icons_.push_back(img);
    }
    // Start on the app you played last.
    nvs_handle_t h;
    if (nvs_open("console", NVS_READONLY, &h) == ESP_OK) {
        uint8_t v;
        if (nvs_get_u8(h, "last_app", &v) == ESP_OK && v < n_) sel_ = v;
        nvs_close(h);
    }
    ESP_LOGI(TAG, "%d apps", n_);
}

void Launcher::enter(Engine &e)
{
    full_ = true;
    anim_ = 0;
}

const Image &Launcher::iconFor(int app) const
{
    if (const Image *themed = Theme::get().icon(apps_[app].id, apps_[app].name)) return *themed;
    return builtin_icons_[app];
}

void Launcher::move(int dir)
{
    sel_ = (sel_ + dir + n_) % n_;
    anim_ += dir * SPACING;   // new selection slides in from the side we swiped from
    full_ = true;
}

void Launcher::update(Engine &e, float dt)
{
    const auto &in = e.input();
    ges_.update(in.touch);

    // Plugged into a computer: the drive belongs to them, so say so and wait.
    const bool on_computer = storage_on_computer();
    if (on_computer != on_computer_) {
        on_computer_ = on_computer;
        full_ = true;
    }
    if (on_computer) return;

    // Drive came back (maybe with new theme files): reload, showing progress.
    Theme::get().reloadWithProgress(e);
    if (Theme::get().generation() != theme_gen_) {
        theme_gen_ = Theme::get().generation();
        full_ = true;
    }

    if (ges_.swipe_left) move(+1);
    if (ges_.swipe_right) move(-1);
    since_click_move_ += dt;
    if (in.double_clicked & wc::BTN_B) {
        // If the first click of the pair already browsed an app, put it back, then sleep.
        if (since_click_move_ < 0.5f) move(-1);
        since_click_move_ = 99;
        wc::audio::play({.f0 = 700, .f1 = 180, .ms = 260, .wave = wc::audio::Wave::Triangle, .volume = 0.6f});
        e.sleep();
        return;
    }
    if (in.clicked & wc::BTN_B) {   // PWR also browses
        move(+1);
        since_click_move_ = 0;
    }

    hint_t_ += dt;
    if (hint_t_ > 3.0f) {
        hint_t_ = 0;
        hint_ = (hint_ + 1) % 2;
        hint_dirty_ = true;
    }

    const int icon_cx = Gfx::CX + int(anim_);
    const int dx = ges_.x - icon_cx, dy = ges_.y - ICON_CY;
    const bool tapped_icon = ges_.tap && dx * dx + dy * dy < (ICON_R + 30) * (ICON_R + 30);
    if (ges_.tap && !tapped_icon) {
        if (ges_.x < Gfx::CX - ICON_R) move(-1);
        else if (ges_.x > Gfx::CX + ICON_R) move(+1);
    }
    if (tapped_icon && std::fabs(anim_) < 40 && apps_[sel_].game) {
        nvs_handle_t h;
        if (nvs_open("console", NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_u8(h, "last_app", uint8_t(sel_));
            nvs_commit(h);
            nvs_close(h);
        }
        e.switchTo(*apps_[sel_].game);
        return;
    }

    // Snappy ease toward the selected app.
    anim_ *= std::exp(-dt * 14.0f);
    if (std::fabs(anim_) < 0.5f) anim_ = 0;

}

void Launcher::drawIcons(Gfx &g, int shift)
{
    const int band_y = ICON_CY - ICON_R - 12, band_h = 2 * ICON_R + 24;
    ui::restoreBg(g, 0, band_y, Gfx::W, band_h);
    for (int k = -1; k <= 1; k++) {
        const int i = (sel_ + k + n_) % n_;
        const Image &icon = iconFor(i);
        if (!icon.valid()) continue;
        const int cx = Gfx::CX + shift + k * SPACING;
        const int cy = ICON_CY;
        if (cx + icon.w / 2 < 0 || cx - icon.w / 2 >= Gfx::W) continue;
        drawImage(g, icon, cx - icon.w / 2, cy - icon.h / 2);
    }
}

bool Launcher::keepAwake() const { return storage_on_computer(); }

void Launcher::draw(Engine &e, Gfx &g)
{
    if (on_computer_) {
        if (!full_) return;
        full_ = false;
        drawn_shift_ = 1 << 30;   // repaint the carousel when the drive comes back
        g.clear(wc::colors::black);
        g.textCentered(Gfx::CX, 150, "CONNECTED", ui::VALUE, 4, true);
        g.textCentered(Gfx::CX, 196, "TO COMPUTER", ui::VALUE, 3, true);
        g.textCentered(Gfx::CX, 252, "ADD THEMES TO", ui::DIM, 2, true);
        g.textCentered(Gfx::CX, 278, "THE Theme FOLDER", ui::DIM, 2, true);
        g.textCentered(Gfx::CX, 330, "EJECT WHEN DONE", ui::ACCENT, 2, true);
        return;
    }

    const int shift = int(std::lround(anim_));

    if (full_) {
        ui::clearScreen(g);   // the theme's background carries any branding
        ui::shadowText(g, Gfx::CX, Gfx::CY + 112, apps_[sel_].name, apps_[sel_].accent, 4);
        const int dots_w = (n_ - 1) * 20;
        for (int i = 0; i < n_; i++) {
            const int x = Gfx::CX - dots_w / 2 + i * 20;
            g.fillCircle(x, Gfx::CY + 152, i == sel_ ? 6 : 4, i == sel_ ? ui::TEXT : ui::BOX);
        }
        full_ = false;
        hint_dirty_ = true;
        drawn_shift_ = 1 << 30;
    }
    if (hint_dirty_) {
        static const char *const hints[] = {"TAP TO PLAY", "PWR X2: SLEEP"};
        ui::restoreBg(g, Gfx::CX - 110, Gfx::CY + 175, 220, 22);
        ui::shadowText(g, Gfx::CX, Gfx::CY + 185, hints[hint_], ui::DIM, 2);
        hint_dirty_ = false;
    }
    // The home screen is still unless the carousel is sliding: nothing to redraw,
    // nothing sent to the panel, and the engine idles between frames.
    if (shift != drawn_shift_) {
        drawIcons(g, shift);
        drawn_shift_ = shift;
    }
}

}  // namespace console
