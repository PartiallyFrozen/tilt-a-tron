// The contract between a Tilt-a-tron and a game.
//
// A game includes this header and nothing else from the console. It never calls the OS by
// name: everything arrives through the one `tat_api_t *` handed to begin(). That is what
// lets a game built today keep running on next year's firmware - the only addresses that
// need fixing when it loads are its own.
//
// Plain C, fixed-layout structs, no inline functions. Anything added here is added at the
// END of tat_api_t and nowhere else, and nothing is ever removed: an old game reading a
// grown struct only calls what it already knew about.
//
// See docs/GAME_API.md for how a game becomes an installable file.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TAT_API_MAJOR 1
#define TAT_API_MINOR 0

// ---------------------------------------------------------------- basics

#define TAT_SCREEN 466   // the round display, in device pixels

// A colour in whatever the panel wants. Games never build one by hand - they ask the
// console with api->rgb(), so the packing stays the console's business.
typedef uint16_t tat_color_t;

typedef struct tat_canvas tat_canvas_t;   // opaque
typedef struct tat_sheet tat_sheet_t;     // opaque

enum {
    TAT_BTN_A = 1 << 0,   // BOOT, the small key by the USB port. The console takes this
                          // one for "home", so a game never sees it held.
    TAT_BTN_B = 1 << 1,   // PWR, the game's secondary action
};

typedef struct {
    float ax, ay, az;   // gravity in g, screen-aligned: +x right, +y down
    float gx, gy, gz;   // turn rate, degrees per second
} tat_tilt_t;

typedef struct {
    uint8_t down, pressed, released;
    int32_t x, y;
} tat_touch_t;

typedef struct {
    uint32_t held, pressed, released, clicked, long_press, double_clicked;
    tat_touch_t touch;
    tat_tilt_t tilt;
} tat_input_t;

// Set for exactly one frame when a finger lifts. Swipe left opens the pause menu by
// convention; swipe right means back, everywhere on the console.
typedef struct {
    uint8_t tap, swipe_left, swipe_right;
    int32_t x, y;   // where the gesture started
} tat_gestures_t;

typedef enum { TAT_SQUARE = 0, TAT_TRIANGLE = 1, TAT_NOISE = 2 } tat_wave_t;

typedef struct {
    float f0, f1;        // start and end frequency in Hz; f1 = 0 holds f0
    uint16_t ms;
    uint8_t wave;        // tat_wave_t
    float volume;        // 0..1, relative to the console's volume setting
    uint16_t delay_ms;   // wait before it sounds, for little melodies
} tat_tone_t;

// ---------------------------------------------------------------- the pause menu
//
// Every game's pause screen is the console's, not its own: same title, same rows, same
// RESUME and HOME. A game says what its rows are and is told which one was tapped.

typedef struct {
    const char *label;
    const char *value;
    tat_color_t color;   // 0 uses the console's usual value colour
} tat_menu_row_t;

enum {
    TAT_MENU_NONE = -1,     // nothing happened this frame
    TAT_MENU_CLOSED = -2,   // the player resumed or went home; the menu is already shut
};

// Named colours, so a game's menu matches the console and follows the player's theme.
typedef enum {
    TAT_UI_TEXT = 0,
    TAT_UI_LABEL,
    TAT_UI_DIM,
    TAT_UI_VALUE,
    TAT_UI_ACCENT,
    TAT_UI_GO,
    TAT_UI_DANGER,
} tat_ui_color_t;

// ---------------------------------------------------------------- what a game gets

