"""Generates the pixel-art sprite sheets for the games as PNG files.

The PNGs under components/games/assets/ are what the firmware embeds. They can be
edited in any image editor (keep the frame size and count); this script just
gives them a first version from the letter grids below.

    python tools/make_sprites.py
"""
import os

from PIL import Image

ROOT = os.path.join(os.path.dirname(__file__), "..", "components", "games", "assets")

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


def main():
    jump = os.path.join(ROOT, "jump")
    sheet([HOPPER, HOPPER_BLINK, HOPPER_DEAD], os.path.join(jump, "hopper.png"))
    sheet([MONSTER, MONSTER_2], os.path.join(jump, "monster.png"))
    sheet([GRASS, ICE, STONE], os.path.join(jump, "ledges.png"))
    sheet([SPRING], os.path.join(jump, "spring.png"))
    sheet([SHOT], os.path.join(jump, "shot.png"))
    sheet([CLOUD_A, CLOUD_B], os.path.join(jump, "clouds.png"))


if __name__ == "__main__":
    main()
