# Tilt-a-tron

**Tilt-a-tron** is a pocket-watch game console built on the Waveshare **ESP32-S3-Touch-AMOLED-1.75C**
(ESP32-S3R8, 32 MB flash, 8 MB PSRAM, 466×466 round CO5300 AMOLED over QSPI,
CST9217 touch, QMI8658 IMU, ES8311 audio). Built on ESP-IDF 5.5.5, C++.

## Install (no toolchain needed)

**Easiest: the browser installer at <https://partiallyfrozen.github.io/tilt-a-tron/>.** Plug the
watch in, open the page in Chrome or Edge on a desktop, press FLASH. (The page lives in `site/`
and is deployed by `.github/workflows/pages.yml`.)

Or from a clone of this repository:

Got the same Waveshare board? The repo ships the built firmware in `firmware/`.
You need Python 3 and a USB cable that carries data.

```powershell
git clone https://github.com/PartiallyFrozen/tilt-a-tron.git
cd tilt-a-tron
.\install.ps1          # Windows          (macOS / Linux: ./install.sh)
```

The script installs `esptool` the first time, then asks you to put the watch in
install mode: unplug it, hold the small **BOOT** button by the USB port, plug it
in, let go after 2 seconds. It writes the bootloader, partition table and app
(about 15 s), and the watch restarts into Tilt-a-tron. `-Erase` wipes everything
first (settings, Wi-Fi, the theme drive). After that, updates can go over Wi-Fi.

Maintainers: after building, `.\tools\make_release.ps1` refreshes `firmware/`
(binaries + `manifest.json` with offsets and the build hash) so the installer
matches the source.

## Build & install (developers)

Needs ESP-IDF 5.5.5 (`C:\Espressif`, installed with EIM). One command:

```powershell
.\update.ps1            # build, install, verify
.\update.ps1 -NoBuild   # install the last build
.\update.ps1 -Status    # what the watch is running
.\update.ps1 -Log       # the watch's recent log, over Wi-Fi
```

It installs over **Wi-Fi** if the watch answers (about 5 s; needs Settings > WI-FI ON),
otherwise over **USB** if it's plugged in (Settings > USB DRIVE off, the default). It pauses the Pal
engine's USB poller automatically, and says what to do if it can't reach the watch.

**Verification uses the build's unique hash** (`sha` in `/status`, the ELF SHA-256),
compared with the file that was sent. Don't use the `built` timestamp: it only
changes when that one source file is recompiled, so it stays the same across
incremental builds.

Recovery, in order of escalation:
- A build that crashes 3 boots in a row drops into **safe mode**: themes, sound and
  the drive are skipped, Wi-Fi is forced on, and the update screen waits for a fix.
- A crash during sound start-up skips sound on the next boot.
- New firmware goes into the idle app slot; if it can't even bring the screen up,
  the bootloader rolls back to the previous build.
- Always works: unplug, hold **BOOT**, plug in, release after 2 s, then `.\update.ps1 -Usb`.

The watch serves `GET /status`, `GET /log`, `GET /reboot` and `POST /update` on port 80
whenever it's on Wi-Fi. Set up the network once in **Settings > NETWORK**. `idf.ps1`
wraps `idf.py` with the ESP-IDF 5.5.5 environment (`C:\Espressif`), e.g.
`.\idf.ps1 -p COM4 monitor`.

## Layout

