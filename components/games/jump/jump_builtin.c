// How SKY JUMP's files reach it when the game is built into the firmware.
//
// This is the only part of the game that knows how it was built, which is the point of
// keeping it in its own file: in a .tat the same six PNGs travel inside the package and
// the loader supplies this table instead, and jump.c does not change a line either way.
//
// The PNGs themselves sit with the other games' art under components/games/assets/, where
// the firmware build embeds them all together and tools/make_sprites.py writes them.
#include "tat/tat_api.h"

extern const uint8_t _binary_hopper_png_start[], _binary_hopper_png_end[];
extern const uint8_t _binary_monster_png_start[], _binary_monster_png_end[];
extern const uint8_t _binary_ledges_png_start[], _binary_ledges_png_end[];
extern const uint8_t _binary_spring_png_start[], _binary_spring_png_end[];
extern const uint8_t _binary_shot_png_start[], _binary_shot_png_end[];
extern const uint8_t _binary_clouds_png_start[], _binary_clouds_png_end[];

#define EMBEDDED(sym) {#sym ".png", _binary_##sym##_png_start, _binary_##sym##_png_end}

const tat_asset_t tat_assets[] = {
    EMBEDDED(hopper), EMBEDDED(monster), EMBEDDED(ledges),
    EMBEDDED(spring), EMBEDDED(shot),    EMBEDDED(clouds),
};
