# Tilt-a-tron OS, packages and the manager app — design spec

**Status:** design. The USB link is built and proven on hardware; everything else is a plan.
Revised 2026-09-18.

The console splits into two things that are built, shipped and versioned separately.

**The OS** is the firmware: drivers, engine, canvas, launcher, settings, Wi-Fi, updates, the
USB link and the game loader. It lives in the two OTA slots and rolls back if a build won't
boot. It is the only thing that has to be protected, because it is the only thing that can't
be reinstalled from a file.

**Packages** are single files you put on the watch and take off again. Three kinds — games,
themes and watch faces — in one container format, so there is one thing to build, one thing
to send, and one thing to share. A package is self-contained: it can be emailed, posted or
handed to a friend, and it works.

Everything below is the contract between them.

---

## 1. What ships in the OS

Built in, always present:

- **Pocket Watch** — it's a watch first; telling the time can't depend on an installed file.
- **Breakout** — so a fresh install is playable before you copy anything.

Both can still be hidden from the home screen through Settings > GAMES, but not uninstalled.
Everything else (Marble Maze, Grand Prix, Sky Jump, Tilt-a-tris, Sleepy Star, and anything
new) becomes a package.

Built-in games use the same ABI as packaged ones, compiled in rather than loaded. One code
path, and a built-in can move out to a package later without a rewrite.

---

## 2. The package — one file, like an APK

`.tat` for all three kinds. Little-endian throughout.

```
offset  size  field
0       8     magic        "TATPKG\0" + format version byte (currently 1)
8       4     header_crc32 CRC-32 of bytes 12..end of the section table
12      4     total_size   whole file, bytes
16      1     kind         1 GAME, 2 THEME, 3 FACE
17      1     reserved
18      2     api_major    for GAME: the game API it was built against. 0 otherwise
20      2     api_minor
22      2     version      the package's own version, for "you have v1, this is v2"
24      16    id           "skyjump" — lowercase [a-z0-9_]; a game's save namespace
40      24    name         "SKY JUMP" — what the launcher and the app show
64      24    author       shown in the app; packages get shared, so credit travels with them
88      2     accent       RGB565
90      2     section_count
92      ...   sections[], 16 bytes each: type u32, offset u32, size u32, crc32 u32
```

| Section | Used by | Meaning |
|---|---|---|
| `CODE` | GAME | The executable image. Format decided by §6. |
| `ICON` | GAME | 210×210 PNG with alpha, the carousel icon. |
| `ASSET` | any | `name[16]` then bytes. Sprite sheets, backgrounds, face art, `theme.json`, `face.json`. |
| `DATA` | GAME | One large opaque blob, read as a stream rather than a pointer, for data too big to hold in memory. |
| `SHOT` | any | 233×233 PNG preview. The app shows it **before** installing — the thing you most want when someone sends you a file. |
| `SIG` | any | Reserved. Not used yet; here so signing can arrive without a format break. |

The installer checks `header_crc32`, then every section's CRC, then (for games) the API
version, **before** anything is committed. A half-written package is never installable.

**Kinds at install time.** A GAME goes into the games region and is registered. A THEME or
FACE is unpacked into storage as a folder named after its `id`, alongside a small record of
which package put it there — so uninstalling removes exactly what it added and nothing the
user made themselves.

---

## 3. Watch faces are data, not code

A face is a background, some art, and a few things that move. That doesn't need a
programming language, and making it data has three payoffs: faces work **before** the code
loader exists, they can't crash the watch, and anybody with an image editor and a text
editor can make one.

`face.json` inside the package:

```json
{
  "name": "Neon Rings",
  "background": "bg.png",
  "elements": [
    { "type": "hand",  "image": "hour.png",   "source": "hour",   "pivot": [16, 84] },
    { "type": "hand",  "image": "minute.png", "source": "minute", "pivot": [12, 128] },
    { "type": "text",  "format": "%H:%M", "at": [233, 300], "size": 6,
      "color": "#ffd93d", "align": "center" },
    { "type": "text",  "format": "%a %d", "at": [233, 352], "size": 2, "color": "#8a97c0" },
    { "type": "arc",   "source": "battery", "at": [233, 233], "radius": 214, "width": 8,
      "from": -90, "to": 270, "color": "#28c86e" },
    { "type": "image", "image": "pip.png", "at": [233, 44] }
  ]
}
```

