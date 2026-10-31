#!/usr/bin/env python3
"""The 48x48 SMDH icon: a plain generated emblem (no game artwork)."""
import sys
from PIL import Image, ImageDraw

img = Image.new("RGB", (48, 48), (24, 32, 72))
d = ImageDraw.Draw(img)
d.ellipse((6, 6, 41, 41), outline=(230, 200, 120), width=3)
d.text((15, 17), "IX", fill=(240, 230, 200))
img.save(sys.argv[1])
