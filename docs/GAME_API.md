# Tilt-a-tron OS and games — design spec

**Status:** design, nothing implemented. Written 2026-09-18.

The console splits into two things that are built, shipped and versioned separately.

**The OS** is the firmware: drivers, engine, canvas, launcher, settings, themes, Wi-Fi,
updates, and the loader that runs games. It lives in the two OTA slots and rolls back if a
build won't boot. It is the only thing that has to be protected, because it is the only
thing that can't be reinstalled from a file.

**A game** is a file you put on the watch and take off again. Code, icon and assets in one
package. Installing one doesn't touch the firmware; uninstalling one leaves no trace but the
free space.

Everything below is the contract between them.

---

## 1. What ships in the OS

Built in, always present:

- **Pocket Watch** — it's a watch first; telling the time can't depend on an installed file.
- **Breakout** — so a fresh install is playable before you copy anything.

Both are still hidden from the home screen through Settings > GAMES, but they can't be
uninstalled. Every other game (Marble Maze, Grand Prix, Sky Jump, Tilt-a-tris, Sleepy Star,
and anything new) becomes a package.

Built-in games use the same ABI as packaged ones, compiled in rather than loaded. That keeps
one code path and means a built-in can be moved out to a package later without a rewrite.

---

## 2. Package format (`.tat`)

One file per game. Little-endian throughout.

```
offset  size  field
0       8     magic        "TATPKG\0" + format version byte (currently 1)
8       4     header_crc32 CRC-32 of bytes 12..header_end
12      4     total_size   whole file, bytes
16      2     api_major    the API this game was built against
18      2     api_minor
20      16    id           "skyjump" — lowercase, [a-z0-9_], NUL-padded; the save namespace
36      24    name         "SKY JUMP" — what the launcher shows
60      2     accent       RGB565, the carousel's title colour
62      2     section_count
64      ...   sections[section_count], 16 bytes each:
                  type u32, offset u32 (from file start), size u32, crc32 u32
```

Section types:

| Type | Meaning |
|---|---|
| `CODE` | The executable image. Exactly one. Format decided by §5. |
| `ICON` | 210×210 PNG with alpha, the carousel icon. Exactly one. |
| `ASSET` | `name[16]` then the bytes. Sprite sheet PNGs, levels, anything the game asks for by name. |
| `DATA` | One large opaque blob, reached as a read-only stream rather than a pointer. For game data too big to hold in memory. |
| `SHOT` | Optional 233×233 PNG, a screenshot for the manager app and the store later. |

The installer checks `header_crc32`, then every section's CRC, then the API version, **before**
anything is written to the registry. A half-written package is never installable.

---

## 3. The ABI

C, not C++. Fixed-layout POD structs, explicit widths, no inline functions, no vtables across
the boundary. This is what lets a game built today keep running on next year's OS.

### 3.1 What a game exports

One symbol, at a fixed name:

```c
typedef struct {
    uint32_t    magic;          /* 'TATG' */
    uint16_t    api_major, api_minor;

    void (*begin)(const tat_api_t *api);  /* once, after loading. Store `api`. */
    void (*enter)(void);                  /* becoming the active app; screen is black */
    void (*update)(float dt);             /* once per frame */
    void (*draw)(void);                   /* once per frame, after update */
    void (*leave)(void);                  /* another app took over, or the watch is sleeping */
    void (*unload)(void);                 /* being removed from memory; release anything */
    bool (*keep_awake)(void);             /* optional, may be NULL */
} tat_game_t;

extern const tat_game_t tat_game;
```

`begin` is the only place the game receives the API table. It stores the pointer; the OS
guarantees it stays valid until `unload`.

### 3.2 What the OS provides