- Coordinates are in screen pixels, 466×466, origin top-left; `[233, 233]` is the centre.
- `hand` rotates its image about `pivot` around `at` (default centre) by its source.
- `source`: `hour`, `minute`, `second`, `battery`, `charging`, `date`.
- `format` is strftime, so a face can show whatever mix of time and date it likes.
- Anything the renderer doesn't understand is skipped, so an old OS shows a newer face
  imperfectly rather than not at all.

The five faces in the firmware today stay as code — they're the fallback when nothing is
installed, and the reference for what the format has to be able to express.

---

## 4. The game ABI

C, not C++. Fixed-layout POD structs, explicit widths, no inline functions, no vtables across
the boundary. This is what lets a game built today keep running on next year's OS.

### 4.1 What a game exports

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
    void (*redraw)(void);                 /* optional: the console needs a frame anyway */
    bool (*keep_awake)(void);             /* optional, may be NULL */
} tat_game_t;

extern const tat_game_t tat_game;
/* A game with files of its own also declares this and points the descriptor at it. */
extern const tat_asset_t tat_assets[];
```

`begin` is the only place the game receives the API table. It stores the pointer; the OS
guarantees it stays valid until `unload`.

### 4.2 What the OS provides

**The header is real now:** `components/tat_api/include/tat/tat_api.h`, with the console's
side in `tat_host.cpp`. PIN DROP and SKY JUMP are written against it and nothing else.
Every change below was found by writing one of them, not by design review — which is the
argument for porting real games early rather than freezing the API on paper.

From PIN DROP (API 1.0):

- **`rgb(r, g, b)` is an API call, not a macro.** The panel stores RGB565 byte-swapped, and
  a macro baked that into the game - every colour came out wrong. How a pixel is packed is
  the console's business, so a game asks for a colour rather than building one.
- **The pause menu is exposed whole** (`menu_open`, `menu_update`, `menu_draw`, ...) rather
  than as row and button primitives. Primitives would have every game reinventing the pause
  screen, which is what was just removed from all seven.
- **`redraw` was added to the game descriptor**, for games that only draw when something
  changed. Without it a screenshot over Wi-Fi catches a black frame.

From SKY JUMP (API 1.1):

- **`canvas_banner()` was added.** The panel a game drops over the play area to say GAME
  OVER lived inside the games component, out of a package's reach, so every packaged game
  would have rolled its own. It now sits beside the pause menu in the console, for the same
  reason the pause menu does.
- **A descriptor's accent colour is three plain bytes**, not a `tat_color_t`. A descriptor
  exists before the game has an api pointer to call `rgb()` with, so the old field asked
  games to hand-pack the panel's byte order — the one thing `rgb()` exists to prevent.
- **`tat_asset_t` holds an end pointer, not a length.** The difference of two linker symbols
  is not a constant expression in C, so with a length a built-in game could not write its
  own asset table down. (The C++ version got away with it; C does not.)
- **Assets come from a table the build supplies**, declared as `extern const tat_asset_t
  tat_assets[]`. Linked into the firmware for a built-in game, supplied by the loader for an
  installed one; the game asks `api->asset()` by name and never learns which.

The sketch that follows is kept for the shape of the thing; the header is the truth.

```c
#define TAT_API_MAJOR 1
#define TAT_API_MINOR 1

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

    /* ---- the shared console look, so every game's menu matches */
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
    const void *(*asset)(const char *name, size_t *len);        /* ASSET sections, in place */
    bool (*data_read)(uint32_t offset, void *buf, size_t len);  /* the DATA section */
    uint32_t (*data_size)(void);
} tat_api_t;
```

Notes that matter:

- **Rotation stays in the API.** `canvas_present_rotated` is how Grand Prix and Tilt-a-tris
  keep the world upright; not something a game should reimplement.
- **`ui_*` is deliberately narrow.** Games get the shared row/button/title widgets, not a
  layout engine, so every pause menu keeps looking the same.
- **`asset` returns a pointer, `data_read` copies.** Small things are used in place; one big
  blob is streamed, because it may be larger than memory.
- **No file system.** Games can't read storage. Saves go through `save_*`, content comes from
  the package. That is what makes uninstall complete, and what makes a shared game safe to
  run.

### 4.3 Porting a game to the API

What the first two conversions came down to, so the rest come out the same shape. A ported
game is one `.c` file plus, if it ships files, a `_builtin.c` naming them.

| C++ it used | What it becomes |
|---|---|
| `wc::Canvas canvas; canvas.init(s)` | `T->canvas_create(s, 0)`, kept in the game's own struct |
| `c.fillCircle(...)`, `c.textCentered(...)` | the matching `T->canvas_*` call, canvas first |
| `Gfx::W`, a hardcoded canvas width | `T->canvas_width(cv)` — ask, don't assume |
| `wc::Store s("id"); s.get(k, v)` | `T->save_get(k, &v, limit)`; the namespace is the descriptor's `id` |
| `wc::audio::play({...})` | `tat_tone_t` + `T->tone(&t)`, one call per tone |
| `console::ui::PauseMenu menu` | `T->menu_open/is_open/update/draw/...` |
| `games::ui::banner(...)` | `tat_banner_t` + `T->canvas_banner(...)` |
| `Gestures ges; ges.update(...)` | `T->gestures()` — the host already did it |
| `esp_random()`, `esp_timer_get_time()` | `T->random()`, `T->now_us()` |
| `ESP_LOGI(TAG, ...)` | `T->log(...)` — the tag is the game's `id` |
| `e.presenter()`, `e.input()`, `e.goHome()` | `T->canvas_present(cv)`, `T->input()`, `T->go_home()` |
| `_binary_x_png_start` in the game | a `tat_assets[]` table in `_builtin.c`; the game calls `T->asset("x.png", &len)` |
| `std::vector<Thing> things` | `Thing things[MAX]; int n_things;` and swap-with-last removal |
| a big array as a plain member | `T->alloc()` in `begin`, `T->free()` in `unload` |

Four things that bite every time:

- **Large state is not free because it is static.** Grand Prix's track came to 20 KB of
  plain members, which land in internal RAM — the scarce kind, and the kind Wi-Fi needs.
  The free internal heap went from 48 KB to 23 KB and the watch stopped answering. Anything
  measured in kilobytes belongs in `alloc()`, which is what a package would have to use.

- **Removing from a fixed array.** `a[i] = a[--n]` moves the last element into the hole, so
  the loop must **not** advance `i` afterwards — or must `break` immediately. Both ports had
  one of each.
- **Filling up.** Decide what happens when the array is full *before* it is, and prefer
  stopping cleanly to dropping the newest thing on the floor.
- **Anything `const` and file-scope must be a constant expression in C.** C++ will fold
  things C will not — the difference of two linker symbols being the one that caught us.
- **Check the old save namespace before choosing an `id`.** The descriptor's `id` *is* the
  save namespace. Sleepy Star's was `sleepystar`, not `star`; calling it the obvious thing
  would have put every player quietly back on level 1.

Build it in by adding the source to `components/games/CMakeLists.txt` and renaming the
symbols a package would export, so several games can share one firmware:

```cmake
set_source_files_properties("mygame/mygame.c" "mygame/mygame_builtin.c"
    PROPERTIES COMPILE_DEFINITIONS "tat_game=tat_game_mygame;tat_assets=tat_assets_mygame")
