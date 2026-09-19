// The games a new watch comes with: factory copies of packages, carried in the firmware.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Call once storage is up, before the games folder is scanned. Installs each factory game
// the first time this watch sees it, and brings an installed one up to date, once, when a
// firmware carries a build of it this watch has not been given before. A game the owner
// removed stays removed - unless the
// folder is empty, which is what a wiped drive looks like, and then they all come back.
// Returns how many files were written.
int factory_seed(void);

// Put back every factory game that is not installed. For Settings > GAMES > RESTORE.
int factory_restore(void);

// Where a game sits among the ones the watch came with, or -1 if it is not one of them.
// The home screen keeps these in this order however the filesystem happens to list them.
int factory_order(const char *id);

// Whether a package is, byte for byte, one the firmware carries. The home screen lets
// these - and only these - be matched to a theme's icon by title as well as by id.
bool factory_is(uint32_t stamp);

#ifdef __cplusplus
}
#endif
