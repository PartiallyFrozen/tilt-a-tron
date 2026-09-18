"""Pixel-art app icons for the carousel, built from the games' own sprite sheets.

Each icon is drawn on a 52 x 52 logical grid, scaled 4x to 208 px and set in a
210 px circle with a dark rim, so it matches the chunky look of the games. The
PNGs go to themes/Default/icons/ and are embedded in the firmware as the
built-in icons.

    python tools/make_icons.py
"""
import math
import os
import random

from PIL import Image

HERE = os.path.dirname(__file__)
ASSETS = os.path.join(HERE, "..", "components", "games", "assets")
OUT = os.path.join(HERE, "..", "themes", "Default", "icons")
L = 52          # logical pixels across
SCALE = 4
ICON = 210


def sheet(game, name, fw=None, fh=None, frame=0):
    """One frame of a sprite sheet as RGBA."""
    img = Image.open(os.path.join(ASSETS, game, name + ".png")).convert("RGBA")
    if fw is None:
        return img
    cols = img.width // fw
    x, y = (frame % cols) * fw, (frame // cols) * fh
    return img.crop((x, y, x + fw, y + fh))


def canvas(fill):
    return Image.new("RGBA", (L, L), fill + (255,))


def vgradient(img, top, bottom):
    px = img.load()
    for y in range(L):
        t = y / (L - 1)
        c = tuple(int(top[i] + (bottom[i] - top[i]) * t) for i in range(3))
        for x in range(L):
            px[x, y] = c + (255,)


def rect(img, x, y, w, h, c):
    px = img.load()
    for yy in range(max(0, y), min(L, y + h)):
        for xx in range(max(0, x), min(L, x + w)):
            px[xx, yy] = c + (255,)


def disc(img, cx, cy, r, c):
    px = img.load()
    for yy in range(L):
        for xx in range(L):
            if (xx + 0.5 - cx) ** 2 + (yy + 0.5 - cy) ** 2 <= r * r:
                px[xx, yy] = c + (255,)


def ring(img, cx, cy, r0, r1, c, gaps=(), seg=None):
    """Annulus, optionally split into `seg` bricks with 1 px gaps."""
    px = img.load()
    for yy in range(L):
        for xx in range(L):
            dx, dy = xx + 0.5 - cx, yy + 0.5 - cy
            d = math.hypot(dx, dy)
            if not (r0 <= d < r1):
                continue
            if seg:
                a = (math.atan2(dy, dx) + math.pi) / (2 * math.pi) * seg
                frac = a - int(a)
                width = 0.6 * seg / (2 * math.pi * (r0 + r1) / 2)   # ~0.6 px gap each side
                if frac < width or frac > 1 - width:
                    continue
                if int(a) in gaps:
                    continue
                # Bevel: lit outer edge, shaded inner edge.
                if d > r1 - 1:
                    c2 = tuple(min(255, int(v * 0.6 + 100)) for v in c)
                elif d < r0 + 1:
                    c2 = tuple(int(v * 0.6) for v in c)
                else:
                    c2 = c
                px[xx, yy] = c2 + (255,)
                continue
            px[xx, yy] = c + (255,)


def finish(img, name):
    """Circle-mask the logical image, scale up, add the rim, save."""
    px = img.load()
    r = L / 2
    for y in range(L):
        for x in range(L):
            d = math.hypot(x + 0.5 - r, y + 0.5 - r)
            if d > r - 0.2:
                px[x, y] = (0, 0, 0, 0)
            elif d > r - 1.6:
                px[x, y] = (70, 74, 92, 255)
    big = img.resize((L * SCALE, L * SCALE), Image.NEAREST)
    out = Image.new("RGBA", (ICON, ICON), (0, 0, 0, 0))
    out.paste(big, ((ICON - L * SCALE) // 2, (ICON - L * SCALE) // 2), big)
    os.makedirs(OUT, exist_ok=True)
    path = os.path.join(OUT, name + ".png")
    out.save(path, optimize=True)
    print(f"{name}.png {os.path.getsize(path)} bytes")


def jump_icon():
    img = canvas((0, 0, 0))
    vgradient(img, (72, 140, 235), (150, 215, 255))
    cloud = sheet("jump", "clouds", 20, 8, 0)
    img.paste(cloud, (28, 6), cloud)
    img.paste(cloud, (-6, 14), cloud)
    ledge = sheet("jump", "ledges", 30, 7, 0)
    img.paste(ledge, (11, 41), ledge)
    hopper = sheet("jump", "hopper", 20, 22, 0)
    img.paste(hopper, (16, 17), hopper)
    finish(img, "jump")


def racer_icon():
    img = canvas((0, 0, 0))
    vgradient(img, (52, 100, 184), (172, 202, 242))
    # Hills, then grass.
    px = img.load()
    for x in range(L):
        h = int(3 + 2 * math.sin(x * 0.35) + 1.5 * math.sin(x * 0.9 + 1))
        for y in range(22 - h, 22):
            px[x, y] = (64, 78, 104, 255)
    rect(img, 0, 22, L, L - 22, (38, 140, 52))
    # Road converging to the horizon, with red/white kerbs.
    for y in range(22, L):
        t = (y - 22) / (L - 22)
        half = 2 + t * 24
        kerb = max(1, int(1 + t * 3))
        c = (98, 98, 104) if (y // 3) % 2 else (88, 88, 94)
        rect(img, int(26 - half), y, int(half * 2), 1, c)
        kc = (214, 40, 40) if (y // 3) % 2 else (240, 240, 240)
        rect(img, int(26 - half) - kerb, y, kerb, 1, kc)
        rect(img, int(26 + half), y, kerb, 1, kc)
        if (y // 3) % 2 and y > 28:
            rect(img, 26, y, 1, 1, (235, 235, 235))
    car = sheet("racer", "car", 32, 16, 0)
    # The player's red livery.
    cpx = car.load()
    for y in range(car.height):
        for x in range(car.width):
            r, g, b, a = cpx[x, y]
            if (r, g, b) == (40, 120, 255):
                cpx[x, y] = (225, 30, 40, a)
            elif (r, g, b) == (22, 72, 170):
                cpx[x, y] = (150, 16, 26, a)
    img.paste(car, (10, 28), car)
    finish(img, "racer")


def maze_icon():
    img = canvas((201, 158, 98))
    px = img.load()
    rng = random.Random(3)
    for y in range(L):
        for x in range(L):
            if y % 7 == 3:
                px[x, y] = (170, 128, 72, 255)
            elif rng.random() < 0.05:
                px[x, y] = (212, 172, 112, 255)
    if True:
        for x0, y0, w, h in [(4, 4, 44, 3), (4, 4, 3, 44), (45, 4, 3, 44), (4, 45, 44, 3),
                             (14, 4, 3, 22), (14, 24, 22, 3), (33, 14, 3, 13), (24, 33, 3, 15), (4, 33, 12, 3)]:
            rect(img, x0 + 1, y0 + 2, w, h, (120, 90, 50))
        for x0, y0, w, h in [(4, 4, 44, 3), (4, 4, 3, 44), (45, 4, 3, 44), (4, 45, 44, 3),
                             (14, 4, 3, 22), (14, 24, 22, 3), (33, 14, 3, 13), (24, 33, 3, 15), (4, 33, 12, 3)]:
            rect(img, x0, y0, w, h, (104, 68, 34))
            rect(img, x0, y0, w, 1, (150, 104, 58))
            rect(img, x0, y0, 1, h, (150, 104, 58))
    disc(img, 40, 40, 4.5, (60, 40, 22))
    disc(img, 40.8, 40.8, 3.5, (226, 190, 130))
    disc(img, 40, 40, 3.2, (8, 6, 4))
    flag = sheet("maze", "flag")
    img.paste(flag, (34, 9), flag)
    ball = sheet("maze", "ball")
    ball = ball.resize((12, 12), Image.NEAREST)
    disc(img, 15, 18, 6, (70, 44, 20))
    img.paste(ball, (8, 10), ball)
    finish(img, "maze")


def breakout_icon():
    img = canvas((0, 0, 0))
    px = img.load()
    rng = random.Random(5)
    for y in range(L):
        for x in range(L):
            if rng.random() < 0.03:
                px[x, y] = (rng.choice([(40, 42, 56), (70, 74, 96)])) + (255,)
    c = 26
    ring(img, c, c, 8, 12, (255, 79, 154), seg=6)
    ring(img, c, c, 13, 17, (255, 138, 61), seg=8)
    ring(img, c, c, 18, 22, (94, 230, 168), seg=10, gaps=(7, 8))
    disc(img, c, c, 6.5, (74, 80, 96))
    disc(img, c, c, 5, (16, 19, 26))
    rect(img, 24, 24, 4, 4, (255, 255, 255))
    # Paddle on the rim at the bottom, and the ball above it.
    for x in range(14, 39):
        y = int(c + math.sqrt(max(0, 24.5 ** 2 - (x + 0.5 - c) ** 2)))
        rect(img, x, y - 2, 1, 2, (235, 238, 245))
    disc(img, 30, 43, 2.2, (255, 255, 255))
    finish(img, "breakout")


def settings_icon():
    img = canvas((26, 30, 40))
    c = 26
    px = img.load()
    for y in range(L):
        for x in range(L):
            dx, dy = x + 0.5 - c, y + 0.5 - c
            d = math.hypot(dx, dy) / 26
            a = math.atan2(dy, dx)
            col = None
            if d < 0.19:
                col = None
            elif d < 0.27:
                col = (150, 156, 170)
            elif d < 0.47:
                col = (205, 210, 222)
            elif d < 0.63 and math.cos(8 * a) > 0.15:
                col = (205, 210, 222) if math.cos(8 * a) > 0.45 else (150, 156, 170)
            if col:
                px[x, y] = col + (255,)
    finish(img, "settings")


def tiltatris_icon():
    img = canvas((12, 12, 20))
    c = 26
    cols = [(80, 220, 240), (255, 215, 60), (190, 90, 240), (255, 150, 40), (70, 130, 255), (90, 230, 110),
            (255, 80, 90)]
    px = img.load()
    # Three settled rings of wedges (with gaps), and an orange piece dropping in from the rim.
    for yy in range(L):
        for xx in range(L):
            dx, dy = xx + 0.5 - c, yy + 0.5 - c
            d = math.hypot(dx, dy)
            if d < 6:
                px[xx, yy] = ((74, 80, 100) if d > 4.5 else (16, 19, 28)) + (255,)
                continue
            if d >= 25:
                continue
            ring = int((d - 6) / 4.4)
            rf = (d - 6) - ring * 4.4
            a = (math.atan2(dy, dx) + math.pi) / (2 * math.pi) * 12
            k, frac = int(a), a - int(a)
            gap = 0.5 * 12 / (2 * math.pi * d)
            if frac < gap or frac > 1 - gap:
                px[xx, yy] = (8, 8, 12, 255)
                continue
            filled = ring < 3 and not (ring == 2 and k in (2, 3)) and not (ring == 1 and k == 3)
            if filled:
                col = cols[(k * 3 + ring * 5) % 7]
                if rf > 3.4:
                    col = tuple(min(255, int(v * 0.6 + 100)) for v in col)
                elif rf < 1:
                    col = tuple(int(v * 0.5) for v in col)
                px[xx, yy] = col + (255,)
            elif (ring == 3 and k in (2, 3)) or (ring == 4 and k == 2):
                col = (255, 150, 40)
                if rf > 3.4:
                    col = (255, 205, 140)
                elif rf < 1:
                    col = (128, 75, 20)
                px[xx, yy] = col + (255,)
            elif rf < 1:
                px[xx, yy] = (26, 26, 40, 255)
    finish(img, "tiltatris")


def main():
    jump_icon()
    racer_icon()
    maze_icon()
    breakout_icon()
    settings_icon()
    tiltatris_icon()


if __name__ == "__main__":
    main()
