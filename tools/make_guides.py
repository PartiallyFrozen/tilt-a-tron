"""Generates themes/Guide: design templates showing where everything sits.

These are copied onto the Tilt-a-tron drive so theme makers can design against
real positions. Open one in any image editor, draw your art underneath, hide the
guide layer, export 466x466 as PNG.

    python tools/make_guides.py
"""
import os

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.join(os.path.dirname(__file__), "..", "themes", "Guide")
W = 466
CX = CY = W // 2

GUIDE = (0, 200, 255, 220)        # cyan guide lines
ZONE = (255, 220, 40, 38)         # translucent "something is drawn here" areas
EDGE = (255, 90, 160, 220)        # screen edge / keep-out
INK = (255, 255, 255, 235)


def font(size=15):
    for name in ("segoeui.ttf", "arial.ttf", "DejaVuSans.ttf"):
        try:
            return ImageFont.truetype(name, size)
        except OSError:
            continue
    return ImageFont.load_default()


FONT = font()
FONT_SMALL = font(13)


def centered(d, cx, y, text, fill=INK, f=None):
    f = f or FONT
    d.text((cx, y), text, fill=fill, font=f, anchor="ma")


class Guide:
    """Dark canvas + a translucent overlay, so zones tint the art instead of hiding it."""

    def __init__(self, label, size=W, backdrop=(10, 10, 14, 255)):
        self.size = size
        self.img = Image.new("RGBA", (size, size), backdrop)
        self.overlay = Image.new("RGBA", (size, size), (0, 0, 0, 0))
        self.d = ImageDraw.Draw(self.overlay)
        if label:
            c = size // 2
            self.d.ellipse([0, 0, size - 1, size - 1], outline=EDGE, width=2)
            self.d.ellipse([16, 16, size - 17, size - 17], outline=(255, 90, 160, 90), width=1)
            self.d.line([c, 0, c, size], fill=(255, 255, 255, 45))
            self.d.line([0, c, size, c], fill=(255, 255, 255, 45))
            centered(self.d, c, 6, label)

    def box(self, x, y, w, h, label):
        self.d.rectangle([x, y, x + w - 1, y + h - 1], fill=ZONE, outline=GUIDE, width=2)
        centered(self.d, x + w // 2, y + h // 2 - 8, label, f=FONT_SMALL)

    def finish(self):
        return Image.alpha_composite(self.img, self.overlay)


def home():
    """Carousel: icon, app name, page dots, hint line."""
    g = Guide("HOME SCREEN")
    icon_cy = CY - 28
    g.d.ellipse([CX - 105, icon_cy - 105, CX + 105, icon_cy + 105], fill=ZONE, outline=GUIDE, width=2)
    centered(g.d, CX, icon_cy - 10, "APP ICON  210 x 210")
    # Neighbour icons peek in from both sides.
    for side in (-1, 1):
        cx = CX + side * 290
        g.d.ellipse([cx - 105, icon_cy - 105, cx + 105, icon_cy + 105], outline=(0, 200, 255, 110), width=2)
    g.box(CX - 160, CY + 96, 320, 34, "APP NAME")
    g.box(CX - 40, CY + 142, 80, 20, "PAGE DOTS")
    g.box(CX - 110, CY + 172, 220, 26, "HINT TEXT")
    return g.finish()


def menus():
    """Settings and pause menus: title, rows, buttons."""
    g = Guide("MENUS (settings / pause)")
    g.box(CX - 150, 40, 300, 34, "TITLE")
    for i, y in enumerate((104, 168, 232, 296)):
        g.box(63, y, 340, 58, f"ROW {i + 1}  (solid panel)")
    g.box(96, 364, 130, 54, "BUTTON")
    g.box(240, 364, 130, 54, "BUTTON")
    centered(g.d, CX, 424, "rows scroll between y=96 and y=354", f=FONT_SMALL)
    return g.finish()


def icon():
    """A single app icon: 210x210, round, transparent outside."""
    g = Guide(None, size=210, backdrop=(10, 10, 14, 255))
    g.d.ellipse([0, 0, 209, 209], outline=GUIDE, width=2)
    g.d.ellipse([10, 10, 199, 199], outline=(0, 200, 255, 90), width=1)
    g.d.line([105, 0, 105, 210], fill=(255, 255, 255, 45))
    g.d.line([0, 105, 210, 105], fill=(255, 255, 255, 45))
    centered(g.d, 105, 94, "APP ICON", f=FONT_SMALL)
    centered(g.d, 105, 110, "210 x 210", f=FONT_SMALL)
    return g.finish()


def main():
    os.makedirs(ROOT, exist_ok=True)
    home().save(os.path.join(ROOT, "guide-home.png"), optimize=True)
    menus().save(os.path.join(ROOT, "guide-menus.png"), optimize=True)
    icon().save(os.path.join(ROOT, "guide-icon.png"), optimize=True)
    for n in sorted(os.listdir(ROOT)):
        print(f"{n:22s} {os.path.getsize(os.path.join(ROOT, n)):7d} bytes")


if __name__ == "__main__":
    main()
