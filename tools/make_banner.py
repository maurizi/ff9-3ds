#!/usr/bin/env python3
"""The CIA's HOME Menu banner: a plain generated 256x128 image (no game artwork) and a short chime (bannertool
makebanner).   make_banner.py OUT.png OUT.wav"""
import math
import struct
import sys
import wave

from PIL import Image, ImageDraw

img = Image.new("RGB", (256, 128), (24, 32, 72))
d = ImageDraw.Draw(img)
for i in range(128):  # (a dusk gradient)
    d.line((0, i, 255, i), fill=(24 + i // 4, 32 + i // 6, 72 + i // 3))
d.ellipse((96, 32, 160, 96), outline=(230, 200, 120), width=4)
d.text((119, 57), "IX", fill=(240, 230, 200))
img.save(sys.argv[1])

rate = 32728
with wave.open(sys.argv[2], "wb") as w:
    w.setnchannels(1)
    w.setsampwidth(2)
    w.setframerate(rate)
    frames = bytearray()
    for i in range(int(rate * 1.2)):
        t = i / rate
        v = sum(math.sin(2 * math.pi * f * t) for f in (523.25, 659.25, 783.99)) / 3 * math.exp(-3 * t)
        frames += struct.pack("<h", int(v * 12000))
    w.writeframes(bytes(frames))
