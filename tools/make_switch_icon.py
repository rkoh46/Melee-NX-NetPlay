#!/usr/bin/env python3
"""Draws the Homebrew Menu icon (256x256 JPEG): the Smash Bros. emblem on Melee's dark blue grid.

The emblem is a trademark of Nintendo / HAL Laboratory, so it is drawn here at build time into
build/icon.jpg and never committed; port/CMakeLists.txt uses it when present (switch/icon.jpg, an
unbranded fallback, otherwise). Usage: make_switch_icon.py <out.jpg> [fallback|replays]
  fallback: the unbranded icon (what switch/icon.jpg holds); replays: the replay app's icon.
SPDX-License-Identifier: GPL-3.0-or-later
"""
import sys
from PIL import Image, ImageDraw, ImageFilter, ImageFont

S = 4            # supersampling
N = 256 * S
BG = (10, 16, 46)
GRID = (28, 44, 104)
WHITE = (244, 246, 252)
YELLOW = (248, 202, 72)

def font(size):
    for path in ("/System/Library/Fonts/Supplemental/Futura.ttc", "/System/Library/Fonts/Helvetica.ttc"):
        try:
            return ImageFont.truetype(path, size, index=4 if path.endswith("Futura.ttc") else 1)
        except Exception:
            continue
    return ImageFont.load_default()

def draw(out, branded, word="NETPLAY"):
    img = Image.new("RGB", (N, N), BG)
    d = ImageDraw.Draw(img)
    # Melee's menu grid, slightly brighter toward the middle (one neutral, a tone shift only).
    step = 32 * S
    for x in range(0, N + 1, step):
        d.line([(x, 0), (x, N)], fill=GRID, width=S)
    for y in range(0, N + 1, step):
        d.line([(0, y), (N, y)], fill=GRID, width=S)
    glow = Image.new("L", (N, N), 0)
    ImageDraw.Draw(glow).ellipse([N * 0.12, N * 0.06, N * 0.88, N * 0.82], fill=70)
    glow = glow.filter(ImageFilter.GaussianBlur(40 * S))
    img = Image.composite(Image.new("RGB", (N, N), (40, 64, 150)), img, glow)
    d = ImageDraw.Draw(img)
    cx, cy, r = N * 0.5, N * 0.42, N * 0.30
    if branded:
        # The emblem: a disc cut by one vertical bar left of centre and one horizontal bar below it.
        d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=WHITE)
        bar = r * 0.16
        vx = cx - r * 0.28
        hy = cy + r * 0.30
        d.rectangle([vx - bar / 2, cy - r - S, vx + bar / 2, cy + r + S], fill=BG)
        d.rectangle([cx - r - S, hy - bar / 2, cx + r + S, hy + bar / 2], fill=BG)
    else:
        d.ellipse([cx - r, cy - r, cx + r, cy + r], outline=WHITE, width=int(r * 0.12))
        d.text((cx, cy), "NX", font=font(int(r * 0.95)), fill=WHITE, anchor="mm")
    # Wordmark in Melee's italic display style: yellow plate, dark text.
    f = font(int(30 * S))
    text = word
    tw = d.textlength(text, font=f)
    py0, py1 = N * 0.80, N * 0.93
    skew = (py1 - py0) * 0.35
    plate = [(cx - tw / 2 - 18 * S + skew, py0), (cx + tw / 2 + 18 * S + skew, py0), (cx + tw / 2 + 18 * S - skew, py1), (cx - tw / 2 - 18 * S - skew, py1)]
    d.polygon(plate, fill=YELLOW)
    d.text((cx - tw / 2, (py0 + py1) / 2), text, font=f, fill=(20, 16, 6), anchor="lm")
    img = img.resize((256, 256), Image.LANCZOS)
    img.save(out, "JPEG", quality=94)

if __name__ == "__main__":
    mode = sys.argv[2] if len(sys.argv) > 2 else ""
    draw(sys.argv[1], branded=mode != "fallback", word="REPLAYS" if mode == "replays" else "NETPLAY")
