// Themes live on the Tilt-a-tron drive as plain files:
//   /data/Theme/<Name>/theme.json          colors
//   /data/Theme/<Name>/background.png      466x466, behind the launcher and menus
//   /data/Theme/<Name>/icons/<app id>.png  app icons (210x210)
// Images are decoded once at load into RGB565 + alpha. Anything a theme leaves
// out falls back to the built-in look.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "engine/engine.h"
#include "engine/gfx.h"

namespace console {

struct Image {
    int w = 0, h = 0;
    wc::Color *px = nullptr;
    uint8_t *alpha = nullptr;   // nullptr = opaque
    bool valid() const { return px != nullptr; }
    void release();
};

void drawImage(wc::Gfx &g, const Image &img, int x, int y);

class Theme {
public:
    static Theme &get();

    void loadActive();                                  // the saved choice, else Default
    void setActive(const std::string &name);            // save + load
    const std::string &name() const { return name_; }
    std::vector<std::string> available() const;         // folder names under Theme/

    // Reload if the drive came back from a computer. Returns true when it reloaded.
    bool poll();
    // True when the drive changed under us and the theme should be re-read.
    bool needsReload() const;
    // Reload, drawing a progress screen (loading a big theme takes a moment).
    void reloadWithProgress(wc::Engine &e);
    uint32_t generation() const { return generation_; }

    const Image &background() const { return background_; }
    // The firmware's own icon for an app (the Default theme's PNG), for the
    // carousel when the active theme doesn't provide one.
    static bool builtinIcon(const std::string &app_id, Image &out);
    // An icon from PNG bytes that came from somewhere else - a package brings its own.
    static bool iconFromPng(const uint8_t *png, size_t len, const char *what, Image &out);
    // The icon for an app: icons/<id>.png, or any icon file whose name looks
    // like the app's id or title ("Marble Maze.png", "grand-prix.png", ...).
    const Image *icon(const std::string &app_id, const std::string &title) const;

private:
    void load(const std::string &name, wc::Engine *progress_ui = nullptr);
    void clear();

    std::string name_ = "Default";
    Image background_;
    std::map<std::string, Image> icons_;
    uint32_t generation_ = 0;
    uint32_t storage_gen_ = 0;
};

}  // namespace console
