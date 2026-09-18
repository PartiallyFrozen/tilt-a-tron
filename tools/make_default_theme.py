"""Generates themes/Default (the built-in look) as plain PNG + JSON files.

The firmware embeds these and writes them into the Tilt-a-tron's storage the
first time it boots, so people have a working example to copy and edit.

    python tools/make_default_theme.py
"""
import json
import math
import os

from PIL import Image

import make_icons

ROOT = os.path.join(os.path.dirname(__file__), "..", "themes", "Default")
ICON = 210          # icon size in the carousel
SCREEN = 466


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
    # The background is hand-made art now; only generate the placeholder if it is missing.
    if not os.path.exists(os.path.join(ROOT, "background.png")):
        background().save(os.path.join(ROOT, "background.png"), optimize=True)
    make_icons.main()   # the app icons, built from the games' sprite sheets
    for dirpath, _, files in os.walk(ROOT):
        for n in files:
            p = os.path.join(dirpath, n)
            print(f"{os.path.relpath(p, ROOT):28s} {os.path.getsize(p):7d} bytes")


if __name__ == "__main__":
    main()