```

then declare `extern "C" const tat_game_t tat_game_mygame;` in `main/main.cpp` and register
`static tat::HostedGame mygame(tat_game_mygame);` in the carousel.

### 4.4 Versioning

`major` changes when something is removed or changes meaning. `minor` changes when functions
are **appended to the end** of the struct and nowhere else.

The OS runs a game when `game.api_major == os.api_major && game.api_minor <= os.api_minor`.
Otherwise the launcher greys the icon with "NEEDS A NEWER TILT-A-TRON" (or "BUILT FOR AN
OLDER TILT-A-TRON") and offers uninstall. It never loads a game it can't satisfy.

---

## 5. Flash layout

Today's map spends 8 MB on two 4 MB app slots because the firmware contains every game. With
an OS-only image (~1.2 MB), that drops hard:

| Partition | Size | Holds |
|---|---|---|
| `nvs` | 24 K | settings, saves, the package registry |
| `otadata` | 8 K | which OS slot to boot |
| `app0` / `app1` | 1.5 MB each | the OS, A/B for rollback |
| `games` | 12 MB | installed game packages, code and assets together |
| `storage` | 14 MB | FAT: themes, watch faces. Never exposed over USB (§8) |
| `coredump` | 64 K | crash dumps |

Changing the partition table needs one USB reflash — it can't be done safely over the air.
One-time, and the browser installer already covers it.

**Games region.** Packages are packed head to tail with a free list; slots are 64 KB-aligned
so code sits on an MMU page boundary. Uninstalling marks the slot free; installing takes the
first hole big enough. Compaction rewrites flash, so it asks first and shows progress.

**Registry** (NVS, one blob per installed package): kind, id, name, author, version, accent,
offsets, install date, bytes used. The launcher builds the carousel from the registry alone —
no game code is loaded until you tap its icon.

---

## 6. Loading game code — the open question

Everything above is ordinary engineering. This is the risk, and it's honest to say so: the
ESP32-S3 has no dynamic linker, and the Xtensa toolchain doesn't produce position-independent
code the way a desktop does.

**A. Relocatable ELF, fixed up at install time.** The game is built as an ELF with
relocations; at install the addresses are resolved for the slot it lands in and the fixed
image is written. At launch the code runs straight from flash through the instruction cache,
exactly like the firmware — full speed, no RAM spent on code. Costs a relocation pass, and a
rewrite when the region is compacted.

**B. Load into PSRAM and execute there.** Copy the code into PSRAM and map those pages
executable. Simpler slot handling, but it spends PSRAM on every running game and depends on
instruction fetch from PSRAM.

**A is preferred.** Either way the game source doesn't change: the same code builds as a
package or compiled into the OS.

### 6.1 Answered: it is A, and it is much smaller than feared

Measured on the real games rather than argued. Compile a game to an object and it has
thousands of relocations, which is where the fear came from:

```
R_XTENSA_SLOT0_OP  4116      (patches a field inside an instruction - the hard kind)
R_XTENSA_32        3445
R_XTENSA_ASM_EXPAND 777
```

But that is the state *before* a linker has run. Link the game properly — one contiguous
image at base 0, `--no-relax`, `-q` to keep the relocations, unresolved symbols left
unresolved — and almost all of it is already done:

```
total 594, and every single one of these is a section symbol:
  386  R_XTENSA_SLOT0_OP    .text / .rodata / .bss     <- intra-image, already correct
   91  R_XTENSA_32          .text / .rodata / .bss     <- add the load address
   23  R_XTENSA_ASM_EXPAND  section                    <- relaxation hint, nothing to do
  ...  R_XTENSA_32          sinf, cosf, memset, ...    <- 50, resolve by name
