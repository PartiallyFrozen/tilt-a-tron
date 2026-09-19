# Tilt-a-tron manager

A desktop app that talks to a watch over USB: see what is on it, move games between it
and a library on your computer, send themes, back everything up. It speaks the protocol in `components/link` (documented in
[docs/GAME_API.md](../docs/GAME_API.md) section 7), the same one `tools/tatlink.py` uses.

Nothing has to be switched on at the watch's end. Plug it in with a cable that carries
data, make sure it is awake, and the app finds it by trying each serial port in turn.

## The library

A watch holds a handful of games, and taking one off deletes it. The library is where games
live when they are not on a watch: a plain folder of `.tat` files, `Documents/Tilt-a-tron/Games`
unless `TILTATRON_LIBRARY` says otherwise. There is no database. A file you copy into the
folder is in the library; a file you copy out of it is a game you can give to someone.

The window shows the watch on the left and the library on the right. Drag a game from one
to the other, or pick it and use the button - everything a drag does, a button does too.
Dropping `.tat` files from the desktop onto the library adds them; dropping them onto the
watch adds them *and* installs them, so nothing is ever on a watch and nowhere else.

Removing is arranged so that the first click never loses anything:

- a game leaves the watch by **moving** to the library, and the copy is read back from
  disk before the watch's file is deleted;
- a theme is copied to `Documents/Tilt-a-tron/Themes` before it is removed;
- a different build of a game you already have is filed beside the old one
  (`starfall.v1.tat`), never over it;
- deleting from the library itself is the one real deletion, and it asks twice.

All of it is in `Library.cs`, which knows nothing about windows, so the command line does
exactly what the drag does.

## Running it

```bash
cd app/Tiltatron.Manager
dotnet run
```

The same binary is a command line tool when given arguments, which is how the protocol
gets tested without a person watching:

```bash
tiltatron-manager list                     # what is on the watch
tiltatron-manager library                  # what is in your library
tiltatron-manager add game.tat             # file -> library
tiltatron-manager install starfall         # library -> watch (or give it a .tat file)
tiltatron-manager save starfall            # watch -> library, stays installed
tiltatron-manager uninstall starfall       # off the watch, kept in the library
tiltatron-manager send themes/CPU Theme/CPU
tiltatron-manager remove Theme/CPU
tiltatron-manager bench                    # measures transfer speed
```

On Windows it is a GUI binary, so a shell will not wait for it unless you pipe its output
(`tiltatron-manager list | Out-Host` in PowerShell).

## A file to hand someone

Ready-made builds for Windows, macOS and Linux are on the
[releases page](https://github.com/PartiallyFrozen/tilt-a-tron/releases/latest). Pushing a tag like `v0.8.0` makes one: `.github/workflows/manager.yml`
builds all three, packages them and publishes the release, with its text taken from
`.github/release-notes/<tag>.md` if that exists. To build one yourself:

```bash
dotnet publish -c Release -r win-x64 -o publish
```

`publish/tiltatron-manager.exe` is one file, about 42 MB, that runs on a machine with no
.NET installed - double-click it and it opens. That size is what a self-contained .NET
app costs; trimming it was tried and broke the build, because Avalonia loads its XAML by
reflection.

CI builds the same file for Windows, macOS and Linux on every change to `app/`, and
attaches them to the run.

## Building something to hand out

```bash
dotnet publish -c Release -r win-x64
dotnet publish -c Release -r osx-arm64
dotnet publish -c Release -r linux-x64
```

Each produces one self-contained file with no runtime for anyone to install. CI builds all
three from a clean checkout.

## How it is put together

| | |
|---|---|
| `Link/Watch.cs` | The protocol: framing, CRC, retries, and every command. No UI in here. |
| `Program.cs` | Entry point, and the command line mode. |
| `MainWindow.axaml(.cs)` | The window. Code-behind rather than MVVM - it is a small app and this is easier to follow. |

Avalonia, because one codebase covers Windows, macOS and Linux and publishes as a single
file. Keep `Watch.cs` free of anything to do with windows so it stays usable from scripts
and tests.

## Speed, and a known limit

`python tools/bench.py` measures a watch, so a change can be shown rather than argued:

```
  round trip, no payload      0.3 ms
  read   (watch -> PC)      163.2 KB/s
  write  (PC -> watch)        9.0 KB/s
```

Writing is the odd one out, and it is the watch's USB receive path - not this app, not the
filesystem. Re-sending chunks the watch already has makes it skip the flash entirely, and
that measures the same 9 KB/s, which rules the storage out completely. Also ruled out, with
the benchmark in hand: chunk sizes from 256 B to 4 KB, the driver buffer sizes, the link
task's priority, batching the writes, and running the firmware from flash instead of PSRAM
(measurably worse, 8.5 vs 9.6 KB/s). esptool pushes firmware over the same port far faster,
so it is solvable - just not yet solved.

A 300 KB theme therefore takes about half a minute to send. Reading and backing up are quick.
