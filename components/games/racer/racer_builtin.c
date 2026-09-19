// How GRAND PRIX's files reach it when the game is built into the firmware.
//
// This is the only part of the game that knows how it was built: in a .tat the same five
// PNGs travel inside the package and the loader supplies this table instead, and racer.c
// does not change a line either way.
//
// The PNGs themselves sit with the other games' art under components/games/assets/, where
// the firmware build embeds them all together and tools/make_sprites.py writes them.
#include "tat/tat_api.h"

extern const uint8_t _binary_car_png_start[], _binary_car_png_end[];
extern const uint8_t _binary_trees_png_start[], _binary_trees_png_end[];
extern const uint8_t _binary_sign_png_start[], _binary_sign_png_end[];
extern const uint8_t _binary_stand_png_start[], _binary_stand_png_end[];
extern const uint8_t _binary_bush_png_start[], _binary_bush_png_end[];

#define EMBEDDED(sym) {#sym ".png", _binary_##sym##_png_start, _binary_##sym##_png_end}

const tat_asset_t tat_assets[] = {
    EMBEDDED(car), EMBEDDED(trees), EMBEDDED(sign), EMBEDDED(stand), EMBEDDED(bush),
};
