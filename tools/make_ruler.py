#!/usr/bin/env python3
"""Generate a geometry ruler test image for the B4 (203 dpi, 8 px/mm).

    make_ruler.py out.png [--width 832] [--height 1200] [--tag "v5"]

Prints column numbers along the top/middle/bottom, row numbers down the
left/right, mm ticks on every edge, a border, a diagonal and a centre cross.
From the printed label we can read: which columns land on the paper
(printhead width + centering), vertical scale, and skew.
"""
import argparse

from PIL import Image, ImageDraw, ImageFont

FONT_CANDIDATES = [
    "/System/Library/Fonts/Supplemental/Arial Bold.ttf",
    "/System/Library/Fonts/Supplemental/Arial.ttf",
    "/System/Library/Fonts/Helvetica.ttc",
]


def font(size):
    for f in FONT_CANDIDATES:
        try:
            return ImageFont.truetype(f, size)
        except OSError:
            continue
    return ImageFont.load_default()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--width", type=int, default=832)
    ap.add_argument("--height", type=int, default=1200)
    ap.add_argument("--tag", default="")
    a = ap.parse_args()
    W, H = a.width, a.height
    img = Image.new("1", (W, H), 1)
    d = ImageDraw.Draw(img)
    f_small, f_big = font(22), font(60)

    # border (2 px so a 1 px loss at the edge is still visible)
    d.rectangle([0, 0, W - 1, H - 1], outline=0, width=2)

    # mm ticks: top & bottom edges (columns), left & right edges (rows)
    for x in range(0, W, 8):
        mm = x // 8
        ln = 40 if mm % 10 == 0 else 24 if mm % 5 == 0 else 12
        d.line([x, 0, x, ln], fill=0)
        d.line([x, H - 1 - ln, x, H - 1], fill=0)
    for y in range(0, H, 8):
        mm = y // 8
        ln = 40 if mm % 10 == 0 else 24 if mm % 5 == 0 else 12
        d.line([0, y, ln, y], fill=0)
        d.line([W - 1 - ln, y, W - 1, y], fill=0)

    # column numbers every 10 mm (80 px) at three heights
    for yy in (48, H // 2 - 60, H - 90):
        for x in range(0, W, 80):
            d.text((x + 3, yy), str(x), font=f_small, fill=0)
            d.line([x, yy - 6, x, yy + 26], fill=0)
    # row numbers every 10 mm down the left and right
    for y in range(0, H, 80):
        d.text((48, y + 2), str(y), font=f_small, fill=0)
        tw = d.textlength(str(y), font=f_small)
        d.text((W - 48 - tw, y + 2), str(y), font=f_small, fill=0)

    # diagonal + centre cross
    d.line([0, 0, W - 1, H - 1], fill=0, width=2)
    cx, cy = W // 2, H // 2
    d.line([cx - 60, cy, cx + 60, cy], fill=0, width=3)
    d.line([cx, cy - 60, cx, cy + 60], fill=0, width=3)

    # label
    txt = f"B4 ruler {W}x{H} {a.tag}".strip()
    d.text((cx - d.textlength(txt, font=f_big) / 2, cy + 80), txt, font=f_big, fill=0)
    d.text((cx - 150, cy + 160), "8 px = 1 mm   203 dpi", font=f_small, fill=0)

    img.save(a.out)
    print(f"wrote {a.out} {W}x{H}")


if __name__ == "__main__":
    main()
