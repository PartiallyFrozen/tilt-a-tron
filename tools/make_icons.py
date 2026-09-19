"""Pixel-art app icons for the carousel, built from the games' own sprite sheets.

Each icon is drawn on a 52 x 52 logical grid, scaled 4x to 208 px and set in a
210 px circle with a dark rim, so it matches the chunky look of the games.

A game's icon goes beside its source, as games/<id>/icon.png, and travels inside its
package: tools/mktat.py will not pack a game without one. The two apps that are part of
the firmware, the clock and Settings, keep theirs in themes/Default/icons/.

    python tools/make_icons.py
"""
import math
import os
import random

from PIL import Image

HERE = os.path.dirname(__file__)
GAMES = os.path.join(HERE, "..", "games")
FIRMWARE_ICONS = os.path.join(HERE, "..", "themes", "Default", "icons")
FIRMWARE_APPS = ("clock", "settings")
L = 52          # logical pixels across
SCALE = 4
ICON = 210


def sheet(game, name, fw=None, fh=None, frame=0):
    """One frame of a sprite sheet as RGBA."""
    img = Image.open(os.path.join(GAMES, game, "assets", name + ".png")).convert("RGBA")
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
    if name in FIRMWARE_APPS:
        path = os.path.join(FIRMWARE_ICONS, name + ".png")
    else:
        path = os.path.join(GAMES, name, "icon.png")
    os.makedirs(os.path.dirname(path), exist_ok=True)
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