| Path | What |
| --- | --- |
| `components/board` | Hardware layer (C): QSPI display + TE vsync, touch, IMU, buttons, pin map |
| `components/engine` | Engine (C++): `Gfx` renderer, `Presenter` frame pipeline, `Input` (debounce, click/long press), `Gestures`, `Polar`, `Engine` loop + app switching |
| `components/wc_console` | Console shell: `Launcher` carousel, `SettingsApp`, `UpdateApp` (Wi-Fi setup + OTA), shared `ui.h` widgets, app icons |
| `components/storage` | The Tilt-a-tron drive: 16 MB FAT at `/data`, USB mass-storage (TinyUSB), seeds `Theme/Default` |
| `components/lodepng` | PNG decoder for themes (zlib license) |
| `themes/` | Source of the built-in Default theme (embedded in firmware, copied to the drive) |
| `tools/make_default_theme.py` | Regenerates `themes/Default` PNGs |
| `components/net` | Wi-Fi join/scan, saved credentials, OTA HTTP server, update-mode flag |
| `components/games` | Games. `Breakout`: round breakout ported from the web prototype. `Maze`: tilt marble labyrinth with generated levels and holes |
| `main/` | App registry (carousel order) + `BenchGame` bring-up/calibration app |
| `docs/` | Board schematic |

## Themes

Ready-made themes live in `themes/` (BeachVibez, CPU, SkaterGirl, Spaceportal,
Tiltatron-8bit): copy a folder into the watch's `Theme` drive and pick it in
Settings > THEME. `Default` is built into the firmware.


Themes are plain files on the Tilt-a-tron drive, so anyone can make one:

```
README.txt            what each file is
Guide/                design templates (where icons/rows/titles land)
  guide-home.png      home screen zones, 466x466
  guide-menus.png     settings / pause menu zones
  guide-icon.png      one app icon, 210x210
Theme/
  Default/            built-in look, copied onto the drive on first boot
    theme.json        colors: background text label dim panel box value accent go danger
    background.png    466x466 (any PNG in the folder works), behind home + menus
    icons/<app>.png   210x210 app icons (breakout.png, settings.png)
  My Theme/           any folder you add is a theme
```

Menu rows and buttons are filled with the theme's `panel` color and text on the
background gets a drop shadow, so busy artwork stays readable. Oversized images
are shrunk to fit (up to ~1264x1264); `tools/make_guides.py` regenerates the templates.

- **Settings → USB DRIVE ON** (restarts): plugging into a computer shows the
  Tilt-a-tron drive. Copy `Default`, rename it, edit PNGs/colors, eject. The console
  reloads the theme as soon as the drive is ejected.
- **Settings → THEME** cycles through the folders.
- Missing files fall back to the built-in look. PNGs are decoded once at load
  (~300 ms) into RGB565 + alpha; drawing is straight copies.
- **USB DRIVE OFF** (the default): the USB port is the flashing/log port instead of
  a drive; themes still load from the drive. Recovery flashing always works with
  BOOT held while plugging in.
- The drive's `README.txt` (from `themes/README.txt`) documents sizes for theme makers.

## Console navigation

- **Home** is a carousel: swipe (or tap beside the icon) to browse, tap the icon to launch; PWR does nothing on its own here.
- **Double-click PWR** on home = sleep (iris-out, light sleep, PWR wakes instantly where
  you left off). Still asleep after **AUTO OFF** (default 2 min) → powers down (deep
  sleep); PWR then cold-boots into the carousel.
- **Idle auto off**: no buttons, touch or deliberate motion for the AUTO OFF time (and
  no app keeping it awake via `Game::keepAwake()`, e.g. a ball in play or a firmware
  download) → the screen dims for 10 s, then powers down. Any touch cancels.
- **BOOT** (the small key by the USB port) returns home from any app (handled by the engine).
- **Settings** (scrollable): brightness · theme · Wi-Fi on/off · network (scan + on-screen
  keyboard) · auto off (1/2/5/10 min/never) · USB drive · update · version.
- Wi-Fi is **off by default**. When on, it connects in the background at boot with
  modem power-save and reconnects with backoff; games keep running on core 1.

### Settings worth knowing

- **GAMES**: take any app off the home screen (HIDDEN) or put it back (ON). Nothing is deleted;
  the app and its saves come back when you turn it on again
- **CALIBRATE**: a one-minute walkthrough (lay it flat, spin it half a turn, check the bubble
  level) that measures this watch's motion sensor at rest and corrects every game's tilt
