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
| `components/games` | The games |
| `components/link` | USB protocol the manager app speaks, and how files get onto a watch |
| `components/net` | Wi-Fi, over-the-air updates, the web endpoints |
| `app/` | The desktop manager app (C#, Avalonia) - see [app/README.md](app/README.md) |
| `docs/GAME_API.md` | Where this is going: an OS plus games as installable files |

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

Today games are compiled into the firmware: add a source file to `components/games`, a
header in `components/games/include/games/`, and an entry in the carousel in
`main/main.cpp`. Use `wc::Canvas` for the playfield and `console::ui::PauseMenu` for the
pause screen so it behaves like the rest of the console — the shared feel is the point, and
`docs/GAME_API.md` describes the contract games will move to.

Games are becoming installable files that don't need a firmware build at all. If you are
thinking of writing one, read that document first, and say so in an issue — the API is
still being settled and your use case should shape it.

## Reporting something

Say which build it is (Settings > VERSION, or the `sha` from `/status`), what you did and
what happened. `./update.ps1 -Log` prints the watch's recent log and `-Crash` turns the last
crash into source lines.
