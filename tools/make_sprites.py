"""Generates the pixel-art sprite sheets for the games as PNG files.

The PNGs under games/<game>/assets/ are what each game's package carries. They can be
edited in any image editor (keep the frame size and count); this script just
gives them a first version from the letter grids below.

    python tools/make_sprites.py
"""
import os

from PIL import Image

ROOT = os.path.join(os.path.dirname(__file__), "..", "games")

# Shared palette: one letter per colour, '.' is transparent.
PALETTE = {
    "D": (40, 24, 20),      # outline
    "O": (255, 185, 40),    # Hopper body
    "o": (255, 228, 130),   # belly
    "R": (220, 50, 50),     # cap
    "r": (255, 120, 110),   # cap highlight
    "W": (255, 255, 255),
    "K": (20, 16, 24),
    "F": (110, 65, 25),     # boots
    "P": (175, 65, 210),    # monster
    "p": (200, 100, 230),   # monster belly
    "G": (80, 200, 95),     # grass
    "g": (170, 245, 150),   # grass tufts
    "B": (150, 100, 55),    # dirt
    "b": (110, 70, 40),     # dirt specks
    "I": (90, 160, 240),    # ice
    "i": (60, 120, 200),
    "w": (200, 235, 255),
    "S": (165, 165, 175),   # stone
    "s": (130, 130, 140),
    "Y": (255, 220, 50),
    # Clouds get their own (nearly white) colours so the game can tint them.
    "C": (240, 248, 255),
    "c": (200, 215, 240),
}

HOPPER = [
    "......RRRRRRR.......",
    "....RRrrrrrrrRR.....",
    "...RRrrrrrrrrrrRR...",
    "..DRRRRRRRRRRRRRRD..",
    ".DOOOOOOOOOOOOOOOOD.",
    "DOOOWWWWWOOWWWWWOOOD",
    "DOOWWWWWWWWWWWWWWOOD",
    "DOOWWWWKKWWWWWKKWOOD",
    "DOOWWWWKKWWWWWKKWOOD",
    "DOOOWWWWWOOWWWWWOOOD",
    "DOOOOOOOOOOOOOOOOOOD",
    "DOOOOOoooooooooOOOOD",
    "DOOOOooooooooooooOOD",
    "DOOOOooooKKKKoooOOOD",
    "DOOOOoooooooooooOOOD",
    ".DOOOOooooooooooOOD.",
    ".DOOOOOOOOOOOOOOOOD.",
    "..DOOOOOOOOOOOOOOD..",
    "...DDOOOOOOOOOODD...",
    ".....DDDDDDDDDD.....",
    "....FFFF....FFFF....",
    "...FFFFF....FFFFF...",
]

# Frame 1: blink. Frame 2: knocked out (X eyes, cap askew).
HOPPER_BLINK = HOPPER[:5] + [
    "DOOOOOOOOOOOOOOOOOOD",
    "DOOOOOOOOOOOOOOOOOOD",
    "DOOKKKKKKOOOKKKKKOOD",
    "DOOOOOOOOOOOOOOOOOOD",
    "DOOOOOOOOOOOOOOOOOOD",
] + HOPPER[10:]

HOPPER_DEAD = [
    "........RRRRRRR.....",
    "......RRrrrrrrrRR...",
    ".....RRrrrrrrrrrrRR.",
    "..D..RRRRRRRRRRRRRRD",
    ".DOOOOOOOOOOOOOOOOD.",
    "DOOKOOOKOOOOKOOOKOOD",
    "DOOOKOKOOOOOOKOKOOOD",
    "DOOOOKOOOOOOOOKOOOOD",
    "DOOOKOKOOOOOOKOKOOOD",
    "DOOKOOOKOOOOKOOOKOOD",
    "DOOOOOOOOOOOOOOOOOOD",
    "DOOOOOoooooooooOOOOD",
    "DOOOOooooooooooooOOD",
    "DOOOOooooooooooooOOD",
    "DOOOOoooKKKKKKooOOOD",
    ".DOOOOooooooooooOOD.",
    ".DOOOOOOOOOOOOOOOOD.",
    "..DOOOOOOOOOOOOOOD..",
    "...DDOOOOOOOOOODD...",
    ".....DDDDDDDDDD.....",
    "....FFFF....FFFF....",
    "...FFFFF....FFFFF...",
]

