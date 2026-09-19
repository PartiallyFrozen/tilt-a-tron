#include "console/launcher.h"

#include "board/board.h"
#include <cstdio>

#include <algorithm>
#include <cmath>
#include <cstring>

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

void Launcher::buildIcons()
{
    for (Image &img : builtin_icons_) img.release();
    builtin_icons_.clear();
    const int size = 2 * ICON_R;
    for (int i = 0; i < n_; i++) {
        Image img;
        // A package's own icon first, then the firmware's for the apps that are part of
        // it, and only then the one drawn in code - which for a package is the blank
        // cartridge, and means its icon is missing or will not decode.
        if (Theme::iconFromPng(apps_[i].icon_png, apps_[i].icon_len, apps_[i].id, img)
            || (!apps_[i].packaged && Theme::builtinIcon(apps_[i].id, img))) {
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
}

// A game was installed or removed over the link. Everything after the built-ins may have
// moved, so the selection is carried across by id rather than by position - the ids are
// copied first, because the strings they point at are about to be rewritten.
void Launcher::rescan()
{
    storage_gen_ = storage_generation();
    if (!rescan_) return;
    char selected[32] = "";
    if (!vis_.empty()) std::snprintf(selected, sizeof(selected), "%s", app(sel_).id);

    const int n = rescan_();
    if (n < 0) return;
    n_ = n;
    buildIcons();
    last_app_ = 0;
    for (int i = 0; i < n_; i++)
        if (std::strcmp(apps_[i].id, selected) == 0) last_app_ = i;
    vis_.clear();
    refreshVisible();
    anim_ = 0;
    full_ = true;
    ESP_LOGI(TAG, "%d apps after a rescan", n_);
}

void Launcher::begin(Engine &e)
{
    buildIcons();
    storage_gen_ = storage_generation();
    // Start on the app you played last.
    nvs_handle_t h;
    if (nvs_open("console", NVS_READONLY, &h) == ESP_OK) {
        uint8_t v;
        if (nvs_get_u8(h, "last_app", &v) == ESP_OK && v < n_) last_app_ = v;
        nvs_close(h);
    }
    refreshVisible();
    ESP_LOGI(TAG, "%d apps", n_);
}

// Rebuild the carousel from the apps that aren't hidden, keeping the same app selected.
void Launcher::refreshVisible()
{
    const int current = vis_.empty() ? last_app_ : vis_[sel_];
    vis_.clear();
    for (int i = 0; i < n_; i++)
        if (!appHidden(apps_[i].id)) vis_.push_back(i);
    sel_ = 0;
    for (int s = 0; s < count(); s++)
        if (vis_[s] == current) sel_ = s;
}

void Launcher::enter(Engine &e)
{
    if (storage_generation() != storage_gen_) rescan();
    refreshVisible();
    full_ = true;
    anim_ = 0;
}

const Image &Launcher::iconFor(int app) const
{
    // A theme dresses any app whose id it names. Matching on the title as well is kept
    // for the apps that are part of the firmware: a title is whatever a package says it
    // is, and "BREAKOUT" from a stranger should not pick up the theme's Breakout icon. An
    // id cannot be borrowed that way - the watch only ever holds one game per id.
    const char *title = apps_[app].packaged ? "" : apps_[app].name;
    if (const Image *themed = Theme::get().icon(apps_[app].id, title)) return *themed;
    return builtin_icons_[app];
}

void Launcher::move(int dir)
{
    sel_ = (sel_ + dir + count()) % count();
    anim_ += dir * SPACING;   // new selection slides in from the side we swiped from
    full_ = true;
}

void Launcher::update(Engine &e, float dt)
{
    const auto &in = e.input();
    ges_.update(in.touch);

    if (storage_generation() != storage_gen_) rescan();
    // New files may have arrived over the USB link: reload, showing progress.
    Theme::get().reloadWithProgress(e);
    if (Theme::get().generation() != theme_gen_) {
        theme_gen_ = Theme::get().generation();
        full_ = true;
    }

    if (ges_.swipe_left) move(+1);
    if (ges_.swipe_right) move(-1);
    // PWR on the home screen does one thing: double-click puts the watch to sleep.
    if (in.double_clicked & wc::BTN_B) {
        wc::audio::play({.f0 = 700, .f1 = 180, .ms = 260, .wave = wc::audio::Wave::Triangle, .volume = 0.6f});
        e.sleep();
        return;
    }

    battery_t_ += dt;
    if (battery_t_ > 5.0f) {
        battery_t_ = 0;
        const int pct = pmu_battery_percent();
        const bool chg = pmu_charging() || pmu_usb_power();
        if (pct != battery_pct_ || chg != battery_charging_) {
            battery_pct_ = pct;
            battery_charging_ = chg;
            battery_dirty_ = true;
        }
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
    if (tapped_icon && std::fabs(anim_) < 40 && app(sel_).game) {
        nvs_handle_t h;
        if (nvs_open("console", NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_u8(h, "last_app", uint8_t(vis_[sel_]));
            nvs_commit(h);
            nvs_close(h);
        }
        e.switchTo(*app(sel_).game);
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
        if (k != 0 && count() < 2) continue;   // a lone app has no neighbours
        const int i = vis_[(sel_ + k + count()) % count()];
        const Image &icon = iconFor(i);
        if (!icon.valid()) continue;
        const int cx = Gfx::CX + shift + k * SPACING;
        const int cy = ICON_CY;
        if (cx + icon.w / 2 < 0 || cx - icon.w / 2 >= Gfx::W) continue;
        drawImage(g, icon, cx - icon.w / 2, cy - icon.h / 2);
    }
}

void Launcher::draw(Engine &e, Gfx &g)
{
    const int shift = int(std::lround(anim_));

    if (full_) {
        ui::clearScreen(g);   // the theme's background carries any branding
        ui::shadowText(g, Gfx::CX, Gfx::CY + 112, app(sel_).name, app(sel_).accent, 4);
        // "3 OF 11" rather than a row of dots. Dots were fine for eight built-in games and
        // stop being fine the moment somebody installs a few: at twenty they are wider than
        // the screen, and past about a dozen nobody is counting them anyway. Text costs the
        // same few characters whether there are four games or four hundred.
        char pos[32];
        std::snprintf(pos, sizeof(pos), "%d OF %d", sel_ + 1, count());
        ui::shadowText(g, Gfx::CX, Gfx::CY + 152, pos, ui::DIM, 2);
        full_ = false;
        hint_dirty_ = true;
        drawn_shift_ = 1 << 30;
    }
    if (hint_dirty_) {
        static const char *const hints[] = {"TAP TO PLAY", "PWR X2: SLEEP"};
        ui::restoreBg(g, Gfx::CX - 110, Gfx::CY + 175, 220, 22);
        ui::shadowText(g, Gfx::CX, Gfx::CY + 185, hints[hint_], ui::DIM, 2);
        hint_dirty_ = false;
        battery_dirty_ = true;
    }
    if (battery_dirty_ && battery_pct_ >= 0) {
        // Battery at the bottom, under the hint: a little cell with its charge and the percentage.
        const int y = Gfx::CY + 208, bx = Gfx::CX - 40, bw = 28, bh = 14;
        ui::restoreBg(g, Gfx::CX - 56, y - 12, 112, 26);
        const Color c = battery_charging_ ? ui::GO : battery_pct_ <= 15 ? ui::DANGER : ui::TEXT;
        g.fillRect(bx + 1, y - bh / 2 + 1, bw, bh, wc::rgb(0, 0, 0));   // shadow
        g.rect(bx, y - bh / 2, bw, bh, c);
        g.fillRect(bx + bw, y - 3, 3, 6, c);                          // the nub
        const int fill = (bw - 4) * battery_pct_ / 100;
        if (fill > 0) g.fillRect(bx + 2, y - bh / 2 + 2, fill, bh - 4, c);
        char buf[8];
        snprintf(buf, sizeof(buf), "%d%%", battery_pct_);
        ui::shadowText(g, bx + bw + 28, y, buf, c, 2);
        battery_dirty_ = false;
    }
    // The home screen is still unless the carousel is sliding: nothing to redraw,
    // nothing sent to the panel, and the engine idles between frames.
    if (shift != drawn_shift_) {
        drawIcons(g, shift);
        drawn_shift_ = shift;
    }
}

}  // namespace console
