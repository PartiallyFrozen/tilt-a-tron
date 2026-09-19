// Loading a game that was not built into the firmware.
//
// A .tat is a header, a section table and the sections (docs/GAME_API.md section 2). The
// one that matters here is CODE: a relocatable ELF, linked as one contiguous image at
// address 0 with the relocations kept. Section 6.1 explains why that leaves almost nothing
// to do - every branch and call inside the image is already correct, so the loader copies
// the image somewhere, adds the load address to the absolute references, points the few
// outward calls at the console's own libc, and hands back the game's descriptor.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tat/tat_api.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LOADER_MAX_GAMES 24

// What the launcher needs to show an installed game without loading a byte of its code.
typedef struct {
    char path[96];     // where the .tat lives on storage
    char id[16];
    char name[24];
    char author[24];
    uint16_t version;
    uint16_t api_major, api_minor;
    uint8_t accent_r, accent_g, accent_b;
    uint32_t size;     // the whole file, for showing and for uninstalling
    uint32_t stamp;    // the header's CRC, which covers every section's: differs if the file does
    bool runnable;     // false when this console cannot satisfy its API version
} loader_entry_t;

// Read every package in the games folder. Cheap: headers only, no code. Returns how many
// were found, which is also how many entries were filled in.
int loader_scan(loader_entry_t *out, int max);

// The same facts about a package that is already in memory - a factory copy carried in the
// firmware, say. `path` is left empty. False if it is not a sound package.
bool loader_inspect(const uint8_t *buf, size_t len, loader_entry_t *out);

// A package's own icon: the PNG from its ICON section, copied out so the file can be
// closed. NULL if it has none or is not a sound package. Give it back with loader_free().
// Like the scan, this never touches the game's code.
uint8_t *loader_read_icon(const char *path, size_t *len);
void loader_free(void *p);

// The folder installed packages live in, under the storage root. Sending a .tat here is
// what installing IS - there is no separate install command, and deleting the file is
// the uninstall.
const char *loader_games_dir(void);

// A loaded game: the memory it lives in and the descriptor to run it with.
typedef struct loader_game loader_game_t;

// Read `path`, check it, put its code in memory and relocate it. Returns NULL and logs
// why on any failure - a bad package must never take the console down with it.
loader_game_t *loader_open(const char *path);

// The descriptor to hand to tat::HostedGame. Valid until loader_close.
const tat_game_t *loader_descriptor(loader_game_t *g);

// Everything the package carries, for api->asset(). The loader owns the memory.
const tat_asset_t *loader_assets(loader_game_t *g, int *count);

// Give back the code, the assets and the mapping.
void loader_close(loader_game_t *g);

#ifdef __cplusplus
}
#endif