- **USB DRIVE**: off = the USB port is for charging, flashing and logs; on = it's the theme drive
- The watch never dozes off while something is talking to it over Wi-Fi: it stays awake during
  an update or upload and for two minutes after any request to its web server

## Shared console UX

All apps follow the same conventions (use `console::ui` and `wc::Gestures`):
bold text at scale 2 or larger; tap is the primary action; BOOT = home; PWR click
= the app's secondary action (Breakout: change control); hold PWR is reserved
(sleep/power later); PWR double-click on home = sleep; swipe left = the app's menu,
swipe right = back; long menus use `console::ui::ScrollList`; tooltips explain
controls before play starts.
Default to what's fun on a watch you hold: tilt, gravity, momentum, juice.

## How frames stay fast

- **Core 1** runs `Game::update()` + `Game::draw()` into a PSRAM framebuffer.
- Every draw call marks 16-px **dirty bands**. `present()` sends only dirty pixels,
  clipped to the round visible area, merged into a few rectangles.
- Dirty pixels are copied into a pool of internal **DMA buffers** and queued to a
  **core 0** task that waits for the panel's **TE (vsync) edge** and streams them.
  Canvas games skip the framebuffer: their bands are generated straight into the
  DMA buffers (each just under the 32 KB single-transfer limit, and only as wide
  as the round panel is at that height), so a whole frame is on the wire in ~14 ms.
- At most **one frame is in flight**: `present()` blocks until the previous frame
  starts transmitting, so every frame is rendered from fresh input (~1 frame latency).
- Input is sampled on core 0 independently of frame rate: buttons 500 Hz, IMU 250 Hz,
  touch on interrupt. Edges between frames are latched so quick taps are never lost.
- Wi-Fi is off unless the user enables it; its code/zeroed data live in PSRAM/flash
  (`ESP_WIFI_IRAM_OPT` off, `SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY`) so internal RAM
  stays free for display DMA buffers.

## Adding a game

```cpp
class MyGame : public wc::Game {
    void begin(wc::Engine &e) override { /* once */ }
    void enter(wc::Engine &e) override { /* each time it's opened; screen is black */ }
    void update(wc::Engine &e, float dt) override { /* e.input() ... */ }
    void draw(wc::Engine &e, wc::Gfx &g) override { /* erase old, draw new */ }
};
```

Register it in `main/main.cpp`'s `apps[]` with a name, accent color and an icon
function (see `wc_console/icons.cpp`).

### Pixel canvas and sprite sheets (how all the games draw now)

Games draw on a `wc::Canvas`: a small 8-bit palette picture (233x233 at 2x or
155x155 at 3x, in PSRAM) that the presenter scales straight into the display's DMA
bands, skipping the 466x466 framebuffer entirely. A full redraw costs well under a
millisecond, so games repaint every frame and run at the display's 60 Hz limit;
`presentRotated()` rotates the canvas on the way out (Grand Prix). Palette entries
can change per frame for free (sky gradients, tints).

Sprites are plain PNGs under `components/games/assets/<game>/`, embedded by the
`games` CMakeLists and loaded with `Canvas::loadSheet(sheet, png, len, fw, fh)`
(frames side by side, alpha < 128 = transparent). Edit them in any image editor;
`tools/make_sprites.py` regenerates the originals from letter grids and
`tools/preview_sheets.py <game>` tiles them for a look. `Polar` (per-pixel
angle/radius tables) is still there for round layouts like Breakout's rings.

### Seeing the watch from the PC

With Wi-Fi on, `GET /screen` returns a PNG of the display (works for canvas games
too) and `GET /input?...` drives it: `app=N` (0 = home), `tap=x,y`,
`swipe=x0,y0,x1,y1`, `hold=x,y,ms`, `btn=a|b[,ms]`, `tilt=ax,ay,az` or `tilt=off`.
Handy for checking a game without picking the watch up.

## Breakout controls

- **Tilt** (default): the paddle is locked to real-world "down". Turn the watch like
  a wheel and the paddle stays at the bottom while the rings rotate around it