```

Two facts make the loader small:

- **Nothing outside the image is reached by a PC-relative instruction.** `-mlongcalls`
  routes every external call through the literal pool, so it lands as a plain 32-bit word.
  Not one `SLOT0_OP` relocation names an external symbol. Load the image contiguously and
  every branch, call and `l32r` inside it is correct without being touched.
- **So the loader handles exactly one relocation type, `R_XTENSA_32`.** Section-relative
  ones get the load address added; named ones are looked up. 141 of them for Pin Drop.

What a game still needs by name is 21 symbols across all seven games, and every one is
standard C or a compiler helper — `sinf`, `memset`, `snprintf`, `__divsf3` and the like.
None is a console API, which is the point: the console exports a frozen list that means the
same thing in ten years, and everything about the console itself still arrives through the
api table.

That leaves ordinary work: pack the linked image into the `CODE` section, copy it to a
64 KB-aligned block, zero the bss, walk `.rela.text`, map it executable, and find `tat_game`
in the symbol table.

### 6.2 Done: an installed game runs

PIN DROP, packaged with `tools/mktat.py`, sent to `Games/` over the USB link, picked up by
the launcher and played from the icon. `components/loader` is the whole of it.

Three things about `--emit-relocs` cost a day between them, and all three are in the packer
with the reason written beside them:

- **An addend is relative to the INPUT section.** An output section therefore only
  relocates correctly when it begins with the input section its addends are counted from.
  The literal pool has to sit in front of the code (`l32r` only reaches backwards), so with
  both in one `.text` every function pointer was short by the size of the pool and landed
  in the middle of another function. The pool gets its own output section now.
- **A section per function makes that offset the whole answer**, so `-ffunction-sections`
  turns every function pointer into `.text + 0`. Packages don't use it.
- **Merged string sections get addends that are not section-relative at all** - they came
  out negative. `-fno-merge-constants`, and the strings arrive intact.

Two addresses, not one. The image is mapped twice: ordinarily, where it can be read and
written a byte at a time, and again on the instruction bus where it executes. The
instruction bus only does aligned 32-bit loads, so a pointer to a function carries the
executable address and a pointer to a string the ordinary one - which is the symbol's
section, not a guess. Literal pools are the exception that needs no thought: they live in
`.text` and `l32r` is an aligned 32-bit load.

And one that was not a bug but a design mistake. The first version loaded packages during
start-up, and one bad file took the watch through three crashed boots into safe mode, where
it could not even be asked what had happened. A package comes from a stranger: nothing that
loads one may sit between the console and being able to boot. `tat::PackagedGame` opens the
package in `begin()`, which the engine calls the first time an app is shown - so a package
that will not load costs a message on screen and a tap to go back.

**Themes and watch faces don't depend on any of this**, so the app and the sharing story can
ship without waiting for it.

---

## 7. The USB link

Built and working. It runs over the built-in USB-Serial/JTAG port — the same one that carries
the log and does the flashing — so nothing has to be switched on the watch and no drive is
ever mounted. Frames are found by a sync word and checked with a CRC, so log text and
protocol traffic share the wire safely.

```
A5 5A | len u16 | seq u8 | cmd u8 | payload[len] | crc16      (CRC over seq, cmd, payload)
```

A reply carries the same `seq`, with `cmd | 0x80` for success, or `0xFF` whose payload is a
readable reason for the refusal.

| Command | Payload | Reply |
|---|---|---|
| `HELLO` 0x01 | — | protocol version, game API version, firmware version, board |
| `INFO` 0x02 | — | games region total, free, installed count |
| `LIST` 0x03 | — | count, then id/name/accent/flags/size per game |
| `ICON` 0x04 | id[16] | that game's icon PNG |
| `FS_FREE` 0x10 | — | storage total, free |
| `FS_LIST` 0x11 | path | entries: is_dir, size, name |
| `FS_PUT` 0x12 | size u32, crc32 u32, path | OK, then `FS_DATA` chunks, then `FS_END` |
| `FS_DATA` 0x13 | offset u32, chunk | OK. The offset lets a resent chunk be recognised rather than counted twice |
| `FS_END` 0x14 | — | OK once the CRC matches; a bad file is deleted, not kept |
| `FS_GET` 0x15 | path | the file |
| `FS_DELETE` 0x16 | path | OK (a file, or a folder and everything under it) |
| `FS_MKDIR` 0x17 | path | OK |

Still to add: `PKG_BEGIN` / `PKG_DATA` / `PKG_END` / `PKG_REMOVE` for game packages, and
`COMPACT`.

Paths are relative to the storage root; `..`, absolute paths and backslashes are refused. The
watch stays awake while a session is open, and the session lapses 30 s after the last frame.

**Opening the port must leave DTR and RTS alone** — esptool uses those lines to reset the
chip, and asserting them would reboot the watch the moment the app connects. `tools/tatlink.py`
is the reference client and shows exactly this.

---

## 8. Retiring the USB drive

Once the app can send files, the watch never needs to appear as a USB drive again. That
removes, in one go:

- the FAT corruption risk that cost a set of themes, and every guard written afterwards
  (reboot postponement, eject-before-update, "don't format unless blank")
- the "CONNECTED TO COMPUTER" screen, the eject dance, and Settings > USB DRIVE
- TinyUSB MSC and its buffers, which is internal RAM back

Plugging in then means exactly one thing — charging, and talking to the app. Order: ship the
app's theme and face sending, confirm it, **then** remove mass storage, so there is never a
window where themes can't be added.

---

## 9. The manager app

**Native, C# + Avalonia**, in `app/`. One codebase, published as a single self-contained file
per platform: `tiltatron-manager.exe` on Windows, and Mac and Linux binaries from the same
source built in GitHub Actions and attached to a release. Nothing for anyone to install.

What it does:

- Finds the watch by trying `HELLO` on each serial port, and shows firmware, board and free
  space.
- Lists what's installed — games, themes, faces — with icons, and what's built in.
- **Install**: drag a `.tat` onto the window, or pick from a catalogue. Shows the package's
  name, author, version and preview **before** installing, and warns if it replaces something
  already there.
- **Remove**, with the same confirmation the watch shows.
- Sends themes and watch faces as well as games (§8).

`tools/tatlink.py` remains the reference client for the protocol, and what CI and development
use.

---

## 10. Uninstalling on the watch

Hold an icon on the home carousel:

1. After ~600 ms it lifts and wobbles.
2. A panel: the name, the space it frees, and a plain warning that saves go too.
3. **DELETE** / **CANCEL**. Nothing else on the panel.
4. The carousel closes the gap; the space is free immediately.

Holding a built-in game's icon says it's part of the watch and offers Settings > GAMES
instead. Settings > GAMES keeps working as it does now for hiding without deleting.

---

## 11. Safety

- **A bad game can't brick the watch.** The OS writes "launching &lt;id&gt;" before the first
  call into a game and clears it on a clean exit. Booting with that mark still set means it
  crashed: the game is quarantined, and the launcher says so and offers uninstall.
- **The OS never loads a game it can't satisfy** (§4.4).
- **Install is checked end to end** (§2), and the registry entry is written last.
- **Memory is accounted per game.** `alloc`/`free` and every canvas and sheet it created are
  released at `unload`, so leaks can't build up across launches.
- **Themes and faces can't execute anything.** They're images and JSON — the safest kind of
  thing to accept from a stranger.
- **No signing yet.** The `SIG` section is reserved. Worth revisiting when packages start
  arriving over the network rather than by USB, but a shared file shouldn't *require* a
  signature — that would kill the trading.

---

## 12. Order of work

Each phase leaves a working watch.

1. ~~USB link: framing, HELLO/INFO/LIST/ICON~~ **done, proven on hardware.**
1. ~~File commands, and the manager app sending themes~~ **done.**
1. ~~Retire USB mass storage~~ **done.**
1. ~~Spike the game loader~~ **done: code written into PSRAM at run time executes.**
2. File commands over the link, and the manager app sending themes and watch faces.
   *No dependency on §6.*
3. The watch-face renderer (§3), so faces are real content.
4. Retire USB mass storage (§8).
5. Spike the game loader (§6). Decides A or B.
1. ~~OS side of the game ABI against the built-in games — no loading yet~~ **done: PIN DROP
   and SKY JUMP run through `tat::HostedGame` and touch nothing else. Between them they
   cover circles, sprite sheets, a per-frame palette gradient, scaled sprites and assets,
   which is most of what a game can ask for.**
7. Sky Jump as the first real package, end to end: build tool, install, run, uninstall.
   Its source is already package-shaped — only `jump_builtin.c` knows how it was built.
1. ~~Convert the rest, one at a time~~ **done: all seven games are on the API and nothing
   in `components/games` includes an engine header. Pocket Watch is deliberately not one of
   them — it draws a full-screen settings page and needs the wall clock, the time zone and
   the network, which is a watch face's business (§3) rather than a game's.**
9. Repartition (one USB reflash); ship the OS plus a starter set.
10. Hold-to-uninstall on the watch.

Doom is on the `doom-port` branch — a good test of a package with a large `DATA` section once
the loader is real.

---

## 13. Still open

- Should the app be able to *pull* a package back off the watch, so a face you tweaked can be
  shared? (It would make trading much better. Needs the package kept intact on the watch,
  which costs space.)
- Where does a game's high score go when it's uninstalled and reinstalled — gone, or kept
  briefly? Spec says gone, and the panel says so.
- Do packages need to declare the hardware they use, so a future board without an IMU can
  warn rather than misbehave?
- Face format: is strftime enough, or do faces want their own digit art for the big numerals?
