#!/usr/bin/env python3
"""Run a Tilt-a-tron game on a PC.

    python tools/emu/emu.py play echo                 a window: play it
    python tools/emu/emu.py shot echo "wait 1; tap 233 233; wait 2; shot a.png"
                                                      no window: drive it from a script

The game is built, with the console's own canvas, font, menus and game API around it, into
a library (see emu_core.cpp), and this drives that library a frame at a time. `play` opens
a round window the size of the watch's screen; `shot` runs a script at exactly sixty frames
a second of its own time, so the same script gives the same pictures every time, which is
what makes it usable for testing.

In the window:
    mouse                  your finger
    left / right           turn the watch like a wheel (hold shift for a finer touch)
    up / down              tip the top away from you / back toward you
    space                  level again
    F                      lay the watch flat / hold it upright (it starts upright)
    B                      the PWR button          Esc or H   the BOOT button (quit)
    P                      save a picture to build/emu/

A script is steps separated by ; or newlines:
    wait <seconds>         run that long
    tap <x> <y>            a finger down and up (screen pixels, 466 x 466)
    touch <x> <y> / release      a finger down and held / let go
    swipe <x0> <y0> <x1> <y1> [seconds]
    tilt <ax> <ay> <az>    gravity as the game sees it, in g (+x right, +y down, +z out of the glass)
    turn <degrees>         the watch upright, turned like a wheel (right is positive)
    pitch <degrees>        ... and tipped, top away from you positive
    button b [seconds]     PWR, clicked or held
    shot <file.png>        save the screen
    sheet <file.png>       save every `shot` so far tiled into one picture
"""
import argparse
import ctypes
import glob
import math
import os
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
OUT = os.path.join(ROOT, "build", "emu")
SCREEN = 466
BTN_B = 2

# The console's sources that a game stands on, compiled unchanged.
CONSOLE_SOURCES = [
    "components/tat_api/tat_host.cpp",
    "components/engine/canvas.cpp",
    "components/engine/font5x7.cpp",
    "components/engine/polar.cpp",
    "components/engine/gfx.cpp",
    "components/engine/store.cpp",
    "components/wc_console/banner.cpp",
    "components/wc_console/pause_menu.cpp",
    "components/lodepng/lodepng.cpp",
]
INCLUDES = [
    "tools/emu/stubs",
    "components/engine/include",
    "components/tat_api/include",
    "components/wc_console/include",
    "components/audio/include",
    "components/lodepng",
    "components/board/include",
]


# ---------------------------------------------------------------------------- building

def find_msvc():
    """cl.exe and the environment it needs, without a developer prompt."""
    hits = sorted(glob.glob(r"C:\Program Files*\Microsoft Visual Studio\*\*\VC\Tools\MSVC\*\bin\Hostx64\x64\cl.exe"))
    if not hits:
        return None
    cl = hits[-1]
    msvc = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(cl))))
    kits = r"C:\Program Files (x86)\Windows Kits\10"
    inc = sorted(glob.glob(os.path.join(kits, "Include", "*")))[-1]
    lib = sorted(glob.glob(os.path.join(kits, "Lib", "*")))[-1]
    env = dict(os.environ)
    env["INCLUDE"] = ";".join([os.path.join(msvc, "include")] + [os.path.join(inc, d) for d in ("ucrt", "shared", "um")])
    env["LIB"] = ";".join([os.path.join(msvc, "lib", "x64"), os.path.join(lib, "ucrt", "x64"), os.path.join(lib, "um", "x64")])
    return cl, env


def library_path(game):
    ext = ".dll" if os.name == "nt" else ".dylib" if sys.platform == "darwin" else ".so"
    return os.path.join(OUT, game, game + ext)


