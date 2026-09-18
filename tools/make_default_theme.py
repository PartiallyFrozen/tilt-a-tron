"""Generates themes/Default (the built-in look) as plain PNG + JSON files.

The firmware embeds these and copies them onto the Tilt-a-tron's USB drive the
first time it boots, so people have a working example to copy and edit.

    python tools/make_default_theme.py
"""
import json
import math
import os

from PIL import Image

ROOT = os.path.join(os.path.dirname(__file__), "..", "themes", "Default")
ICON = 210          # icon size in the carousel
SCREEN = 466


def hexrgb(h):
    return tuple(int(h[i:i + 2], 16) for i in (1, 3, 5))


def circle_icon(shade, ss=3):
    """Render an icon with `shade(nd, a, x, y) -> (r,g,b) or None`, supersampled for smooth edges."""
    img = Image.new("RGBA", (ICON, ICON), (0, 0, 0, 0))
    px = img.load()
    r = ICON / 2
    for y in range(ICON):
        for x in range(ICON):
            acc = [0, 0, 0]
            hits = 0
            for sy in range(ss):
                for sx in range(ss):
                    dx = x + (sx + 0.5) / ss - r
                    dy = y + (sy + 0.5) / ss - r
                    d = math.hypot(dx, dy)
                    if d > r:
                        continue
                    a = math.atan2(dy, dx) % (2 * math.pi)
                    c = shade(d / r, a, dx / r, dy / r)
                    acc = [acc[i] + c[i] for i in range(3)]
                    hits += 1
            if hits:
                px[x, y] = (acc[0] // hits, acc[1] // hits, acc[2] // hits, 255 * hits // (ss * ss))
    return img


def breakout_icon():
    bg, edge, core = (18, 20, 30), (70, 74, 92), (44, 48, 62)
    bands = [(0.24, 0.36, 9, hexrgb("#ff4f9a")), (0.40, 0.52, 13, hexrgb("#ff8a3d")),
             (0.56, 0.68, 17, hexrgb("#ffd23f"))]

    def shade(nd, a, x, y):
        if nd > 0.955:
            return edge
        if (x - 0.27) ** 2 + (y - 0.69) ** 2 < 0.055 ** 2:
            return (255, 255, 255)
        wrapped = (a - math.pi / 2 + math.pi) % (2 * math.pi) - math.pi
        if 0.79 < nd < 0.87 and abs(wrapped) < 0.42:
            return (255, 255, 255)
        if nd < 0.17:
            return core
        for i, (r0, r1, n, c) in enumerate(bands):
            if r0 <= nd <= r1:
                pos = a / (2 * math.pi) * n
                idx, frac = int(pos), pos - int(pos)
                if frac < 0.07 or frac > 0.93 or (i == 2 and (idx * 7 + 3) % 5 == 0):
                    return bg
                return c
        return bg

    return circle_icon(shade)


def maze_icon():
    floor, wall, edge, ball = (201, 158, 98), (104, 68, 34), (70, 74, 92), (206, 211, 219)
    gaps = [(0.30, 0.6, 1.3), (0.52, 3.4, 4.0), (0.74, 5.2, 5.7), (0.74, 1.9, 2.3)]

    def wrap(a):
        return (a + math.pi) % (2 * math.pi) - math.pi

    def shade(nd, a, x, y):
        if nd > 0.955:
            return edge
        if (x - 0.40) ** 2 + (y + 0.44) ** 2 < 0.010:
            return ball
        if (x + 0.42) ** 2 + (y - 0.12) ** 2 < 0.012:
            return (0, 0, 0)
        for ring in (0.30, 0.52, 0.74):
            if abs(nd - ring) <= 0.035 and not any(g[0] == ring and g[1] < a < g[2] for g in gaps):
                return wall
        if 0.30 < nd < 0.52 and abs(wrap(a - 2.6)) < 0.07:
            return wall
        if 0.52 < nd < 0.74 and abs(wrap(a - 0.4)) < 0.05:
            return wall
        return floor

    return circle_icon(shade)


def racer_icon():
    sky, grass, road, edge = (90, 150, 220), (38, 140, 52), (96, 96, 102), (70, 74, 92)
    red, tire, white = (225, 30, 40), (24, 24, 28), (240, 240, 240)

    def shade(nd, a, x, y):
        if nd > 0.955:
            return edge
        if y < -0.15:
            return sky
        if 0.30 < y < 0.40 and abs(x) < 0.34:
            return tire
        if 0.40 < y < 0.72 and abs(x) < 0.16:
            return red
        if 0.46 < y < 0.76 and 0.20 < abs(x) < 0.36:
            return tire
        depth = (y + 0.15) / 1.1
        half = 0.06 + depth * 0.62
        ax = abs(x)
        if ax < half:
            dash = int(depth * depth * 9) % 2 == 0
            return white if (ax < 0.012 + depth * 0.02 and dash) else road
        if ax < half * 1.16:
            return red if int(depth * depth * 12) % 2 else white
        return grass

    return circle_icon(shade)


def jump_icon():
    sky, edge, cloud = (110, 180, 245), (70, 74, 92), (245, 250, 255)
    plat, plat_top, body, belly = (70, 195, 90), (160, 240, 150), (255, 190, 50), (255, 225, 130)
    white, black, feet = (255, 255, 255), (0, 0, 0), (120, 70, 20)

    def shade(nd, a, x, y):
        if nd > 0.955:
            return edge
        bx, by = x, y + 0.08
        if bx * bx / 0.36 ** 2 + by * by / 0.40 ** 2 < 1.0:
            for i in (-1, 1):
                ex, ey = bx - i * 0.14, by + 0.10
                if (ex - 0.03) ** 2 + (ey - 0.02) ** 2 < 0.045 ** 2:
                    return black
                if ex * ex + ey * ey < 0.085 ** 2:
                    return white
            if bx * bx / 0.22 ** 2 + (by - 0.12) ** 2 / 0.20 ** 2 < 1.0:
                return belly
            return body
        if 0.36 < y < 0.40 and 0.05 < abs(x) < 0.32:
            return feet
        if 0.50 < y < 0.66 and abs(x) < 0.52:
            return plat_top if y < 0.54 else plat
        cx, cy = x + 0.45, y + 0.55
        if (cx * cx + cy * cy < 0.16 ** 2 or (cx + 0.16) ** 2 + (cy + 0.05) ** 2 < 0.12 ** 2
                or (cx - 0.17) ** 2 + (cy + 0.04) ** 2 < 0.13 ** 2):
            return cloud
        return sky

    return circle_icon(shade)


def settings_icon():
    bg, edge, gear, hub = (26, 30, 40), (70, 74, 92), (205, 210, 222), (150, 156, 170)

    def shade(nd, a, x, y):
        if nd > 0.955:
            return edge
        if nd < 0.19:
            return bg
        if nd < 0.27:
            return hub
        if nd < 0.47 or (nd < 0.63 and math.cos(8 * a) > 0.15):
            return gear
        return bg

    return circle_icon(shade)


def background():
    """Near-black with a faint cool glow in the middle (AMOLED-friendly)."""
    img = Image.new("RGB", (SCREEN, SCREEN))
    px = img.load()
    c = SCREEN / 2
    for y in range(SCREEN):
        for x in range(SCREEN):
            t = max(0.0, 1.0 - math.hypot(x + 0.5 - c, y + 0.5 - c) / c)
            t = t * t
            px[x, y] = (int(4 + 8 * t), int(5 + 10 * t), int(8 + 20 * t))
    return img


THEME = {
    "name": "Default",
    "author": "Tilt-a-tron",
    "colors": {
        "background": "#000000",
        "text": "#FFFFFF",
        "label": "#E1E1E1",
        "dim": "#969696",
        "panel": "#10121A",
        "box": "#5A5A64",
        "value": "#28E6E6",
        "accent": "#FFDC28",
        "go": "#28C86E",
        "danger": "#FF2828",
    },
}


def main():
    os.makedirs(os.path.join(ROOT, "icons"), exist_ok=True)
    with open(os.path.join(ROOT, "theme.json"), "w", newline="\n") as f:
        json.dump(THEME, f, indent=2)
        f.write("\n")
    background().save(os.path.join(ROOT, "background.png"), optimize=True)
    breakout_icon().save(os.path.join(ROOT, "icons", "breakout.png"), optimize=True)
    settings_icon().save(os.path.join(ROOT, "icons", "settings.png"), optimize=True)
    maze_icon().save(os.path.join(ROOT, "icons", "maze.png"), optimize=True)
    racer_icon().save(os.path.join(ROOT, "icons", "racer.png"), optimize=True)
    jump_icon().save(os.path.join(ROOT, "icons", "jump.png"), optimize=True)
    for dirpath, _, files in os.walk(ROOT):
        for n in files:
            p = os.path.join(dirpath, n)
            print(f"{os.path.relpath(p, ROOT):28s} {os.path.getsize(p):7d} bytes")


if __name__ == "__main__":
    main()
