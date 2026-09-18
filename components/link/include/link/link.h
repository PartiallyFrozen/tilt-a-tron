// The USB link: how the Tilt-a-tron manager app on a computer talks to the watch.
//
// It runs over the built-in USB-Serial/JTAG port - the same one that carries the log and
// does the flashing - so nothing has to be switched on the watch and the FAT drive is never
// mounted. Frames are found by a sync word and checked with a CRC, so log text and protocol
// traffic can share the wire; while a session is open the log is held back anyway.
//
//   A5 5A | len u16 | seq u8 | cmd u8 | payload[len] | crc16      (CRC over seq, cmd, payload)
//
// A reply carries the same seq, with cmd | 0x80 for success or LINK_ERR for a refusal whose
// payload is a readable reason. See docs/GAME_API.md section 6.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LINK_PROTO_VERSION 1
#define LINK_MAX_PAYLOAD 4096

enum {
    LINK_HELLO = 0x01,   // -> nothing;  <- proto, api version, firmware, board
    LINK_INFO = 0x02,    // -> nothing;  <- bytes for games: total, free, installed count
    LINK_LIST = 0x03,    // -> nothing;  <- count, then one link_game_t per game
    LINK_ICON = 0x04,    // -> id[16];   <- that game's icon PNG

    // Files on the watch's storage (themes today, anything later). Paths are relative to
    // the storage root, '/' separated; "..", absolute paths and backslashes are refused.
    LINK_FS_FREE = 0x10,    // -> nothing;              <- total u32, free u32
    LINK_FS_LIST = 0x11,    // -> path;                 <- entries: u8 is_dir, u32 size, name\0
    LINK_FS_PUT = 0x12,     // -> size u32, crc32 u32, path;  then FS_DATA frames, then FS_END
    LINK_FS_DATA = 0x13,    // -> chunk
    LINK_FS_END = 0x14,     // -> nothing;              <- ok once the CRC matches
    LINK_FS_GET = 0x15,     // -> path;                 <- the file, in one frame
    LINK_FS_DELETE = 0x16,  // -> path (a file, or a directory and everything under it)
    LINK_FS_MKDIR = 0x17,   // -> path

    LINK_ERR = 0xFF,
};

enum {
    LINK_GAME_BUILTIN = 1 << 0,   // part of the firmware: can be hidden, never uninstalled
    LINK_GAME_HIDDEN = 1 << 1,    // taken off the home screen in Settings > GAMES
};

// Sent over the wire: fixed layout, little-endian, no padding.
typedef struct __attribute__((packed)) {
    char id[16];
    char name[24];
    uint16_t accent;   // RGB565
    uint8_t flags;
    uint8_t reserved;
    uint32_t bytes;    // space it occupies, 0 for built-ins
} link_game_t;

// Starts the reader task. Safe to call once; does nothing without the hooks below.
void link_start(void);

// What the console tells the app about itself.
void link_set_info_hook(void (*fn)(uint32_t *total, uint32_t *free_bytes, uint8_t *count));
void link_set_list_hook(int (*fn)(link_game_t *out, int max));   // returns how many it wrote
void link_set_icon_hook(bool (*fn)(const char *id, const uint8_t **png, size_t *len));

// Where FS_* paths resolve, e.g. "/data". Without it the file commands are refused.
void link_set_fs_root(const char *root);

// True while the app is connected, so the watch stays awake and holds off its log.
bool link_session_active(void);

#ifdef __cplusplus
}
#endif