MONSTER = [
    ".DD............DD.",
    ".DPD..........DPD.",
    "..DPD........DPD..",
    "..DDPPDDDDDDPPDD..",
    ".DPKKKKPPPPKKKKPD.",
    "DPPWWWWPPPPWWWWPPD",
    "DPWWWWWWPPWWWWWWPD",
    "DPWWKKWWPPWWKKWWPD",
    "DPPWWWWPPPPWWWWPPD",
    "DPPPPPPPPPPPPPPPPD",
    "DPPDDDDDDDDDDDDPPD",
    "DPPDWDWDWDWDWDWDPD",
    ".DPPDDDDDDDDDDDPD.",
    ".DPPppppppppppPPD.",
    "..DDPPDDDDDDPPDD..",
    "...DD........DD...",
]
# Frame 1: mouth shut, eyes narrowed (it's about to lunge).
MONSTER_2 = MONSTER[:4] + [
    ".DPPKKKKPPPPKKKKPD",
    "DPPPWWWWPPPPWWWWPD",
    "DPPWWKKWWPPWWKKWPD",
    "DPPPWWWWPPPPWWWWPD",
    "DPPPPPPPPPPPPPPPPD",
    "DPPPPPPPPPPPPPPPPD",
    "DPPPDDDDDDDDDDDPPD",
    "DPPPPPPPPPPPPPPPPD",
    ".DPPppppppppppPPD.",
    ".DPPppppppppppPPD.",
    "..DDPPDDDDDDPPDD..",
    "...DD........DD...",
]

GRASS = [
    "..g..g.....gg...g....g..g.g...",
    ".GGGGGGGGGGGGGGGGGGGGGGGGGGGG.",
    "GGGGGGGGGGGGGGGGGGGGGGGGGGGGGG",
    "DBBBBBBBBBBBBBBBBBBBBBBBBBBBBD",
    "DBbBBBBbBBBBBbBBBBbBBBBBbBBbBD",
    "DBBBBBBBBBBBBBBBBBBBBBBBBBBBBD",
    ".DDDDDDDDDDDDDDDDDDDDDDDDDDDD.",
]
ICE = [
    "..............................",
    ".WWWWWWWWWWWWWWWWWWWWWWWWWWWW.",
    "WWwwwwwwwwwwwwwwwwwwwwwwwwwwWW",
    "IwwIIIIIIIIIIIIIIIIIIIIIIIwwII",
    "IIIIIIIiIIIIIiIIIIIIiIIIIIIIII",
    "DIIIIIIIIIIIIIIIIIIIIIIIIIIIID",
    ".DDDDDDDDDDDDDDDDDDDDDDDDDDDD.",
]
STONE = [
    "..............................",
    ".SSSSSSSSSSSSSSSSSSSSSSSSSSSS.",
    "SSSSSSKSSSSSSSSSSSSSSKSSSSSSSS",
    "SssssKssssssKKKsssssKsssssssss",
    "SssssKsssssKsssKssssKKsssssssS",
    "DssssssssssKssssssssssKsssssSD",
    ".DDDDDDDDDDDDDDDDDDDDDDDDDDDD.",
]
SPRING = [
    ".rrrrrrrrrr.",
    "RRRRRRRRRRRR",
    "..DssssssD..",
    "..DsDDDDsD..",
    "..DssssssD..",
    "..DsDDDDsD..",
    "..DssssssD..",
]
SHOT = [
    ".YY.",
    "YWWY",
    "YWWY",
    ".YY.",
]
CLOUD_A = [
    ".......CCCC.........",
    ".....CCCCCCC...CCC..",
    "...CCCCCCCCCCCCCCCC.",
    "..CCCCCCCCCCCCCCCCCC",
    ".CCCCCCCCCCCCCCCCCCC",
    "CCCCCCCCCCCCCCCCCCCC",
    "cCCCCCCCCCCCCCCCCCCc",
    ".cccccccccccccccccc.",
]
CLOUD_B = [
    "....................",
    "....................",
    ".......CCC..........",
    ".....CCCCCCC.CC.....",
    "....CCCCCCCCCCCC....",
    "...CCCCCCCCCCCCCC...",
    "...cCCCCCCCCCCCCc...",
    "....cccccccccccc....",
]


