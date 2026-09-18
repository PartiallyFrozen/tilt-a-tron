#include "console/theme.h"

#include <dirent.h>
#include <algorithm>
#include <cctype>
#include <sys/stat.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "cJSON.h"
#include "console/console.h"
#include "console/ui.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lodepng.h"
#include "nvs.h"
#include "storage/storage.h"

namespace console {

using wc::Color;

static const char *TAG = "theme";

void Image::release()
{
    heap_caps_free(px);
    heap_caps_free(alpha);
    px = nullptr;
    alpha = nullptr;
    w = h = 0;
}

void drawImage(wc::Gfx &g, const Image &img, int x, int y)
{
    if (!img.valid()) return;
    if (img.alpha) g.blitAlpha(img.px, img.alpha, img.w, img.h, x, y);
    else g.blit(img.px, img.w, img.h, x, y);
}

// "Marble Maze.png", "marble-maze.png" and "MARBLEMAZE.PNG" should all mean the
// same thing: lower-case letters and digits only.
static std::string slug(const std::string &s)
{
    std::string out;
    for (char ch : s)
        if (std::isalnum(static_cast<unsigned char>(ch))) out += char(std::tolower(static_cast<unsigned char>(ch)));
    return out;
}

// Width and height from the PNG header alone (33 bytes), without decoding.
static bool pngSize(const std::string &path, unsigned &w, unsigned &h, bool warn = false)
{
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    unsigned char head[33];
    const size_t n = std::fread(head, 1, sizeof(head), f);
    std::fclose(f);
    if (n < sizeof(head)) return false;

    LodePNGState state;
    lodepng_state_init(&state);
    const unsigned err = lodepng_inspect(&w, &h, &state, head, n);
    lodepng_state_cleanup(&state);
    if (err && warn) ESP_LOGW(TAG, "%s: not a PNG (%s)", path.c_str(), lodepng_error_text(err));
    return err == 0;
}

// Decoding needs width*height*4 bytes at once, so check the PNG header before
// touching a huge image someone dropped on the drive.
static bool pngFits(const std::string &path, int max_w, int max_h)
{
    unsigned w = 0, h = 0;
    if (!pngSize(path, w, h, true)) return false;
    // Bigger pictures are shrunk to fit (see loadPng), but decoding still needs
    // width*height*4 bytes, so there's a limit on what we'll take on.
    constexpr uint32_t kMaxPixels = 1600000;   // ~1264x1264
    if (w == 0 || h == 0 || uint32_t(w) * h > kMaxPixels) {
        ESP_LOGW(TAG, "%s is %ux%u - too big to load", path.c_str(), w, h);
        return false;
    }
    // RGBA scratch + our RGB565 + alpha copies, with room to spare.
    const size_t needed = size_t(w) * h * 7 + 65536;
    const size_t have = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    if (needed > have) {
        ESP_LOGW(TAG, "%s needs %u KB, only %u KB free - skipped", path.c_str(), unsigned(needed / 1024),
                 unsigned(have / 1024));
        return false;
    }
    return true;
}

static bool loadPng(const std::string &path, Image &out, int max_w, int max_h)
{
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return false;
    if (!pngFits(path, max_w, max_h)) return false;

    unsigned char *rgba = nullptr;
    unsigned w = 0, h = 0;
    const unsigned err = lodepng_decode32_file(&rgba, &w, &h, path.c_str());
    if (err) {
        ESP_LOGW(TAG, "%s: %s", path.c_str(), lodepng_error_text(err));
        free(rgba);
        return false;
    }

    // Too big for the screen? Shrink it by a whole-number factor rather than
    // refusing it, so any art someone drops in works.
    unsigned step = 1;
    while ((w + step - 1) / step > unsigned(max_w) || (h + step - 1) / step > unsigned(max_h)) step++;
    const unsigned out_w = (w + step - 1) / step, out_h = (h + step - 1) / step;
    if (step > 1) ESP_LOGI(TAG, "%s: %ux%u shrunk to %ux%u", path.c_str(), w, h, out_w, out_h);

    out.release();
    out.w = out_w;
    out.h = out_h;
    out.px = static_cast<Color *>(heap_caps_malloc(out_w * out_h * sizeof(Color), MALLOC_CAP_SPIRAM));
    out.alpha = static_cast<uint8_t *>(heap_caps_malloc(out_w * out_h, MALLOC_CAP_SPIRAM));
    if (!out.px || !out.alpha) {
        out.release();
        free(rgba);
        return false;
    }
    bool translucent = false;
    if (step == 1) {
        for (unsigned i = 0; i < w * h; i++) {
            const unsigned char *p = rgba + i * 4;
            out.px[i] = wc::rgb(p[0], p[1], p[2]);
            out.alpha[i] = p[3];
            translucent |= p[3] != 255;
        }
    } else {
        // Shrink by averaging step x step blocks.
        for (unsigned y = 0; y < out_h; y++) {
            for (unsigned x = 0; x < out_w; x++) {
                unsigned r = 0, g = 0, b = 0, a = 0, n = 0;
                for (unsigned sy = y * step; sy < (y + 1) * step && sy < h; sy++) {
                    for (unsigned sx = x * step; sx < (x + 1) * step && sx < w; sx++) {
                        const unsigned char *p = rgba + (sy * w + sx) * 4;
                        r += p[0];
                        g += p[1];
                        b += p[2];
                        a += p[3];
                        n++;
                    }
                }
                const unsigned i = y * out_w + x;
                out.px[i] = wc::rgb(r / n, g / n, b / n);
                out.alpha[i] = uint8_t(a / n);
                translucent |= out.alpha[i] != 255;
            }
        }
    }
    free(rgba);
    if (!translucent) {   // opaque images draw with a straight copy
        heap_caps_free(out.alpha);
        out.alpha = nullptr;
    }
    return true;
}

static bool parseColor(cJSON *colors, const char *key, Color &out)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(colors, key);
    if (!cJSON_IsString(item) || !item->valuestring) return false;
    const char *s = item->valuestring;
    if (*s == '#') s++;
    if (std::strlen(s) != 6) return false;
    char *end = nullptr;
    const unsigned long v = std::strtoul(s, &end, 16);
    if (*end) return false;
    out = wc::rgb((v >> 16) & 255, (v >> 8) & 255, v & 255);
    return true;
}

