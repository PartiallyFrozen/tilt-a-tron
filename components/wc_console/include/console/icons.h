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

}  // namespace console::icons