- **Tap** to launch / restart · **PWR** cycles control: tilt → drag → follow · **BOOT** home
- **Swipe left** pauses (control, tilt direction, speed, home); saved to NVS

## Marble Maze controls

- **Tap** to start: however you're holding the watch at that moment becomes "level"
- **Tilt** rolls the ball; reach the checkered flag. Holes wait at the end of wrong turns
- Mazes are generated each level (7x7 up to 13x13, clipped to the round board), with start
  and finish at the two ends of the longest route; more dead ends get holes as levels climb
- **Swipe left** pauses (recenter tilt, sound, best level, home) · **BOOT** home
- 3 balls per run; best level reached is saved

## Grand Prix controls

- The picture stays upright; the whole watch is the steering wheel. Twist to steer
- **Tilt forward** for throttle, **back** to brake (sensitivity or OFF in the menu)
- **Touch** pauses · **Swipe left** menu (steering, tilt speed, sound, restart) · **BOOT** home
- Three laps against five rivals; the pseudo-3D road is drawn upright into a 256x256
  buffer and rotated by the device's roll straight into the display bands

## Sky Jump controls

- **Tilt** left/right to steer Hopper; the screen wraps at the edges
- **Tap** to start, **tap or hold** to shoot straight up
- Green ledges bounce, blue ones slide, brown ones crumble, red springs launch you
- Stomp monsters from above or shoot them; touching one from the side ends the run
- The sky turns to stars as you climb. **Swipe left** menu (tilt sensitivity, sound,
  new game, best) · **BOOT** home. Best height is saved

## Tilt-a-tris controls

- Radial Tetris: wedge pieces fall inward from the rim; fill a whole ring to clear it
- The pile is locked to the real world: **turn the watch** to spin the pile under the
  falling piece, which stays at the top of the screen
- **Tap** rotates the piece · **hold** soft-drops · **PWR** hard-drops
- Some level-ups flip gravity: pieces rise from the core and the pile builds against
  the rim, until the next flip
- Score, level and the next piece live in the core. **Swipe left** menu (sound, new
  game, best) · **BOOT** home. Best score is saved

## Sleepy Star controls

- A laser always falls straight down; rings of walls with gaps, mirrors and splitters sit
  between it and a sleeping star. **Turn the watch** and the emitter moves round the rim to
  real-world "up" (free, 8 notches); **tap a ring** to click it one notch, and the ring inside
  it turns the other way. Get the light through the star's door
- Only taps are counted, against each level's verified best (3 stars at best, 2 within two,
  1 for any solve). Six hand-made levels at 1, 2, 4, 7, 11, 16; every other level is generated
  on the watch from its number (the same for everybody) and solved by breadth-first search
- **PWR** or the RST button starts the level over · **swipe left** menu (level, sound,
  flat play with the gyro, reset) · built from `docs/SLEEPY_STAR_SPEC.md`

## Pocket Watch (Clock)

- Five faces, **tap** to cycle: Pocket (brass, numerals, sweeping seconds, date window),
  Retro LCD (seven-segment digits, day and date), Hopper Sky (sky and sun/moon follow
  the time of day, Hopper hops every second), Tacho (minutes on a rev counter, hours
  on a small dial) and Rings (Tilt-a-tris wedges fill for seconds, minutes, hours)
- Time syncs from NTP whenever Wi-Fi is on; otherwise **swipe left** > SET TIME
- Menu: FACE · 12H/24H · TIME ZONE (whole hours from UTC) · SET TIME

## Bench app

Hold BOOT as the screen turns on. **Tilt** rolls the ball, **touch** drags the dot,
**BOOT** toggles vsync, **PWR** toggles a full-screen stress test.

## Roadmap

- More games in the carousel
- Game store (way later): download games over Wi-Fi. Groundwork in place: background
  Wi-Fi, saved network, OTA slots, 23 MB `storage` partition for game data.
