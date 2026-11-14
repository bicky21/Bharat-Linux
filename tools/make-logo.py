#!/usr/bin/env python3
"""
Turns the Bharat Linux logo (docs/logo.png) into the pictures the boot
screen, login screen and desktop show.

Output, in rootfs/usr/share/bharat/:
  logo.img            the full logo; its dark background is see-through,
                      so it blends into any dark screen
  mark26/42/96.img    the penguin in a circle, for small places

.img format (simple on purpose, so C can read it without a library):
  4 bytes width, 4 bytes height (little-endian), then for every pixel
  4 bytes: red, green, blue, alpha.

Run:   python3 tools/make-logo.py      (needs: pip install pillow)
The .img files are stored in git, so building the OS does not need Python.
"""
import struct
from PIL import Image, ImageDraw, ImageFilter

SRC = "docs/logo.png"
OUT = "rootfs/usr/share/bharat/"
BG = (4, 6, 11)        # the dark colour of the logo's own background


def save(img, name):
    img = img.convert("RGBA")
    with open(OUT + name, "wb") as f:
        f.write(struct.pack("<II", img.width, img.height))
        f.write(img.tobytes())


src = Image.open(SRC).convert("RGB")
sw, sh = src.size

# full logo: cut away the grey smoke at the edges by fading into BG
full = src.crop((int(sw * 0.124), int(sh * 0.03), int(sw * 0.876), int(sh * 0.967)))
w, h = full.size
mask = Image.new("L", (w, h), 0)
ImageDraw.Draw(mask).ellipse((-int(w * 0.08), -int(h * 0.10), int(w * 1.08), int(h * 1.10)), fill=255)
mask = mask.filter(ImageFilter.GaussianBlur(60))
full = Image.composite(full, Image.new("RGB", (w, h), BG), mask)
full = full.resize((420, round(420 * h / w)), Image.LANCZOS).convert("RGBA")

# Near-black pixels become see-through: the darker, the more transparent.
# (The penguin's black body stays visible on a dark screen anyway.)
pixels = full.load()
for y in range(full.height):
    for x in range(full.width):
        r, g, b, _ = pixels[x, y]
        light = max(r, g, b)
        alpha = max(0, min(255, (light - 16) * 255 // 36))
        pixels[x, y] = (r, g, b, alpha)

save(full, "logo.img")

# small mark: the penguin in a round dark badge
cx, cy, r = int(sw * 0.5), int(sh * 0.361), int(sh * 0.239)
mark = src.crop((cx - r, cy - r, cx + r, cy + r))
circle = Image.new("L", mark.size, 0)
ImageDraw.Draw(circle).ellipse((0, 0, 2 * r - 1, 2 * r - 1), fill=255)
mark.putalpha(circle)

for size in (26, 42, 96):
    # shrinking the big picture keeps the round edge smooth
    save(mark.resize((size, size), Image.LANCZOS), "mark%d.img" % size)

print("Wrote logo.img and mark26/42/96.img to " + OUT)
