# Tilt-a-tron manager

A desktop app that talks to a watch over USB: see what is on it, send themes, back
everything up. It speaks the protocol in `components/link` (documented in
[docs/GAME_API.md](../docs/GAME_API.md) section 7), the same one `tools/tatlink.py` uses.

Nothing has to be switched on at the watch's end. Plug it in with a cable that carries
data, make sure it is awake, and the app finds it by trying each serial port in turn.

## Running it

```bash
cd app/Tiltatron.Manager
dotnet run
```

The same binary is a command line tool when given arguments, which is how the protocol
gets tested without a person watching:

```bash
tiltatron-manager list                     # what is on the watch
tiltatron-manager send themes/CPU Theme/CPU
tiltatron-manager remove Theme/CPU
tiltatron-manager bench                    # measures transfer speed
```

On Windows it is a GUI binary, so a shell will not wait for it unless you pipe its output
(`tiltatron-manager list | Out-Host` in PowerShell).

## A file to hand someone

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

Reading from the watch runs at about **86 KB/s**. Writing to it runs at about **9 KB/s**,
so a 300 KB theme takes around half a minute.

That asymmetry is in the watch's USB receive path, not in this app - `tools/tatlink.py`
measures the same. Things that were tried and made no difference: chunk sizes from 256 B to
4 KB, the driver's buffer sizes, the link task's priority, batching the writes to flash,
running the firmware's code from flash instead of PSRAM, and doing it while the watch was
in a game so nothing else touched storage. Worth another look, because esptool pushes
firmware over the same port far faster.