```c
#define TAT_API_MAJOR 1
#define TAT_API_MINOR 0

typedef uint16_t tat_color_t;              /* RGB565, panel byte order */
typedef struct tat_canvas tat_canvas_t;    /* opaque */
typedef struct tat_sheet  tat_sheet_t;     /* opaque */

typedef struct { float ax, ay, az;         /* g, screen axes: +x right, +y down */
                 float gx, gy, gz; } tat_tilt_t;        /* deg/s */
typedef struct { uint8_t down, pressed, released;
                 int32_t x, y; int64_t t_us; } tat_touch_t;
typedef struct { uint32_t held, pressed, released, clicked, long_press, double_clicked;
                 tat_touch_t touch; tat_tilt_t tilt; } tat_input_t;
typedef struct { uint8_t tap, swipe_left, swipe_right;
                 int32_t x, y; } tat_gestures_t;
typedef struct { float f0, f1; uint16_t ms; uint8_t wave;
                 float volume; uint16_t delay_ms; } tat_tone_t;

typedef struct {
    uint16_t major, minor;

    /* ---- console */
    void     (*go_home)(void);
    void     (*request_sleep)(void);
    void     (*log)(const char *fmt, ...);
    int64_t  (*now_us)(void);

    /* ---- input (valid for this frame only) */
    const tat_input_t    *(*input)(void);
    const tat_gestures_t *(*gestures)(void);

    /* ---- memory: everything here is freed automatically on unload */
    void *(*alloc)(size_t bytes);
    void  (*free)(void *p);

    /* ---- canvas: the palette picture every game draws on */
    tat_canvas_t *(*canvas_create)(int scale, int size);
    void     (*canvas_destroy)(tat_canvas_t *c);
    uint8_t *(*canvas_pixels)(tat_canvas_t *c);
    int      (*canvas_width)(tat_canvas_t *c);
    int      (*canvas_height)(tat_canvas_t *c);
    uint8_t  (*canvas_color)(tat_canvas_t *c, tat_color_t rgb);
    uint8_t  (*canvas_reserve)(tat_canvas_t *c, int n);
    void     (*canvas_set_color)(tat_canvas_t *c, uint8_t i, tat_color_t rgb);
    void     (*canvas_set_palette)(tat_canvas_t *c, const tat_color_t *pal, int n);
    void     (*canvas_clear)(tat_canvas_t *c, uint8_t i);
    void     (*canvas_pixel)(tat_canvas_t *c, int x, int y, uint8_t i);
    void     (*canvas_fill_rect)(tat_canvas_t *c, int x, int y, int w, int h, uint8_t i);
    void     (*canvas_rect)(tat_canvas_t *c, int x, int y, int w, int h, uint8_t i);
    void     (*canvas_line)(tat_canvas_t *c, int x0, int y0, int x1, int y1, uint8_t i);
    void     (*canvas_fill_circle)(tat_canvas_t *c, int cx, int cy, int r, uint8_t i);
    int      (*canvas_text)(tat_canvas_t *c, int x, int y, const char *s, uint8_t i,
                            int scale, bool bold);
    void     (*canvas_text_centered)(tat_canvas_t *c, int cx, int cy, const char *s,
                                     uint8_t i, int scale, bool bold);
    tat_sheet_t *(*sheet_load)(tat_canvas_t *c, const void *png, size_t len, int fw, int fh);
    void     (*sheet_free)(tat_sheet_t *s);
    int      (*sheet_frames)(tat_sheet_t *s);
    void     (*canvas_sprite)(tat_canvas_t *c, tat_sheet_t *s, int frame, int x, int y,
                              bool flip_x);
    void     (*canvas_sprite_scaled)(tat_canvas_t *c, tat_sheet_t *s, int frame, float x,
                                     float y, float sx, float sy, bool flip_x);
    void     (*canvas_present)(tat_canvas_t *c);
    void     (*canvas_present_rotated)(tat_canvas_t *c, float radians);

    /* ---- framebuffer, for menus and anything full-resolution */
    void (*gfx_clear)(tat_color_t c);
    void (*gfx_fill_rect)(int x, int y, int w, int h, tat_color_t c);
    void (*gfx_rect)(int x, int y, int w, int h, tat_color_t c);
    void (*gfx_fill_circle)(int cx, int cy, int r, tat_color_t c);
    int  (*gfx_text)(int x, int y, const char *s, tat_color_t c, int scale, bool bold);
    void (*gfx_text_centered)(int cx, int cy, const char *s, tat_color_t c, int scale,
                              bool bold);
    int  (*gfx_text_width)(const char *s, int scale, bool bold);

    /* ---- the shared console look, so every game's menu matches (README "Shared console UX") */
    void (*ui_clear_screen)(void);           /* the theme's background */
    void (*ui_title)(const char *s);
    void (*ui_hint)(int y, const char *s);
    void (*ui_row)(int i, const char *label, const char *value, tat_color_t value_color);
    void (*ui_button)(int i, int of, const char *label, bool filled);
    bool (*ui_row_hit)(int i, int x, int y);
    bool (*ui_button_hit)(int i, int of, int x, int y);
    tat_color_t (*ui_color)(int which);      /* TAT_UI_TEXT, _DIM, _ACCENT, _GO, _DANGER... */

    /* ---- sound */
    void (*tone)(const tat_tone_t *t);
    void (*pcm_start)(int channel, const uint8_t *samples, int len, int rate, float volume);
    void (*pcm_stop)(int channel);
    int  (*volume)(void);                    /* 0..3, the console setting */

    /* ---- saves: this game's own namespace, erased when it's uninstalled */
    bool (*save_get)(const char *key, void *buf, size_t *len);
    bool (*save_set)(const char *key, const void *buf, size_t len);

    /* ---- the package's own contents */
    const void *(*asset)(const char *name, size_t *len);   /* ASSET sections, in place */
    bool (*data_read)(uint32_t offset, void *buf, size_t len);  /* the DATA section */
    uint32_t (*data_size)(void);
} tat_api_t;
```