Theme &Theme::get()
{
    static Theme t;
    return t;
}

const Image *Theme::icon(const std::string &app_id, const std::string &title) const
{
    auto it = icons_.find(app_id);
    if (it != icons_.end()) return &it->second;
    const std::string id = slug(app_id), t = slug(title);
    for (const auto &kv : icons_) {
        const std::string &k = kv.first;
        if (k == id || k == t || k.find(id) != std::string::npos || k.find(t) != std::string::npos ||
            (k.size() >= 4 && t.find(k) != std::string::npos))
            return &kv.second;
    }
    return nullptr;
}

void Theme::clear()
{
    background_.release();
    for (auto &kv : icons_) kv.second.release();
    icons_.clear();
    // Built-in palette.
    ui::BG = wc::colors::black;
    ui::TEXT = wc::colors::white;
    ui::LABEL = wc::rgb(225, 225, 225);
    ui::DIM = wc::rgb(150, 150, 150);
    ui::PANEL = wc::rgb(16, 18, 26);
    ui::BOX = wc::rgb(90, 90, 100);
    ui::VALUE = wc::colors::cyan;
    ui::ACCENT = wc::colors::yellow;
    ui::GO = wc::rgb(40, 200, 110);
    ui::DANGER = wc::colors::red;
}

// While a theme loads there's nothing on screen to explain the pause, so draw a
// simple progress screen: this runs on the app task, between file loads.
static void drawLoading(wc::Engine *e, const std::string &theme, const std::string &file, int done, int total)
{
    if (!e) return;
    wc::Gfx &g = e->gfx();
    const int cx = wc::Gfx::CX;
    g.clear(wc::colors::black);
    g.textCentered(cx, 170, "LOADING THEME", wc::colors::white, 3, true);
    std::string name = theme;
    for (auto &ch : name) ch = char(std::toupper(ch));
    g.textCentered(cx, 206, name.c_str(), ui::ACCENT, 2, true);

    const int bar_w = 300, bar_x = cx - bar_w / 2, bar_y = 250;
    g.rect(bar_x, bar_y, bar_w, 20, ui::BOX);
    if (total > 0) g.fillRect(bar_x + 2, bar_y + 2, (bar_w - 4) * done / total, 16, ui::GO);
    g.textCentered(cx, 296, file.c_str(), ui::DIM, 2, true);

    e->presenter().present(g);
    e->presenter().flush();
}