# ---------------------------------------------------------------- Grand Prix
# The car uses the racer's own palette colours so the firmware can recolour the
# body per livery: B/b are the "blue" livery and get remapped at draw time.
CAR_PAL = {
    "W": (34, 34, 42),      # wing
    "N": (255, 255, 255),   # wing plate
    "B": (40, 120, 255),    # body (remapped per car)
    "b": (22, 72, 170),     # body shade (remapped per car)
    "H": (255, 214, 40),    # helmet
    "h": (255, 244, 190),   # helmet highlight
    "T": (22, 22, 26),      # tyre
    "t": (62, 62, 70),      # tyre highlight
    "G": (112, 116, 126),   # suspension / diffuser
    "K": (0, 0, 0),
    "R": (255, 50, 50),     # rain light
    "F": (255, 150, 30),    # exhaust flame (drawn only under throttle)
}
CAR = [
    "...WWWWWWWWWWWWWWWWWWWWWWWWWW...",
    "...WNNNNNNNNNNNNNNNNNNNNNNNNW...",
    "...WNBBBBBBBBBBBBBBBBBBBBBBNW...",
    "...WWWWWWWWWWWWWWWWWWWWWWWWWW...",
    "...W...........HHH..........W...",
    "...W..........HhhhH.........W...",
    "TTTTTT........BHHHB.......TTTTTT",
    "TtTTTTT......BBBBBBB.....TTTTTtT",
    "TtTTTTTGGG.BBBBbbBBBB.GGGTTTTTtT",
    "TtTTTTTGGBBBBbbbbbbBBBBGGTTTTTtT",
    "TtTTTTT.BBBBbbKKKKbbBBBB.TTTTTtT",
    "TtTTTTT.BBBbbKRRRRKbbBBB.TTTTTtT",
    "TtTTTTT..GGGGKKKKKKGGGG..TTTTTtT",
    "TtTTTTT...GGGGGFFGGGGG...TTTTTtT",
    "TTTTTT.....GGG.FF.GGG.....TTTTTT",
    ".TTTT.........FF...........TTTT.",
]


def new_rgba(w, h):
    img = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    return img, img.load()


