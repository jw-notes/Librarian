# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
# This file is part of Librarian. Additional terms under GPLv3 section 7
# and third-party notices: see the NOTICE file.

"""Draws Librarian's package artwork: sce_sys/icon0.png (512x512) and
sce_sys/pic1.png (1920x1080). Original artwork, generated here so it carries
no third-party rights. Run: python tools/make_art.py
"""
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter, ImageFont

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "assets"
FONTS = ROOT / "third_party" / "fonts"


def gradient(size, top, bottom):
    w, h = size
    img = Image.new("RGB", size)
    px = img.load()
    for y in range(h):
        t = y / max(1, h - 1)
        c = tuple(int(top[i] + (bottom[i] - top[i]) * t) for i in range(3))
        for x in range(w):
            px[x, y] = c
    return img


def books(draw, x, y, scale):
    """Three book spines: blue, pale, and a grey one leaning on them."""
    w = int(46 * scale)
    gap = int(12 * scale)
    h = int(300 * scale)
    r = int(10 * scale)
    draw.rounded_rectangle([x, y + int(50 * scale), x + w, y + h], r, fill=(76, 141, 255))
    draw.rounded_rectangle([x + w + gap, y, x + 2 * w + gap, y + h], r, fill=(186, 205, 238))
    # Bands on the spines.
    for bx in (x, x + w + gap):
        top = y + (int(50 * scale) if bx == x else 0)
        draw.rectangle([bx, top + int(40 * scale), bx + w, top + int(52 * scale)], fill=(20, 26, 40))
    # The leaning book: a rectangle rotated about its bottom-left corner,
    # resting against the pale one.
    import math
    a = math.radians(20)
    bx, by = x + 2 * (w + gap) + int(4 * scale), y + h
    corners = []
    for px, py in ((0, 0), (w, 0), (w, h - int(20 * scale)), (0, h - int(20 * scale))):
        # Tilt to the right: up the spine is (sin a, -cos a).
        corners.append((bx + px * math.cos(a) + py * math.sin(a), by + px * math.sin(a) - py * math.cos(a)))
    draw.polygon(corners, fill=(140, 150, 170))
    band = []
    for px, py in ((0, h - int(80 * scale)), (w, h - int(80 * scale)), (w, h - int(68 * scale)), (0, h - int(68 * scale))):
        band.append((bx + px * math.cos(a) + py * math.sin(a), by + px * math.sin(a) - py * math.cos(a)))
    draw.polygon(band, fill=(20, 26, 40))


def icon():
    img = gradient((512, 512), (34, 44, 66), (14, 18, 28)).convert("RGBA")
    d = ImageDraw.Draw(img)
    books(d, 110, 70, 1.0)
    font = ImageFont.truetype(str(FONTS / "NotoSans-Bold.ttf"), 64)
    text = "Librarian"
    tw = d.textlength(text, font=font)
    d.text(((512 - tw) / 2, 392), text, font=font, fill=(236, 240, 247))
    img.convert("RGB").save(OUT / "icon0.png")


def background():
    img = gradient((1920, 1080), (26, 31, 43), (12, 15, 22)).convert("RGBA")
    glow = Image.new("RGBA", (1920, 1080), (0, 0, 0, 0))
    gd = ImageDraw.Draw(glow)
    gd.ellipse([1150, 120, 1850, 820], fill=(76, 141, 255, 60))
    glow = glow.filter(ImageFilter.GaussianBlur(120))
    img.alpha_composite(glow)
    d = ImageDraw.Draw(img)
    books(d, 1330, 330, 1.25)
    img.convert("RGB").save(OUT / "pic1.png")


if __name__ == "__main__":
    OUT.mkdir(exist_ok=True)
    icon()
    background()
    print("wrote", OUT / "icon0.png", OUT / "pic1.png")