void Theme::load(const std::string &name, wc::Engine *progress_ui)
{
    const int64_t t0 = esp_timer_get_time();
    clear();
    name_ = name;
    generation_++;
    if (!storage_ready()) return;

    const std::string dir = std::string(STORAGE_THEMES) + "/" + name;
    crumb(("theme " + name).c_str());

    if (FILE *f = std::fopen((dir + "/theme.json").c_str(), "rb")) {
        std::string text;
        char buf[512];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
        std::fclose(f);
        if (cJSON *root = cJSON_Parse(text.c_str())) {
            if (cJSON *c = cJSON_GetObjectItemCaseSensitive(root, "colors")) {
                parseColor(c, "background", ui::BG);
                parseColor(c, "text", ui::TEXT);
                parseColor(c, "label", ui::LABEL);
                parseColor(c, "dim", ui::DIM);
                parseColor(c, "panel", ui::PANEL);
                parseColor(c, "box", ui::BOX);
                parseColor(c, "value", ui::VALUE);
                parseColor(c, "accent", ui::ACCENT);
                parseColor(c, "go", ui::GO);
                parseColor(c, "danger", ui::DANGER);
            }
            cJSON_Delete(root);
        } else {
            ESP_LOGW(TAG, "%s/theme.json isn't valid JSON", name.c_str());
        }
    }

    crumb("background.png");
    drawLoading(progress_ui, name, "theme.json", 0, 3);
    // List the icons first so the progress bar knows how much work there is.
    // The documented place is icons/, but PNGs dropped next to theme.json count
    // too, as long as they aren't the background.
    std::vector<std::string> icon_files;   // paths relative to the theme folder
    for (const std::string &sub : {std::string("icons/"), std::string("")}) {
        if (DIR *d = opendir((dir + "/" + sub).c_str())) {
            while (dirent *e = readdir(d)) {
                std::string file = e->d_name;
                // Skip the junk Windows/macOS leave behind (._name.png, .DS_Store, ...).
                if (file.size() < 5 || file[0] == '.') continue;
                std::string ext = file.substr(file.size() - 4);
                for (auto &ch : ext) ch = char(std::tolower(ch));
                if (ext == ".png") icon_files.push_back(sub + file);
            }
            closedir(d);
        }
    }

    // background.png is the documented name, but any PNG dropped in the theme
    // folder works too (people export art with all sorts of names): take the one
    // called something like "background", else the biggest picture in the folder.
    std::string bg_file;
    {
        uint32_t best_px = 0;
        for (const std::string &file : icon_files) {
            if (file.rfind("icons/", 0) == 0) continue;
            const std::string s = slug(file.substr(0, file.size() - 4));
            unsigned w = 0, h = 0;
            pngSize(dir + "/" + file, w, h);
            const bool named = s.find("background") != std::string::npos || s == "bg" || s.find("back") == 0;
            const uint32_t px = uint32_t(w) * h + (named ? 1u << 30 : 0);
            if (px > best_px) {
                best_px = px;
                bg_file = file;
            }
        }
        if (!bg_file.empty())
            icon_files.erase(std::find(icon_files.begin(), icon_files.end(), bg_file));
        else
            bg_file = "background.png";
    }

    const int total = 1 + int(icon_files.size());
    int done = 0;
    drawLoading(progress_ui, name, bg_file.c_str(), done, total);
    loadPng(dir + "/" + bg_file, background_, wc::Gfx::W, wc::Gfx::H);
    ++done;
    std::string icon_names;
    for (const std::string &file : icon_files) {
        const size_t slash = file.rfind('/');
        const std::string base = file.substr(slash == std::string::npos ? 0 : slash + 1);
        const std::string id = slug(base.substr(0, base.size() - 4));
        Image img;
        crumb(base.c_str());
        drawLoading(progress_ui, name, base.c_str(), ++done, total);
        if (loadPng(dir + "/" + file, img, 232, 232)) {
            icons_[id] = img;
            icon_names += (icon_names.empty() ? "" : ", ") + file;
        }
    }
    drawLoading(progress_ui, name, "", total, total);
    ESP_LOGI(TAG, "loaded '%s' (bg %s: %s; %u icons: %s) in %lld ms", name.c_str(),
             background_.valid() ? "yes" : "no", bg_file.c_str(), unsigned(icons_.size()), icon_names.c_str(),
             (esp_timer_get_time() - t0) / 1000);
    crumb("");
}

