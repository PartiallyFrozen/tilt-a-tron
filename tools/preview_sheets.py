"""Tiles the sprite sheets of one game, scaled up, into build/shots/<game>_sheets.png for a quick look.

    python tools/preview_sheets.py racer
"""
import os
import sys

from PIL import Image

game = sys.argv[1] if len(sys.argv) > 1 else "jump"
root = os.path.join(os.path.dirname(__file__), "..", "games", game, "assets")
names = sorted(n for n in os.listdir(root) if n.endswith(".png"))
imgs = [Image.open(os.path.join(root, n)).convert("RGBA") for n in names]
W = sum(i.width for i in imgs) + 6 * (len(imgs) + 1)
H = max(i.height for i in imgs) + 12
sheet = Image.new("RGBA", (W, H), (90, 150, 220, 255))
x = 6
for i in imgs:
    sheet.paste(i, (x, H - 6 - i.height), i)
    x += i.width + 6
scale = max(1, min(6, 900 // W))
sheet = sheet.resize((W * scale, H * scale), Image.NEAREST)
out = os.path.join(os.path.dirname(__file__), "..", "build", "shots", f"{game}_sheets.png")
os.makedirs(os.path.dirname(out), exist_ok=True)
sheet.save(out)
print(out, names)
