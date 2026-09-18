// Doom on the Tilt-a-tron.
//
// The Doom engine (id Software's GPL source by way of Chocolate Doom and doomgeneric,
// in ../../src) runs in its own task with all of its memory in PSRAM. The game data
// (a WAD file, e.g. the shareware DOOM1.WAD) sits in a spare region of the flash and is
// memory-mapped, so its graphics and sounds are used in place and cost no RAM.
//
// The console side (components/games/doom.cpp) feeds it analog controls and shows the
// latest finished frame, rotated to stay upright, at the display's own rate.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DOOM_W 320
#define DOOM_H 200

// ---- the WAD store (a region of the main flash; see doom_wad.c)
typedef struct {
    bool present;
    char name[32];     // the file it came from, e.g. "doom1.wad" (Doom tells the games apart by name)
    uint32_t length;
} doom_wad_info_t;

bool doom_wad_info(doom_wad_info_t *out);
// Writing a new WAD: begin, write the bytes in order, end. Nothing may be running.
bool doom_wad_write_begin(const char *name, uint32_t length);
bool doom_wad_write(const void *data, size_t len);
bool doom_wad_write_end(void);
// Copy a WAD file from the drive into the store. progress(0..100) may be NULL.
bool doom_wad_install_file(const char *path, void (*progress)(int percent));

// ---- the engine
typedef enum {
    DOOM_LOADING,        // starting up
    DOOM_LEVEL,          // playing
    DOOM_INTERMISSION,   // the tally between levels
    DOOM_FINALE,         // end-of-episode text
    DOOM_TITLE,          // title / demo loop (after the episode ends)
    DOOM_FAILED,         // see doom_port_error()
} doom_state_t;

typedef struct {
    doom_state_t state;
    bool dead;
    int health, armor, ammo;   // ammo < 0: the weapon doesn't use any
    int episode, map;
    uint32_t frames;           // frames finished so far
} doom_status_t;

// Starts the engine task (once). skill 0..4 = "I'm too young to die" .. "Nightmare".
bool doom_port_start(int skill);
bool doom_port_started(void);
// The engine only runs, and its clock only ticks, while active.
void doom_port_set_active(bool active);
void doom_port_new_game(int skill);

// Controls, sampled once per game tic (35 Hz).
//   forward: -50..50 (25 = walk, 50 = run)   turn: angle units per tic, + = left (1280 = fast key turn)
void doom_port_controls(int forward, int turn, bool fire, bool use);
void doom_port_next_weapon(void);
void doom_port_set_sfx_volume(int vol_0_15);

void doom_port_status(doom_status_t *out);
const char *doom_port_error(void);
// Latest finished frame: DOOM_W x DOOM_H palette indices, and its palette as 256 x RGB.
const uint8_t *doom_port_frame(void);
const uint8_t *doom_port_palette(uint32_t *generation);

#ifdef __cplusplus
}
#endif