Notes that matter:

- **Rotation stays in the API.** `canvas_present_rotated` is how Grand Prix and Tilt-a-tris
  keep the world upright; it's not something a game should reimplement.
- **`ui_*` is deliberately narrow.** Games get the shared row/button/title widgets, not a
  general layout engine, so every pause menu keeps looking the same.
- **`asset` returns a pointer, `data_read` copies.** Small things (sprite sheets) are mapped
  and used in place; one big blob is streamed, because it may be larger than memory.
- **No file system.** Games can't read the drive. Saves go through `save_*`, content comes
  from the package. That's what makes uninstall complete.

### 3.3 Versioning

`major` changes when something is removed or its meaning changes. `minor` changes when
functions are **appended to the end** of the struct and nowhere else.

The OS runs a game when `game.api_major == os.api_major && game.api_minor <= os.api_minor`.
Otherwise the launcher shows the icon greyed with "NEEDS A NEWER TILT-A-TRON" (or "BUILT FOR
AN OLDER TILT-A-TRON") and offers uninstall. It never loads a game it can't satisfy.

---

## 4. Flash layout

Today's map spends 8 MB on two 4 MB app slots because the firmware contains every game. With
an OS-only image (~1.2 MB), that drops hard:

| Partition | Size | Holds |
|---|---|---|
| `nvs` | 24 K | settings, saves, the game registry |
| `otadata` | 8 K | which OS slot to boot |
| `app0` / `app1` | 1.5 MB each | the OS, A/B for rollback |
| `games` | 12 MB | installed packages, code and assets together |
| `storage` | 14 MB | the FAT drive: themes, and where files are dropped |
| `coredump` | 64 K | crash dumps |

Changing the partition table needs one USB reflash — it can't be done safely over the air.
That's a one-time cost, and the browser installer already covers it.

**Games region.** Packages are packed head to tail with a free-list; a slot is 64 KB-aligned
so a package's code sits on an MMU page boundary. Uninstalling marks the slot free;
installing takes the first hole big enough, and compacts only when it has to (a compaction
rewrites flash, so it asks first and shows progress).

**Registry** (in NVS, one blob per installed game): id, name, accent, API version, package
offset and size, section offsets, install date, bytes used. The launcher builds the carousel
from the registry alone — no game code is loaded until you tap its icon.

---

## 5. Loading the code — the open question

Everything above is ordinary engineering. This part is the risk, and it's honest to say so:
the ESP32-S3 has no dynamic linker, and the Xtensa toolchain doesn't produce
position-independent code the way a desktop does.

Two candidate mechanisms:

**A. Relocatable ELF, resolved at install time.** The game is built as an ELF with
relocations. At install the OS (or the build tool) fixes it to the flash address of its slot
and writes the fixed-up image. At launch the code runs straight from flash through the
instruction cache, exactly like the firmware does — full speed, no RAM cost for code. The
cost is a relocation pass and a rewrite on compaction.

**B. Load into PSRAM and execute there.** The loader copies the code into PSRAM and maps
those pages as executable. Simpler slot management, but it spends PSRAM on every running
game and depends on instruction fetch from PSRAM, which interacts with how the firmware
itself is configured.

**A is preferred** — code stays in flash, memory stays for the game. The relocation set
needed for Xtensa is small and known.

Either way this must be proved before any game is converted: build a throwaway "bouncing
ball" game as a separate file, install it, run it, confirm it draws and reads input. If
neither mechanism works, the whole design falls back to games being separate OTA images,
which is worse in every way and worth knowing early.

Whatever wins, the game side doesn't change: the same source builds either as a package or
compiled into the OS.

---

## 6. Talking to the watch over USB

With **USB DRIVE off** (the default) the port is the built-in USB serial that already carries
the log. The manager app speaks a framed protocol over the same port, so nothing has to be
switched on the watch and the FAT drive is never mounted — which is what caused the theme
loss before. Plug in and it works.

Frame:

```
A5 5A | len u16 | seq u8 | cmd u8 | payload[len] | crc16
```

The watch ignores anything that isn't a valid frame, so log output and protocol traffic can
share the wire. While a session is open, the OS holds off log writes to reduce noise, and
refuses to sleep.

| Command | Payload | Reply |
|---|---|---|
| `HELLO` | app version | OS version, API major/minor, board id |
| `INFO` | — | games region size, free bytes, installed count |
| `LIST` | — | for each game: id, name, size, API version, install date |
| `ICON` | id | the icon PNG |
| `INSTALL_BEGIN` | id, total size, CRC | OK, or why not (no room, bad API version) |
| `INSTALL_DATA` | seq, chunk | OK |
| `INSTALL_END` | — | OK once CRC checks and the registry entry is written |
| `REMOVE` | id | OK, bytes freed |
| `COMPACT` | — | progress reports |
| `REBOOT` | — | — |

A game is never partially installed: the registry entry is the last write.

---

## 7. The manager app

**Recommendation: a page on the existing site, using Web Serial** — the same mechanism as the
browser installer that's already live. Nothing to download, works on Windows and macOS in
Chrome or Edge, and updates for everyone when the site updates. It shows what's installed,
what's free, and a catalogue of games to add; drag a `.tat` file onto it to install one that
isn't in the catalogue.

A native app can come later if it's ever needed. `tools/tatpkg.py` covers the command line
for development (build, install, remove, list) and is what CI would use.

---

## 8. Uninstalling on the watch

What was asked for, and it works only on packaged games:

1. Press and hold an icon on the home carousel. After ~600 ms it lifts and wobbles.
2. A panel: the game's name, the space it frees, and a plain warning that its saves go too.
3. **DELETE** / **CANCEL**. Nothing else on the panel.
4. The carousel closes the gap, and the space is free immediately.

Holding a built-in game's icon says it's part of the watch and offers Settings > GAMES
instead. Settings > GAMES keeps working as it does now for hiding without deleting.

---

## 9. Safety

- **A bad game can't brick the watch.** The OS writes "launching <id>" before the first call
  into a game and clears it on a clean exit. Booting with that mark still set means the game
  crashed: it's quarantined, the launcher says so and offers uninstall.
- **The OS never loads a game it can't satisfy** (§3.3).
- **Install is checked end to end** (§2) and the registry entry written last.
- **Memory is accounted per game.** `alloc`/`free` and every canvas and sheet the game
  created are released at `unload`, so leaks can't accumulate across launches.
- **No signing for now.** A `SIG` section is reserved so it can be added without a format
  break, when packages start arriving over the network rather than by USB.

---

## 10. Order of work

Each phase leaves a working watch.

1. **Spike the loader** (§5). Throwaway. Decides A or B, or kills the design.
2. **Write the OS side of the ABI** against the built-in games — no loading yet. Breakout and
   the Clock move to `tat_game_t` while still compiled in. Proves the API is sufficient
   before anything depends on it.
3. **Convert Sky Jump to a package** end to end: build tool, install over USB, run, uninstall.
   Launcher handles built-in and installed games side by side.
4. **Convert the rest** — Marble Maze, Grand Prix, Tilt-a-tris, Sleepy Star — one at a time.
5. **Repartition** (one USB reflash), shrink the OS image, ship the OS and a starter set of
   games from the installer.
6. **Hold-to-uninstall** and the manager page.

Doom sits on the `doom-port` branch. It's a good test case for a package with a large `DATA`
section, and can come back once the loader is real.

---

## 11. Still open

- Can a game ship more than one screen's worth of assets without a `DATA` section? (Probably:
  `ASSET` count isn't limited, and they're read in place.)
- Should games be able to call each other, or hand off? (No, for now.)
- Do packages need to declare the hardware they use (IMU, sound), so the OS can warn on a
  future board without one?
- Where does a game's high score go when it's uninstalled and reinstalled — gone, or kept
  briefly? (Spec says gone, and the panel says so. Worth revisiting once it's real.)
