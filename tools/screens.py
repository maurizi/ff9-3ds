#!/usr/bin/env python3
"""Crop Azahar screenshots (tools/azahar.sh, default layout at 1x) into the 3DS's screens, and montage several.
   tools/screens.py OUT.png SHOT.png [SHOT.png ...]   -> each shot's top screen above its bottom screen, side by side
"""
import sys
from PIL import Image

TOP = (312, 194, 712, 434)
BOTTOM = (352, 434, 672, 674)

def screens(path):
    im = Image.open(path).convert("RGB")
    out = Image.new("RGB", (400, 480))
    out.paste(im.crop(TOP), (0, 0))
    out.paste(im.crop(BOTTOM), (40, 240))
    return out

if __name__ == "__main__":
    shots = [screens(p) for p in sys.argv[2:]]
    out = Image.new("RGB", (404 * len(shots), 480), (40, 40, 40))
    for i, s in enumerate(shots):
        out.paste(s, (i * 404, 0))
    out.save(sys.argv[1])