def build(game, force=False, verbose=False):
    game_dir = os.path.join(ROOT, "games", game)
    if not os.path.isdir(game_dir):
        sys.exit(f"no such game: games/{game}")
    game_sources = sorted(glob.glob(os.path.join(game_dir, "*.c")))
    sources = [os.path.join(ROOT, s) for s in CONSOLE_SOURCES] + [os.path.join(HERE, "emu_core.cpp"),
                                                                 os.path.join(HERE, "emu_assets.c")] + game_sources
    lib = library_path(game)
    headers = glob.glob(os.path.join(ROOT, "components", "*", "include", "**", "*.h"), recursive=True)
    newest = max(os.path.getmtime(p) for p in sources + headers + [os.path.abspath(__file__)])
    if not force and os.path.exists(lib) and os.path.getmtime(lib) >= newest:
        return lib

    work = os.path.join(OUT, game)
    os.makedirs(work, exist_ok=True)
    incs = [os.path.join(ROOT, i) for i in INCLUDES]
    msvc = find_msvc() if os.name == "nt" else None
    t0 = time.time()
    if msvc:
        cl, env = msvc
        common = [cl, "/nologo", "/c", "/O2", "/MD", "/W3", "/wd4100", "/wd4996", "/wd4244", "/wd4305", "/wd4267", "/wd4018",
                  "/wd4146", "/D_USE_MATH_DEFINES", "/D_CRT_SECURE_NO_WARNINGS", "/utf-8", "/Fo" + work + os.sep]
        common += ["/I" + i for i in incs]
        # C and C++ in two goes: MSVC will not be told a standard for both at once.
        c_files = [s for s in sources if s.endswith(".c")]
        cpp_files = [s for s in sources if not s.endswith(".c")]
        r = subprocess.run(common + ["/std:c17"] + c_files, env=env, capture_output=True, text=True)
        if r.returncode == 0:
            r = subprocess.run(common + ["/std:c++20", "/EHsc"] + cpp_files, env=env, capture_output=True, text=True)
        if r.returncode == 0:
            objs = [os.path.join(work, os.path.splitext(os.path.basename(s))[0] + ".obj") for s in sources]
            r = subprocess.run([cl, "/nologo", "/LD", "/MD", "/Fe" + lib] + objs, env=env, capture_output=True, text=True,
                               cwd=work)
    else:
        cxx = shutil.which("g++") or shutil.which("clang++")
        cc = shutil.which("gcc") or shutil.which("clang")
        if not cxx or not cc:
            sys.exit("no C++ compiler found: install gcc or clang (or the MSVC build tools on Windows)")
        objs = []
        for s in sources:
            o = os.path.join(work, os.path.basename(s) + ".o")
            is_c = s.endswith(".c")
            cmd = [cc if is_c else cxx, "-c", "-O2", "-fPIC", "-std=gnu17" if is_c else "-std=gnu++20", "-w", "-o", o, s]
            cmd += ["-I" + i for i in incs]
            r = subprocess.run(cmd, capture_output=True, text=True)
            if r.returncode:
                sys.exit(r.stdout + r.stderr)
            objs.append(o)
        r = subprocess.run([cxx, "-shared", "-o", lib] + objs + ["-lm"], capture_output=True, text=True)
    if r.returncode:
        # The compiler names every file as it goes; only what went wrong is worth reading.
        lines = [l for l in (r.stdout + r.stderr).splitlines() if "error" in l.lower() or "unresolved" in l.lower()]
        sys.exit("\n".join(lines[:40]) or (r.stdout + r.stderr))
    if verbose:
        print(f"built {os.path.relpath(lib, ROOT)} in {time.time() - t0:.1f} s")
    return lib


# ---------------------------------------------------------------------------- the console

