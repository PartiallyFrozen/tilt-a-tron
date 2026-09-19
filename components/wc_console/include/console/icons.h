#pragma once

#include "engine/gfx.h"

namespace console::icons {

// Each fills a (2r x 2r) buffer; pixels outside the icon circle are left black.
void breakout(wc::Color *buf, int r);
void settings(wc::Color *buf, int r);
void maze(wc::Color *buf, int r);
void racer(wc::Color *buf, int r);
void jump(wc::Color *buf, int r);
void tiltatris(wc::Color *buf, int r);
void clock(wc::Color *buf, int r);
void star(wc::Color *buf, int r);
void pindrop(wc::Color *buf, int r);
// For a game that arrived as a package and brought no icon of its own. A cartridge, so
// that "this one was installed" is something you can see on the home screen.
void package(wc::Color *buf, int r);

}  // namespace console::icons