def clock_icon():
    img = canvas((0, 0, 0))
    c = 26
    px = img.load()
    gold, gold_d, cream, ink, red = (214, 170, 60), (140, 100, 30), (244, 234, 205), (40, 30, 24), (220, 40, 40)
    for yy in range(L):
        for xx in range(L):
            dx, dy = xx + 0.5 - c, yy + 0.5 - c
            d = math.hypot(dx, dy) / 25
            if d > 1:
                continue
            col = cream
            if d > 0.86:
                col = gold
            elif d > 0.78:
                col = gold_d
            elif d > 0.70:
                a = math.atan2(dy, dx)
                col = ink if (a + math.pi) % (math.pi / 6) < 0.09 else cream
            px[xx, yy] = col + (255,)
    # Crown.
    rect(img, c - 2, 1, 5, 4, gold_d)
    rect(img, c - 1, 0, 3, 3, gold)
    # Hands: hour to 10, minute to 2.
    def hand(angle, length, w, col):
        sx, sy = math.sin(angle), -math.cos(angle)
        for k in range(int(length * 10)):
            t = k / 10
            for o in range(-(w // 2), w // 2 + 1):
                rect(img, int(c + sx * t - sy * o), int(c + sy * t + sx * o), 1, 1, col)
    hand(-math.pi / 3, 10, 3, ink)
    hand(math.pi / 3, 15, 2, ink)
    disc(img, c, c, 1.6, red)
    finish(img, "clock")


def star_icon():
    img = canvas((14, 14, 28))
    c = 26
    px = img.load()
    disc_c, plate, plate2, lip = (22, 22, 42), (46, 53, 80), (58, 67, 104), (74, 85, 128)
    beam, glow, star, face, door = (255, 77, 210), (92, 36, 88), (255, 217, 61), (138, 90, 0), (255, 243, 160)
    for yy in range(L):
        for xx in range(L):
            dx, dy = xx + 0.5 - c, yy + 0.5 - c
            d = math.hypot(dx, dy)
            if d > 25:
                continue
            col = disc_c if d < 22 else (14, 14, 28)
            for (r0, r1, p, holes) in ((15, 20, plate, (0, 3, 5)), (8.5, 13, plate2, (0, 2, 6))):
                if r0 <= d < r1:
                    a = (math.degrees(math.atan2(dy, dx)) + 90 + 22.5) % 360
                    spoke, off = int(a // 45), (a % 45) - 22.5
                    tang = math.radians(off) * d
                    col = p
                    if spoke in holes and abs(tang) < 2.6:
                        col = lip if abs(tang) > 1.8 else disc_c
            px[xx, yy] = col + (255,)
    # The beam, straight down from the rim through both gaps to the star.
    rect(img, c - 2, 2, 4, c - 8, glow)
    rect(img, c - 1, 2, 2, c - 8, beam)
    rect(img, c - 3, 1, 6, 4, (58, 66, 96))
    # The star: an octagon with a sleepy face and its door at the bottom.
    for yy in range(L):
        for xx in range(L):
            dx, dy = abs(xx + 0.5 - c), abs(yy + 0.5 - c)
            if dx <= 6.5 and dy <= 6.5 and dx + dy <= 9.2:
                px[xx, yy] = star + (255,)
    rect(img, c - 4, c - 1, 3, 1, face)
    rect(img, c + 1, c - 1, 3, 1, face)
    rect(img, c - 1, c + 2, 2, 1, face)
    rect(img, c - 2, c + 5, 4, 2, door)
    finish(img, "star")


def pindrop_icon():
    """A cream board with pegs, a red ball mid-fall, and the hole it is heading for."""
    img = canvas((238, 228, 200))
    face, face2 = (238, 228, 200), (218, 206, 176)
    pin, pin_hi, pin_sh = (150, 152, 160), (250, 250, 255), (96, 98, 108)
    ball, ball_hi = (232, 46, 74), (255, 170, 182)
    hole, lip = (14, 16, 24), (255, 205, 60)

    # A hint of the board's dish, so it does not read as flat paper.
    disc(img, L / 2, L / 2, L / 2 - 1, face2)
    disc(img, L / 2, L / 2, L / 2 - 4, face)

    # Pegs in the offset rows a peg board actually has.
    for row in range(4):
        y = 14 + row * 9
        for col in range(5):
            x = 9 + col * 9 + (4 if row % 2 else 0)
            if (x - L / 2) ** 2 + (y - L / 2) ** 2 > (L / 2 - 5) ** 2:
                continue
            rect(img, x, y + 1, 3, 2, pin_sh)
            rect(img, x, y, 3, 2, pin)
            rect(img, x, y, 1, 1, pin_hi)

    # The hole, low and off to one side.
    disc(img, 33, 41, 6, lip)
    disc(img, 33, 41, 5, hole)

    # The ball, on its way down.
    disc(img, 20, 24, 4, ball)
    disc(img, 19, 23, 1, ball_hi)
    finish(img, "pindrop")


def starfall_icon():
    """Looking down the shaft: rings closing in, each with its one gap, and the ship at the rim."""
    img = canvas((6, 8, 20))
    c = L / 2
    px = img.load()
    rnd = random.Random(7)
    for _ in range(26):
        x, y = rnd.randrange(L), rnd.randrange(L)
        px[x, y] = rnd.choice(((90, 100, 140), (150, 160, 200), (60, 70, 110))) + (255,)
    # Far rings are small, dim and thin; the near one is wide and bright. Each has a gap,
    # and the gaps do not line up - which is the whole game.
    rings = ((6, 8, (44, 60, 110), 200), (11, 14, (60, 96, 170), 320), (17, 21, (90, 170, 235), 95))
    for r0, r1, col, gap_at in rings:
        for yy in range(L):
            for xx in range(L):
                dx, dy = xx + 0.5 - c, yy + 0.5 - c
                d = math.hypot(dx, dy)
                if not (r0 <= d < r1):
                    continue
                a = math.degrees(math.atan2(dy, dx)) % 360
                off = (a - gap_at + 180) % 360 - 180
                if abs(off) < 26:
                    continue
                px[xx, yy] = col + (255,)
    disc(img, c, c, 2, (255, 240, 200))
    # The ship, low on the rim, nose toward the centre, with its shot already away.
    ship, ship_hi, flame = (127, 232, 255), (240, 252, 255), (255, 150, 60)
    rect(img, 25, 40, 2, 2, ship_hi)
    rect(img, 24, 42, 4, 3, ship)
    rect(img, 22, 44, 8, 2, ship)
    rect(img, 25, 46, 2, 2, flame)
    rect(img, 25, 33, 2, 4, (255, 230, 120))
    # A mine riding between the rings.
    disc(img, 38, 18, 2.5, (255, 84, 92))
    px[37, 17] = (255, 200, 204, 255)
    finish(img, "starfall")


def skatergirlz_icon():
    """Sunset rooftops, and her mid-ollie over the gap between two of them."""
    img = canvas((44, 32, 74))
    vgradient(img, (44, 32, 74), (242, 146, 110))
    disc(img, 37, 25, 8, (255, 214, 120))
    # Two rows of skyline, far then near.
    far, near = (76, 58, 100), (52, 40, 74)
    for x, w, h in ((0, 9, 14), (9, 7, 20), (16, 10, 12), (26, 8, 17), (34, 9, 11), (43, 9, 19)):
        rect(img, x, 40 - h, w, h, far)
    for x, w, h in ((0, 12, 8), (12, 9, 12), (30, 10, 10), (40, 12, 7)):
        rect(img, x, 40 - h, w, h, near)
    # The street, with the gap she is over.
    street, street2, kerb, void = (62, 62, 72), (48, 48, 58), (150, 150, 164), (14, 12, 22)
    rect(img, 0, 40, L, 12, street2)
    rect(img, 0, 40, 20, 3, street)
    rect(img, 34, 40, 18, 3, street)
    rect(img, 0, 39, 20, 1, kerb)
    rect(img, 34, 39, 18, 1, kerb)
    rect(img, 20, 39, 14, 13, void)
    # Her: blonde, pink shirt, jeans, board tilted nose-up under her feet.
    skin, hair, shirt, jeans = (246, 200, 164), (255, 206, 70), (244, 72, 148), (66, 104, 190)
    board, wheel = (90, 222, 216), (250, 250, 255)
    rect(img, 24, 17, 5, 4, skin)
    rect(img, 23, 15, 6, 3, hair)
    rect(img, 20, 17, 4, 5, hair)      # hair streaming back
    rect(img, 18, 19, 3, 2, hair)
    rect(img, 23, 21, 6, 6, shirt)
    rect(img, 29, 22, 3, 2, skin)      # arm out for balance
    rect(img, 23, 27, 3, 4, jeans)
    rect(img, 27, 27, 3, 3, jeans)
    for i in range(10):                # the deck, rising toward the nose
        rect(img, 20 + i, 32 - i // 3, 2, 2, board)
    rect(img, 21, 34, 2, 2, wheel)
    rect(img, 28, 32, 2, 2, wheel)
    finish(img, "skatergirlz")


def echo_icon():
    """The four pads, one of them lit, around a dark hub."""
    img = canvas((8, 9, 16))
    c = L / 2
    px = img.load()
    hue = ((60, 220, 110), (255, 72, 84), (70, 140, 255), (255, 206, 60))   # up right down left
    lit = 1
    for yy in range(L):
        for xx in range(L):
            dx, dy = xx + 0.5 - c, yy + 0.5 - c
            d = math.hypot(dx, dy)
            if d <= 8:
                px[xx, yy] = ((52, 58, 84) if d > 6.5 else (18, 20, 32)) + (255,)
                continue
            if not (10 <= d <= 24.5):
                continue
            ax, ay = abs(dx), abs(dy)
            if abs(ax - ay) * 0.7071 <= 1.3:
                continue
            pad = (0 if dy < 0 else 2) if ay > ax else (1 if dx > 0 else 3)
            k = 1.0 if pad == lit else 0.34
            if d > 22.5:
                k *= 0.7
            h = hue[pad]
            col = tuple(min(255, int(v * k)) for v in h)
            if pad == lit and d < 12:
                col = tuple(int(v + (255 - v) * 0.5) for v in h)
            px[xx, yy] = col + (255,)
    finish(img, "echo")


def main():
    echo_icon()
    starfall_icon()
    skatergirlz_icon()
    jump_icon()
    racer_icon()
    maze_icon()
    breakout_icon()
    settings_icon()
    tiltatris_icon()
    clock_icon()
    star_icon()
    pindrop_icon()


if __name__ == "__main__":
    main()