class Console:
    """One game, running. step() is a frame; frame() is what is on the screen."""

    def __init__(self, game, seed=0, quiet=False, fresh=False, verbose=False):
        self.game = game
        lib = build(game, verbose=verbose)
        # Loaded from a copy, so that the next build can replace the library while a window
        # still has the last one open.
        self._copy = os.path.join(OUT, game, f"run-{os.getpid()}{os.path.splitext(lib)[1]}")
        shutil.copyfile(lib, self._copy)
        self.lib = ctypes.CDLL(self._copy)
        L = self.lib
        L.emu_add_asset.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int]
        L.emu_options.argtypes = [ctypes.c_char_p, ctypes.c_uint, ctypes.c_int]
        L.emu_input.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_uint, ctypes.c_float, ctypes.c_float,
                                ctypes.c_float]
        L.emu_step.argtypes = [ctypes.c_float]
        L.emu_frame.argtypes = [ctypes.c_char_p, ctypes.c_int, ctypes.c_int, ctypes.c_int]
        L.emu_game_name.restype = ctypes.c_char_p

        save = os.path.join(OUT, game, "save.txt")
        if fresh and os.path.exists(save):
            os.remove(save)
        L.emu_options(save.encode(), seed, 1 if quiet else 0)
        self._assets = []   # kept alive: the library holds pointers into them
        for path in sorted(glob.glob(os.path.join(ROOT, "games", game, "assets", "*"))):
            data = open(path, "rb").read()
            self._assets.append(data)
            L.emu_add_asset(os.path.basename(path).encode(), data, len(data))
        if not L.emu_begin():
            sys.exit("the console would not start")
        self.name = L.emu_game_name().decode()
        self.touch = (False, 0, 0)
        self.held = 0
        self.tilt = (0.0, 1.0, 0.0)   # upright, the way a watch is held to be turned like a wheel
        self._buf = ctypes.create_string_buffer(SCREEN * SCREEN * 3)
        self.time = 0.0

    def pose(self, turn_deg=0.0, pitch_deg=0.0, flat=False):
        """Gravity for a watch turned like a wheel and tipped, upright or lying flat."""
        t, p = math.radians(turn_deg), math.radians(pitch_deg)
        if flat:   # face up on a table: tilting it spills gravity along the screen
            self.tilt = (math.sin(t), math.sin(p), math.cos(t) * math.cos(p))
        else:
            self.tilt = (math.sin(t) * math.cos(p), math.cos(t) * math.cos(p), math.sin(p))

    def step(self, dt=1 / 60):
        down, x, y = self.touch
        self.lib.emu_input(1 if down else 0, int(x), int(y), self.held, *self.tilt)
        self.time += dt
        return self.lib.emu_step(dt) != 0

    def run(self, seconds, dt=1 / 60):
        home = False
        for _ in range(max(1, round(seconds / dt))):
            home = self.step(dt) or home
        return home

    def frame(self, corner=(0, 0, 0)):
        self.lib.emu_frame(self._buf, *corner)
        return self._buf.raw

    def image(self, corner=(0, 0, 0)):
        from PIL import Image
        return Image.frombytes("RGB", (SCREEN, SCREEN), self.frame(corner))

    def close(self):
        self.lib.emu_end()


# ---------------------------------------------------------------------------- scripted