typedef struct tat_api {
    uint16_t major, minor;

    // ---- console
    tat_color_t (*rgb)(uint8_t r, uint8_t g, uint8_t b);
    void (*go_home)(void);
    void (*log)(const char *fmt, ...);
    int64_t (*now_us)(void);
    uint32_t (*random)(void);

    // ---- input, valid for this frame only
    const tat_input_t *(*input)(void);
    const tat_gestures_t *(*gestures)(void);

    // ---- memory. Everything taken here is given back when the game unloads, so a game
    // that forgets cannot leak across launches.
    void *(*alloc)(size_t bytes);
    void (*free)(void *p);

    // ---- the canvas: a small palette picture the console scales onto the round screen.
    // `scale` is 2 (233x233) or 3 (155x155). `size` 0 fits the screen exactly; a larger
    // one is centred, which gives canvas_present_rotated something to show in the corners.
    tat_canvas_t *(*canvas_create)(int scale, int size);
    void (*canvas_destroy)(tat_canvas_t *c);
    int (*canvas_width)(tat_canvas_t *c);
    uint8_t *(*canvas_pixels)(tat_canvas_t *c);

    // Palette. color() hands back the index for a colour, making a new entry the first
    // time. Index 0 is transparent in sprites and draws black.
    uint8_t (*canvas_color)(tat_canvas_t *c, tat_color_t rgb);
    uint8_t (*canvas_reserve)(tat_canvas_t *c, int n);   // entries the game will set itself
    void (*canvas_set_color)(tat_canvas_t *c, uint8_t i, tat_color_t rgb);

    void (*canvas_clear)(tat_canvas_t *c, uint8_t i);
    void (*canvas_pixel)(tat_canvas_t *c, int x, int y, uint8_t i);
    void (*canvas_fill_rect)(tat_canvas_t *c, int x, int y, int w, int h, uint8_t i);
    void (*canvas_rect)(tat_canvas_t *c, int x, int y, int w, int h, uint8_t i);
    void (*canvas_line)(tat_canvas_t *c, int x0, int y0, int x1, int y1, uint8_t i);
    void (*canvas_fill_circle)(tat_canvas_t *c, int cx, int cy, int r, uint8_t i);
    int (*canvas_text)(tat_canvas_t *c, int x, int y, const char *s, uint8_t i, int scale, bool bold);
    void (*canvas_text_centered)(tat_canvas_t *c, int cx, int cy, const char *s, uint8_t i, int scale, bool bold);

    // Sprite sheets are PNGs of frames side by side; alpha below half is transparent.
    tat_sheet_t *(*sheet_load)(tat_canvas_t *c, const void *png, size_t len, int fw, int fh);
    void (*canvas_sprite)(tat_canvas_t *c, tat_sheet_t *s, int frame, int x, int y, bool flip_x);
    void (*canvas_sprite_scaled)(tat_canvas_t *c, tat_sheet_t *s, int frame, float x, float y,
                                 float sx, float sy, bool flip_x);

    // Put the canvas on the screen. Rotated turns it about the centre, which is how a
    // game keeps the world upright while the watch is turned.
    void (*canvas_present)(tat_canvas_t *c);
    void (*canvas_present_rotated)(tat_canvas_t *c, float radians);

    // ---- the pause menu (see above)
    void (*menu_open)(void);
    void (*menu_close)(void);
    bool (*menu_is_open)(void);
    void (*menu_invalidate)(void);
    int (*menu_update)(void);   // a row index, or TAT_MENU_NONE / TAT_MENU_CLOSED
    void (*menu_draw)(const tat_menu_row_t *rows, int count, const char *title);
    tat_menu_row_t (*menu_sound_row)(void);
    void (*menu_toggle_sound)(void);
    tat_color_t (*ui_color)(int which);   // tat_ui_color_t

    // ---- sound
    void (*tone)(const tat_tone_t *t);
    int (*volume)(void);   // 0 off .. 3 high, the console's setting

    // ---- saved settings and scores, in this game's own namespace. Erased with the game.
    // get() leaves the value alone if nothing is stored, so the caller's initialiser is
    // the default. `limit` > 0 also rejects anything outside 0..limit-1.
    void (*save_get)(const char *key, int *value, int limit);
    void (*save_set)(const char *key, int value);

    // ---- the game's own files, by the name they were packaged under
    const void *(*asset)(const char *name, size_t *len);
} tat_api_t;

// ---------------------------------------------------------------- what a game exports

#define TAT_GAME_MAGIC 0x47544154u   // 'TATG'

// A file the game carries: a sprite sheet, a level, anything. Built into the firmware for
// a built-in game; unpacked from the .tat for an installed one. Same to the game either way.
typedef struct {
    const char *name;
    const uint8_t *data;
    uint32_t len;
} tat_asset_t;

typedef struct {
    uint32_t magic;
    uint16_t api_major, api_minor;

    const char *id;      // "pindrop" - lowercase; also the save namespace
    const char *name;    // "PIN DROP" - what the carousel shows
    tat_color_t accent;

    const tat_asset_t *assets;
    int asset_count;

    void (*begin)(const tat_api_t *api);   // once, after loading. Keep the pointer.
    void (*enter)(void);                   // becoming the active app; the screen is black
    void (*update)(float dt);              // once per frame
    void (*draw)(void);                    // once per frame, after update
    void (*leave)(void);                   // another app took over, or the watch is sleeping
    void (*unload)(void);                  // being removed from memory; let go of things
    // Optional. A game that only draws when something changes gets this when the console
    // needs a frame anyway - a screenshot over Wi-Fi, or the screen coming back on.
    void (*redraw)(void);
    bool (*keep_awake)(void);              // optional: true while the player is mid-action
} tat_game_t;

// Every game defines exactly one of these, named tat_game.
extern const tat_game_t tat_game;

#ifdef __cplusplus
}
#endif
