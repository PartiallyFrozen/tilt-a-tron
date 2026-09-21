# The emulator

Run a Tilt-a-tron game on a PC: in a window, to play it, or from a script, to test it.

```bash
python tools/emu/emu.py play skatergirlz
python tools/emu/emu.py shot echo "wait 0.5; tap 233 233; wait 2; shot turn.png"
```

The console's own screens run here too, though they are not games: `@boot` is the boot
animation, `@power` the power menu and `@hold` the power menu as the hold brings it up
(`emu.py shot @hold "hold b; wait 2; shot ring.png; wait 1.5; shot menu.png"`).

You need Python 3 with Pillow, and a C++ compiler: gcc or clang, or on Windows the MSVC build
tools, which it finds by itself. The first run of a game builds it, in about three seconds;
after that it rebuilds only when a source has changed.

## What it is

Not a lookalike. A game is plain C that reaches the console through one table of functions
(`tat_api.h`), and the console's side of that table is ordinary C++ with very little hardware
in it. So the emulator compiles **the firmware's own sources, unchanged** - the pixel canvas,
the font, banners, the pause menu, the save store, the whole game API
(`components/tat_api/tat_host.cpp`) - together with the game, into one library, and replaces
only what is underneath:

| On the watch | Here |
| --- | --- |
| the presenter streams bands of pixels to the panel | it collects them into a picture |
| an input task reads touch, buttons and the motion sensor | the front end says what they read |
| the clock is the chip's | the clock advances by exactly the step it is given |
| sound goes to the codec | each tone is a line in the log |
| saves go to NVS | saves go to `build/emu/<game>/save.txt` |

What a game draws here is what it draws on a watch, to the pixel, and a game that misbehaves
here misbehaves there. `emu_core.cpp` is the whole of the replacement; `stubs/` is the handful
of ESP-IDF headers the firmware's sources include.

## What it is not

- **It cannot tell you how a game feels in the hand.** Tilt from arrow keys is not tilt from a
  wrist. It will show you that the controls work, not that they are good.
- **It says nothing about speed.** A PC is hundreds of times faster than the watch.
- **It does not exercise the loader.** A game is linked in here, not loaded from a `.tat`. Build
  the package and install it before calling anything finished.
- The gyro reads zero. No game depends on it yet.

## Playing

`play` opens a round window the size of the watch's screen.

| | |
| --- | --- |
| mouse | your finger |
| left / right | turn the watch like a wheel (shift: finer) |
| up / down | tip the top away from you / back toward you |
| space | level |
| F | lay the watch flat, or hold it upright again (it starts upright, which is how it is held) |
| B | the PWR button |
| P | save a picture to `build/emu/<game>/` |
| Esc, H | BOOT: home, which here is quit |

`play <game> --record clip.mp4` films what is played (it needs ffmpeg): every frame, pixel for
pixel at 2x on a black 1080 square, at exactly sixty frames a second of the game's own time, so
the film is smooth even where the window was not.

## Testing

`shot` has no window. It runs a script at exactly sixty frames a second of its own time, with
its own dice, so **the same script gives the same pictures every time** - which is the point.
Steps are separated by `;` or newlines, and the script can be a file.

```
wait <seconds>                       tap <x> <y>
touch <x> <y>  /  release            swipe <x0> <y0> <x1> <y1> [seconds]
turn <degrees>    upright, turned like a wheel, right positive
pitch <degrees>   and tipped, top away from you positive
tilt <ax> <ay> <az>                  gravity as the game sees it, in g
button b [seconds]                   PWR
hold b  /  letgo                     PWR down and left down
shot <file.png>                      sheet <file.png>   every shot so far, tiled
```

From Python, `Console(game, defines=("SK_AUTOPILOT",))` builds the game with a test switch
of its own turned on, as a separate library.

`--seed N` changes the dice, `--fresh` forgets the saved scores first, `--quiet` drops the log.
The log is the same one the watch serves at `/log`: whatever the game says with `T->log()`.

It can also be driven from Python, which is how a test that needs to *react* to a game is
written - `Console` in `emu.py` has `step()`, `run(seconds)`, `pose()`, `touch`, `held` and
`image()`.