def run_script(console, script, out_dir):
    from PIL import Image
    shots = []
    steps = [s.strip() for s in script.replace("\n", ";").split(";") if s.strip() and not s.strip().startswith("#")]
    turn = pitch = 0.0
    for step in steps:
        op, *a = step.split()
        if op == "wait":
            console.run(float(a[0]))
        elif op == "tap":
            console.touch = (True, float(a[0]), float(a[1]))
            console.run(0.08)
            console.touch = (False, float(a[0]), float(a[1]))
            console.run(0.05)
        elif op == "touch":
            console.touch = (True, float(a[0]), float(a[1]))
            console.run(1 / 60)
        elif op == "release":
            console.touch = (False, console.touch[1], console.touch[2])
            console.run(1 / 60)
        elif op == "swipe":
            x0, y0, x1, y1 = map(float, a[:4])
            n = max(2, round((float(a[4]) if len(a) > 4 else 0.2) * 60))
            for i in range(n + 1):
                console.touch = (True, x0 + (x1 - x0) * i / n, y0 + (y1 - y0) * i / n)
                console.step()
            console.touch = (False, x1, y1)
            console.run(0.05)
        elif op == "tilt":
            console.tilt = tuple(map(float, a[:3]))
        elif op == "turn":
            turn = float(a[0])
            console.pose(turn, pitch)
        elif op == "pitch":
            pitch = float(a[0])
            console.pose(turn, pitch)
        elif op == "button":
            console.held = BTN_B
            console.run(float(a[1]) if len(a) > 1 else 0.1)
            console.held = 0
            console.run(0.05)
        elif op == "shot":
            path = a[0] if os.path.isabs(a[0]) else os.path.join(out_dir, a[0])
            os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
            img = console.image(corner=(14, 14, 28))
            img.save(path)
            shots.append(img)
            print(f"  {console.time:6.2f} s  {os.path.relpath(path, ROOT)}")
        elif op == "sheet":
            path = a[0] if os.path.isabs(a[0]) else os.path.join(out_dir, a[0])
            cols = min(4, len(shots)) or 1
            rows = (len(shots) + cols - 1) // cols
            cell = 312
            sheet = Image.new("RGB", (cols * cell, rows * cell), (14, 14, 28))
            for i, img in enumerate(shots):
                sheet.paste(img.resize((cell - 2, cell - 2), Image.NEAREST), ((i % cols) * cell + 1, (i // cols) * cell + 1))
            sheet.save(path)
            print(f"  sheet of {len(shots)}: {os.path.relpath(path, ROOT)}")
        else:
            sys.exit(f"don't know how to '{step}'")


# ---------------------------------------------------------------------------- the window

def play(console, zoom=1.0):
    import tkinter as tk
    from PIL import Image, ImageTk

    size = int(SCREEN * zoom)
    root = tk.Tk()
    root.title(f"Tilt-a-tron - {console.name}")
    root.configure(bg="#0e0e1c")
    root.resizable(False, False)
    label = tk.Label(root, bd=0, bg="#0e0e1c", cursor="hand2")
    label.pack(padx=18, pady=(18, 6))
    status = tk.Label(root, bg="#0e0e1c", fg="#8a97c0", font=("Consolas", 9))
    status.pack(pady=(0, 10))

    state = {"turn": 0.0, "pitch": 0.0, "flat": False, "keys": set(), "quit": False, "photo": None, "last": time.time(), "shot": 0}

    def touch(down):
        def handler(e):
            console.touch = (down, e.x / zoom, e.y / zoom)
        return handler

    label.bind("<ButtonPress-1>", touch(True))
    label.bind("<B1-Motion>", touch(True))
    label.bind("<ButtonRelease-1>", touch(False))

    def key_down(e):
        k = e.keysym.lower()
        state["keys"].add(k)
        if k == "space":
            state["turn"] = state["pitch"] = 0.0
        elif k == "f":
            state["flat"] = not state["flat"]
        elif k in ("escape", "h"):
            state["quit"] = True
        elif k == "p":
            state["shot"] += 1
            path = os.path.join(OUT, console.game, f"shot-{state['shot']:02d}.png")
            console.image(corner=(14, 14, 28)).save(path)
            print("saved", os.path.relpath(path, ROOT))

    root.bind("<KeyPress>", key_down)
    root.bind("<KeyRelease>", lambda e: state["keys"].discard(e.keysym.lower()))

    def frame():
        now = time.time()
        dt = min(0.05, now - state["last"])
        state["last"] = now
        keys = state["keys"]
        rate = (25.0 if ("shift_l" in keys or "shift_r" in keys) else 90.0) * dt
        if "left" in keys: state["turn"] -= rate
        if "right" in keys: state["turn"] += rate
        if "up" in keys: state["pitch"] += rate
        if "down" in keys: state["pitch"] -= rate
        state["turn"] = max(-180.0, min(180.0, state["turn"]))
        state["pitch"] = max(-80.0, min(80.0, state["pitch"]))
        console.pose(state["turn"], state["pitch"], state["flat"])
        console.held = BTN_B if "b" in keys else 0

        if console.step(dt) or state["quit"]:
            console.close()
            root.destroy()
            return
        img = Image.frombytes("RGB", (SCREEN, SCREEN), console.frame(corner=(14, 14, 28)))
        if zoom != 1.0:
            img = img.resize((size, size), Image.NEAREST)
        state["photo"] = ImageTk.PhotoImage(img)
        label.configure(image=state["photo"])
        status.configure(text=f"{'flat' if state['flat'] else 'upright'}   turn {state['turn']:+4.0f}   pitch {state['pitch']:+4.0f}"
                              "     arrows tilt - space level - F flat - B pwr - P picture - Esc quit")
        root.after(8, frame)

    root.after(0, frame)
    root.mainloop()


def main():
    ap = argparse.ArgumentParser(description="run a Tilt-a-tron game on a PC", epilog=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("play", help="a window to play it in")
    p.add_argument("game")
    p.add_argument("--zoom", type=float, default=1.0)
    p.add_argument("--fresh", action="store_true", help="forget the saved scores first")
    s = sub.add_parser("shot", help="run a script and save pictures")
    s.add_argument("game")
    s.add_argument("script", help="the steps, or a file of them")
    s.add_argument("--out", default=None, help="where pictures go (default build/emu/<game>)")
    s.add_argument("--seed", type=int, default=1, help="the dice; the same seed is the same run")
    s.add_argument("--fresh", action="store_true", help="forget the saved scores first")
    s.add_argument("--quiet", action="store_true", help="no log")
    b = sub.add_parser("build", help="just build it")
    b.add_argument("game", nargs="+")
    args = ap.parse_args()

    if args.cmd == "build":
        for g in args.game:
            build(g, force=True, verbose=True)
        return
    if args.cmd == "play":
        play(Console(args.game, fresh=args.fresh, verbose=True), args.zoom)
        return
    script = open(args.script).read() if os.path.exists(args.script) else args.script
    console = Console(args.game, seed=args.seed, quiet=args.quiet, fresh=args.fresh, verbose=True)
    run_script(console, script, args.out or os.path.join(OUT, args.game))
    console.close()


if __name__ == "__main__":
    main()
