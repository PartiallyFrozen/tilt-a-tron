"""The manager app's icon: the console's round screen, tilted.

    python tools/make_app_icon.py

Writes app/Tiltatron.Manager/icon.ico at the sizes Windows asks for.
"""
import math
import os

from PIL import Image, ImageDraw

HERE = os.path.dirname(__file__)
OUT = os.path.join(HERE, "..", "app", "Tiltatron.Manager", "icon.ico")
S = 256   # drawn at this size, then scaled down for the smaller entries

VOID = (14, 14, 28, 255)
EDGE = (58, 66, 96, 255)
GOLD = (255, 217, 61, 255)
GOLD_DARK = (138, 90, 0, 255)
CYAN = (127, 232, 255, 255)


def main():
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    pad = S * 0.04
    box = (pad, pad, S - pad, S - pad)

    # The watch: a dark disc with a gold bezel.
    d.ellipse(box, fill=VOID, outline=GOLD, width=int(S * 0.055))
    inner = S * 0.13
    d.ellipse((inner, inner, S - inner, S - inner), outline=EDGE, width=int(S * 0.012))

    # Tilted: a square leaning over, which is the whole idea of the console.
    c, r, a = S / 2, S * 0.20, math.radians(20)
    pts = []
    for dx, dy in ((-1, -1), (1, -1), (1, 1), (-1, 1)):
        x, y = dx * r, dy * r
        pts.append((c + x * math.cos(a) - y * math.sin(a), c + x * math.sin(a) + y * math.cos(a)))
    d.polygon(pts, fill=GOLD, outline=GOLD_DARK)

    # A dot marking "up", so the tilt reads as motion rather than a wonky square.
    d.ellipse((c - S * 0.035, S * 0.17, c + S * 0.035, S * 0.17 + S * 0.07), fill=CYAN)

    sizes = [256, 128, 64, 48, 32, 16]
    img.save(OUT, format="ICO", sizes=[(s, s) for s in sizes])
    print(f"{os.path.relpath(OUT, HERE)}  {os.path.getsize(OUT)} bytes, sizes {sizes}")
    img.resize((128, 128), Image.LANCZOS).save(os.path.join(HERE, "..", "build", "app_icon_preview.png"))


if __name__ == "__main__":
    main()