def paint_tree_round(px, x0, w, h):
    """A leafy tree: dark outline, mid green, lit top-left, dark right side, trunk."""
    trunk, leaf, lit, dark, edge = (92, 60, 30), (40, 150, 60), (96, 200, 90), (26, 104, 44), (18, 60, 30)
    cx = x0 + w / 2
    tw, th = max(2, w // 6), h * 0.3
    for y in range(int(h - th), h):
        for x in range(int(cx - tw / 2), int(cx + tw / 2) + 1):
            px[x, y] = trunk + (255,)
    blobs = [(cx, h * 0.38, w * 0.48, h * 0.36), (cx - w * 0.22, h * 0.5, w * 0.32, h * 0.26),
             (cx + w * 0.22, h * 0.5, w * 0.32, h * 0.26)]

    def inside(x, y, grow=0.0):
        for (bx, by, rx, ry) in blobs:
            if ((x - bx) / (rx + grow)) ** 2 + ((y - by) / (ry + grow)) ** 2 <= 1.0:
                return True
        return False

    for y in range(h):
        for x in range(x0, x0 + w):
            if not inside(x + 0.5, y + 0.5):
                continue
            c = leaf
            if not inside(x + 0.5, y + 0.5, -1.2):
                c = edge
            elif x + 0.5 < cx - w * 0.05 and y < h * 0.45 and inside(x + 0.5 - 1.5, y - 1.5, -2.5):
                c = lit
            elif x + 0.5 > cx + w * 0.12 or y > h * 0.55:
                c = dark
            px[x, y] = c + (255,)


def paint_tree_pine(px, x0, w, h):
    trunk, leaf, lit, dark, edge = (92, 60, 30), (30, 120, 50), (70, 170, 80), (18, 80, 36), (14, 50, 26)
    cx = x0 + w / 2
    tw = max(2, w // 7)
    for y in range(int(h * 0.78), h):
        for x in range(int(cx - tw / 2), int(cx + tw / 2) + 1):
            px[x, y] = trunk + (255,)
    tiers = [(0.0, 0.42, 0.55), (0.28, 0.66, 0.8), (0.52, 0.86, 1.0)]
    for (t0, t1, wf) in tiers:
        y0, y1 = int(h * t0), int(h * t1)
        for y in range(y0, y1):
            half = (w / 2) * wf * (y - y0 + 1) / max(1, (y1 - y0))
            for x in range(int(cx - half), int(cx + half) + 1):
                if x < x0 or x >= x0 + w:
                    continue
                c = leaf
                if abs(x + 0.5 - cx) > half - 1.2 or y == y1 - 1:
                    c = edge
                elif x + 0.5 < cx - 1:
                    c = lit
                elif x + 0.5 > cx + half * 0.45:
                    c = dark
                px[x, y] = c + (255,)


def paint_sign(px, x0, w, h):
    """Corner board: red with white chevrons pointing right, on two grey legs."""
    red, white, grey, edge = (220, 40, 50), (244, 244, 248), (112, 116, 126), (40, 24, 20)
    bh = int(h * 0.6)
    for y in range(bh):
        for x in range(x0, x0 + w):
            c = red
            o = int(abs(y - (bh - 1) / 2) * 0.9)   # chevrons ">" : tip in the middle row
            if (x - x0 + o) % 7 < 2:
                c = white
            if x == x0 or x == x0 + w - 1 or y == 0 or y == bh - 1:
                c = edge
            px[x, y] = c + (255,)
    for lx in (x0 + w // 5, x0 + w - w // 5 - 2):
        for y in range(bh, h):
            px[lx, y] = grey + (255,)
            px[lx + 1, y] = edge + (255,)


def paint_stand(px, x0, w, h):
    """Grandstand: dark roof, rows of crowd, grey front wall with a stripe."""
    import random
    rng = random.Random(7)
    roof, wall, edge = (34, 34, 42), (112, 116, 126), (20, 20, 26)
    crowd = [(255, 255, 255), (255, 50, 50), (255, 214, 40), (40, 120, 255), (40, 190, 90), (255, 130, 30),
             (170, 80, 220)]
    rh = max(2, h // 6)
    for y in range(h):
        for x in range(x0, x0 + w):
            if y < rh:
                c = roof if y < rh - 1 else edge
            elif y >= h - rh:
                c = wall if (y - (h - rh)) != 1 else (214, 40, 40)
            else:
                c = (60, 64, 76) if (y - rh) % 3 == 2 else rng.choice(crowd)
            if x == x0 or x == x0 + w - 1:
                c = edge
            px[x, y] = c + (255,)


def paint_bush(px, x0, w, h):
    leaf, lit, dark, edge = (40, 150, 60), (96, 200, 90), (26, 104, 44), (18, 60, 30)
    cx, cy = x0 + w / 2, h * 0.6
    for y in range(h):
        for x in range(x0, x0 + w):
            d = ((x + 0.5 - cx) / (w / 2)) ** 2 + ((y + 0.5 - cy) / (h * 0.62)) ** 2
            if d > 1:
                continue
            c = leaf
            if d > 0.78:
                c = edge
            elif x + 0.5 < cx - w * 0.1 and y < cy:
                c = lit
            elif x + 0.5 > cx + w * 0.15:
                c = dark
            px[x, y] = c + (255,)


def racer_sheets(root):
    out = os.path.join(root, "racer", "assets")
    os.makedirs(out, exist_ok=True)
    fw, fh = len(CAR[0]), len(CAR)
    img, px = new_rgba(fw, fh)
    for y, row in enumerate(CAR):
        assert len(row) == fw, (y, len(row))
        for x, ch in enumerate(row):
            if ch != ".":
                px[x, y] = CAR_PAL[ch] + (255,)
    img.save(os.path.join(out, "car.png"), optimize=True)
    img, px = new_rgba(48, 32)
    paint_tree_round(px, 0, 24, 32)
    paint_tree_pine(px, 24, 24, 32)
    img.save(os.path.join(out, "trees.png"), optimize=True)
    img, px = new_rgba(24, 18)
    paint_sign(px, 0, 24, 18)
    img.save(os.path.join(out, "sign.png"), optimize=True)
    img, px = new_rgba(48, 30)
    paint_stand(px, 0, 48, 30)
    img.save(os.path.join(out, "stand.png"), optimize=True)
    img, px = new_rgba(16, 10)
    paint_bush(px, 0, 16, 10)
    img.save(os.path.join(out, "bush.png"), optimize=True)
    for n in ("car", "trees", "sign", "stand", "bush"):
        p = os.path.join(out, n + ".png")
        print(f"racer/{n}.png {Image.open(p).size} {os.path.getsize(p)} bytes")


# ---------------------------------------------------------------- Marble Maze
def paint_ball(px, w):
    """A steel ball: dark rim, mid grey body, soft light from the top-left, a hot spot."""
    rim, body, light, spot, dark = (90, 96, 108), (168, 174, 184), (206, 211, 219), (255, 255, 255), (120, 126, 138)
    r = w / 2
    for y in range(w):
        for x in range(w):
            dx, dy = x + 0.5 - r, y + 0.5 - r
            d = (dx * dx + dy * dy) ** 0.5 / r
            if d > 1:
                continue
            c = body
            if d > 0.86:
                c = rim
            elif d > 0.62 and dx + dy > 0:
                c = dark
            lx, ly = dx + r * 0.35, dy + r * 0.38
            if (lx * lx + ly * ly) ** 0.5 / r < 0.42 and d <= 0.86:
                c = light
            if (lx * lx + ly * ly) ** 0.5 / r < 0.18:
                c = spot
            px[x, y] = c + (255,)


def paint_flag(px, w):
    black, white, edge = (0, 0, 0), (255, 255, 255), (60, 40, 22)
    q = w // 4
    for y in range(w):
        for x in range(w):
            c = white if ((x // q) + (y // q)) % 2 else black
            if x == 0 or y == 0 or x == w - 1 or y == w - 1:
                c = edge
            px[x, y] = c + (255,)


def maze_sheets(root):
    out = os.path.join(root, "maze", "assets")
    os.makedirs(out, exist_ok=True)
    img, px = new_rgba(16, 16)
    paint_ball(px, 16)
    img.save(os.path.join(out, "ball.png"), optimize=True)
    img, px = new_rgba(12, 12)
    paint_flag(px, 12)
    img.save(os.path.join(out, "flag.png"), optimize=True)
    for n in ("ball", "flag"):
        p = os.path.join(out, n + ".png")
        print(f"maze/{n}.png {Image.open(p).size} {os.path.getsize(p)} bytes")


def sheet(frames, path):
    """Frames side by side, all the same size, saved as RGBA."""
    fw, fh = len(frames[0][0]), len(frames[0])
    for f in frames:
        assert len(f) == fh and all(len(r) == fw for r in f), path
    img = Image.new("RGBA", (fw * len(frames), fh), (0, 0, 0, 0))
    px = img.load()
    for i, f in enumerate(frames):
        for y, row in enumerate(f):
            for x, ch in enumerate(row):
                if ch != ".":
                    px[i * fw + x, y] = PALETTE[ch] + (255,)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    img.save(path, optimize=True)
    print(f"{os.path.relpath(path, ROOT):24s} {len(frames)} x {fw}x{fh}  {os.path.getsize(path)} bytes")


def starfall_sheets(root):
    """STARFALL's two ships, seen from behind. Both are drawn here, from shapes rather than
    letter grids, because they are symmetrical and bank: a frame is the level ship with each
    wing raised or lowered. They are this project's own designs on purpose - see the game."""
    out = os.path.join(root, "starfall", "assets")
    os.makedirs(out, exist_ok=True)

    def put(px, w, h, x, y, c):
        if 0 <= x < w and 0 <= y < h:
            px[x, y] = c + (255,)

    # ---- ours: a twin-engine interceptor. Long wings with a little droop and a gun at each
    # tip, a pod with a canopy, two engines burning blue straight at the camera, a V tail.
    W, H, CX = 61, 33, 30
    HULL, HULL_HI, HULL_DK, EDGE = (176, 184, 204), (232, 238, 250), (108, 118, 146), (52, 58, 80)
    GLASS, GLINT, TEAM = (38, 60, 110), (150, 200, 255), (60, 190, 255)
    BURN, BURN_HI, GUN, LAMP = (70, 190, 255), (235, 250, 255), (70, 76, 98), (255, 80, 80)
    frames = []
    for bank in (-1, 0, 1):
        img, px = new_rgba(W, H)
        for side in (-1, 1):
            lift = -bank * side          # banking raises one wing and lowers the other
            for d in range(5, 30):
                x = CX + side * d
                y = 17 + int(d * 0.16 + d * 0.20 * lift)
                thick = 4 if d < 12 else 3 if d < 22 else 2
                for k in range(thick):
                    c = HULL_HI if k == 0 else HULL if k < thick - 1 else HULL_DK
                    put(px, W, H, x, y + k, c)
                if 8 <= d <= 10:
                    for k in range(thick):
                        put(px, W, H, x, y + k, TEAM)      # a team stripe near the root
            tip_y = 17 + int(29 * 0.16 + 29 * 0.20 * lift)
            for yy in range(tip_y - 3, tip_y + 5):          # the gun pod
                put(px, W, H, CX + side * 29, yy, GUN)
                put(px, W, H, CX + side * 30, yy, EDGE)
            put(px, W, H, CX + side * 29, tip_y - 4, LAMP)
            for i in range(9):                              # the V tail
                x, y = CX + side * (3 + i), 9 - i
                put(px, W, H, x, y, HULL_HI)
                put(px, W, H, x, y + 1, HULL)
                put(px, W, H, x + side, y + 1, HULL_DK)
            for yy in range(13, 25):                        # an engine, and its fire
                for xx in range(-3, 4):
                    if abs(xx) == 3 and yy in (13, 24):
                        continue
                    put(px, W, H, CX + side * 10 + xx, yy, HULL_DK if abs(xx) == 3 else GUN)
            ex, ey = CX + side * 10, 19
            for yy in range(-3, 4):
                for xx in range(-3, 4):
                    r2 = xx * xx + yy * yy
                    if r2 <= 9:
                        put(px, W, H, ex + xx, ey + yy, BURN_HI if r2 <= 2 else BURN if r2 <= 6 else EDGE)
        for yy in range(8, 26):                             # the pod, rounded
            half = 6 if 11 <= yy <= 22 else 5 if 9 <= yy <= 24 else 4
            for xx in range(-half, half + 1):
                c = HULL_HI if yy < 11 else HULL_DK if abs(xx) == half or yy > 23 else HULL
                put(px, W, H, CX + xx, yy, c)
        for yy in range(10, 16):                            # the canopy
            for xx in range(-3, 4):
                if abs(xx) == 3 and yy in (10, 15):
                    continue
                put(px, W, H, CX + xx, yy, GLASS)
        put(px, W, H, CX - 2, 11, GLINT)
        put(px, W, H, CX - 1, 11, GLINT)
        put(px, W, H, CX - 2, 12, GLINT)
        frames.append(img)
    sheet_img = Image.new("RGBA", (W * 3, H), (0, 0, 0, 0))
    for i, f in enumerate(frames):
        sheet_img.paste(f, (i * W, 0))
    path = os.path.join(out, "player.png")
    sheet_img.save(path, optimize=True)
    print(f"{os.path.relpath(path, ROOT):24s} 3 x {W}x{H}  {os.path.getsize(path)} bytes")

    # ---- theirs: a crescent. One wing swept up into two horns, a pod hung in the middle of
    # it with a single red slit of an eye, a fin below, two engines burning red. Six frames:
    # banked left, level, banked right, and the same again with the engines flickered.
    W, H, CX = 43, 29, 21
    SKIN, SKIN_HI, SKIN_DK, DARK = (150, 158, 132), (208, 214, 186), (92, 100, 82), (46, 50, 44)
    EYE, EYE_HI, FIRE_A, FIRE_B = (255, 60, 50), (255, 200, 150), (255, 80, 60), (255, 170, 60)
    frames = []
    for flick in (0, 1):
        for bank in (-1, 0, 1):
            img, px = new_rgba(W, H)
            for side in (-1, 1):
                lift = -bank * side
                for d in range(3, 21):
                    t = d / 20.0
                    x = CX + side * d
                    y = 17 - int(t * t * 13) - int(d * 0.22 * lift)
                    thick = 6 if d < 8 else 5 if d < 13 else 3 if d < 17 else 2
                    for k in range(thick):
                        c = SKIN_HI if k == 0 else SKIN_DK if k == thick - 1 else SKIN
                        put(px, W, H, x, y + k, c)
                    if d in (9, 14):
                        for k in range(1, thick - 1):
                            put(px, W, H, x, y + k, DARK)    # panel lines
                put(px, W, H, CX + side * 20, 17 - 13 - int(20 * 0.22 * lift) - 1, SKIN_HI)   # the point of the horn
                fx, fy = CX + side * 4, 23
                for yy in range(0, 3):
                    for xx in range(-1, 2):
                        put(px, W, H, fx + xx, fy + yy, (FIRE_B if flick else FIRE_A) if xx == 0 or yy == 1 else DARK)
            for yy in range(10, 24):                           # the pod
                half = 5 if 13 <= yy <= 20 else 4 if 11 <= yy <= 22 else 3
                for xx in range(-half, half + 1):
                    c = SKIN_HI if yy < 12 else SKIN_DK if abs(xx) == half or yy > 21 else SKIN
                    put(px, W, H, CX + xx, yy, c)
            for xx in range(-3, 4):                            # the eye
                put(px, W, H, CX + xx, 15, EYE)
                put(px, W, H, CX + xx, 16, EYE if abs(xx) > 1 else EYE_HI)
            for yy in range(24, 29):                           # the fin below
                put(px, W, H, CX, yy, SKIN_DK)
            frames.append(img)
    sheet_img = Image.new("RGBA", (W * 6, H), (0, 0, 0, 0))
    for i, f in enumerate(frames):
        sheet_img.paste(f, (i * W, 0))
    path = os.path.join(out, "raider.png")
    sheet_img.save(path, optimize=True)
    print(f"{os.path.relpath(path, ROOT):24s} 6 x {W}x{H}  {os.path.getsize(path)} bytes")


def main():
    jump = os.path.join(ROOT, "jump", "assets")
    os.makedirs(jump, exist_ok=True)
    sheet([HOPPER, HOPPER_BLINK, HOPPER_DEAD], os.path.join(jump, "hopper.png"))
    sheet([MONSTER, MONSTER_2], os.path.join(jump, "monster.png"))
    sheet([GRASS, ICE, STONE], os.path.join(jump, "ledges.png"))
    sheet([SPRING], os.path.join(jump, "spring.png"))
    sheet([SHOT], os.path.join(jump, "shot.png"))
    sheet([CLOUD_A, CLOUD_B], os.path.join(jump, "clouds.png"))
    racer_sheets(ROOT)
    maze_sheets(ROOT)
    starfall_sheets(ROOT)


if __name__ == "__main__":
    main()