std::vector<std::string> Theme::available() const
{
    std::vector<std::string> names;
    if (!storage_ready()) return names;
    if (DIR *d = opendir(STORAGE_THEMES)) {
        while (dirent *e = readdir(d)) {
            const std::string name = e->d_name;
            if (name.empty() || name[0] == '.' || name == "System Volume Information") continue;
            if (name.compare(0, 6, "FOUND.") == 0) continue;   // chkdsk leftovers
            if (e->d_type != DT_DIR) {
                // Not every filesystem fills in d_type; ask the filesystem directly.
                struct stat st;
                if (stat((std::string(STORAGE_THEMES) + "/" + name).c_str(), &st) != 0 || !S_ISDIR(st.st_mode))
                    continue;
            }
            names.push_back(name);
        }
        closedir(d);
    } else {
        ESP_LOGW(TAG, "no %s folder on the drive", STORAGE_THEMES);
    }
    // Default first, the rest alphabetical.
    std::sort(names.begin(), names.end(), [](const std::string &a, const std::string &b) {
        if (a == "Default") return true;
        if (b == "Default") return false;
        return a < b;
    });
    return names;
}

void Theme::loadActive()
{
    std::string name = "Default";
    nvs_handle_t h;
    if (nvs_open("console", NVS_READONLY, &h) == ESP_OK) {
        char buf[64];
        size_t len = sizeof(buf);
        if (nvs_get_str(h, "theme", buf, &len) == ESP_OK) name = buf;
        nvs_close(h);
    }
    // A theme folder that was deleted or renamed on the computer falls back to Default.
    const auto names = available();
    if (std::find(names.begin(), names.end(), name) == names.end()) name = "Default";
    storage_gen_ = storage_generation();
    load(name);
}

void Theme::setActive(const std::string &name)
{
    nvs_handle_t h;
    if (nvs_open("console", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "theme", name.c_str());
        nvs_commit(h);
        nvs_close(h);
    }
    load(name);
}

namespace ui {

void clearScreen(wc::Gfx &g, int dx, int dy)
{
    const Image &bg = Theme::get().background();
    if (bg.valid() && bg.w == wc::Gfx::W && bg.h == wc::Gfx::H && !bg.alpha && dx == 0 && dy == 0) {
        g.blit(bg.px, bg.w, bg.h, 0, 0);
        return;
    }
    g.clear(BG);   // fills whatever the shifted picture leaves uncovered
    drawImage(g, bg, (wc::Gfx::W - bg.w) / 2 + dx, (wc::Gfx::H - bg.h) / 2 + dy);
}

void restoreBg(wc::Gfx &g, int x, int y, int w, int h, int dx, int dy)
{
    const Image &bg = Theme::get().background();
    g.setClip(x, y, w, h);
    g.fillRect(x, y, w, h, BG);
    drawImage(g, bg, (wc::Gfx::W - bg.w) / 2 + dx, (wc::Gfx::H - bg.h) / 2 + dy);
    g.clearClip();
}

}  // namespace ui

bool Theme::needsReload() const { return storage_generation() != storage_gen_ && storage_ready(); }

bool Theme::poll()
{
    storage_service();
    if (!needsReload()) return false;
    loadActive();
    return true;
}

void Theme::reloadWithProgress(wc::Engine &e)
{
    storage_service();
    if (!needsReload()) return;
    storage_gen_ = storage_generation();
    std::string want = name_;
    const auto names = available();
    if (std::find(names.begin(), names.end(), want) == names.end()) want = "Default";
    load(want, &e);
}

}  // namespace console
