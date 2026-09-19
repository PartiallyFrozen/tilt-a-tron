# Contributing to Tilt-a-tron

Tilt-a-tron is an open console for the Waveshare ESP32-S3-Touch-AMOLED-1.75. The point is
that other people can build for it and make things with it, so anything that makes that
harder is a bug — including undocumented setup, code that only works on one person's
machine, and cleverness that a newcomer can't follow.

MIT licensed. By contributing you agree your work goes out under the same licence.

## Building

You need [ESP-IDF 5.5.5](https://docs.espressif.com/projects/esp-idf/en/v5.5.5/esp32s3/get-started/)
and nothing else. From a clean clone:

```bash
idf.py set-target esp32s3
idf.py build
```

`sdkconfig` is generated — **don't commit it.** Every setting lives in `sdkconfig.defaults`,
which is the source of truth; if you need to change one, change it there so everyone gets it.
Component versions are pinned in `dependencies.lock`.

CI builds the firmware from a clean checkout on every pull request. If it builds there, it
builds for everyone.

## Installing it on a watch

- **Browser:** <https://partiallyfrozen.github.io/tilt-a-tron/> — no toolchain needed.
- **From a clone:** `./install.ps1` or `./install.sh`.
- **While developing:** `./update.ps1` builds and sends the firmware over Wi-Fi, falling
  back to USB. It verifies the install by build hash, so "OK" means the new code is running.
- **Talking to a watch:** `python tools/tatlink.py` lists what's on it, sends and fetches
  files. It is the reference implementation of the USB protocol in `components/link`.

## How the code is arranged

| | |
|---|---|
| `components/board` | **The only hardware-specific code.** Display, touch, IMU, buttons, power. Port this to target another board. |
| `components/engine` | Renderer, frame pipeline, input, the pixel `Canvas` every game draws on, saved settings |
| `components/wc_console` | The shell: launcher, settings, themes, the shared pause menu |
| `games/<id>/` | The games: plain C against `tat_api.h`, each built into a `.tat` package by `tools/mktat.py` |
| `components/loader`, `components/factory` | Loading a package and running it; the factory copies a new watch is given |
| `components/games` | Pocket Watch, the one app that is part of the firmware |
| `components/link` | USB protocol the manager app speaks, and how files get onto a watch |
| `components/net` | Wi-Fi, over-the-air updates, the web endpoints |
| `app/` | The desktop manager app (C#, Avalonia) - see [app/README.md](app/README.md) |
| `docs/GAME_API.md` | The package format, the game API, the loader and the USB protocol |

## Tests

```bash
./tests/run.sh      # or .\tests\run.ps1 on Windows without gcc
```

They compile the real firmware sources against stubs, so they test what ships. CI runs them
before it builds anything. `python tools/bench.py` measures a watch over USB when you want
to show a performance change rather than argue it.

## House style

The existing code is the specification; match it. In particular:

- **Comments say why, not what.** If a line needs explaining, explain the reason it exists
  or the trap it avoids — not what the syntax does. Several comments in this codebase
  record a bug that cost an afternoon; those are the valuable ones.
- **Plain names.** `speed`, not `spd`. `pitch_neutral`, not `pn`.
- 4 spaces, 120 columns, braces on the same line. `clang-format` config is coming.
- Big allocations go in PSRAM (`MALLOC_CAP_SPIRAM`). Internal RAM is scarce and shared with
  Wi-Fi and the display; keep an eye on `heap_internal` in `/status`.
- Anything drawn per frame should avoid allocating at all.

## Adding a game

A game is a folder under `games/`, written in plain C against one header, and built into a
`.tat` package that is installed on a running watch. It does not need a firmware build, and
it is not compiled into one - not even the games in this repository are.

```
games/mygame/
  game.json       id, name, author, version, accent colour
  mygame.c        includes "tat/tat_api.h" and nothing else from the console
  icon.png        210 x 210, at most 4096 bytes. Required: tools/mktat.py will not pack without it
  assets/*.png    sprite sheets, if any
```

```bash
python tools/mktat.py games/mygame          # -> build/mygame.tat
tiltatron-manager install build/mygame.tat  # it appears on the home screen; no restart
```

Start from `games/echo/echo.c` (the newest, about 450 lines) and
[docs/GAME_API.md](docs/GAME_API.md). Use the shared pause menu (`menu_*`) and banners so it
behaves like the rest of the console - the shared feel is the point: tilt first, big bold
text, tap is the primary action, PWR is the secondary one, swipe left is the menu.

Things the console does for you, so that a game cannot get them wrong: everything a game
allocates is taken back when it is unloaded; a package that will not load costs a message
on screen rather than a boot; saves live under the game's id and survive it being removed
and reinstalled.

To ship a game *with* the firmware, add its id to `FACTORY_GAMES` in
`components/factory/CMakeLists.txt` and to the table in `factory.c`. Bump `version` in
`game.json` when you change a game; a watch is given each build of a factory game once.

## Reporting something

Say which build it is (Settings > VERSION, or the `sha` from `/status`), what you did and
what happened. `./update.ps1 -Log` prints the watch's recent log and `-Crash` turns the last
crash into source lines.
