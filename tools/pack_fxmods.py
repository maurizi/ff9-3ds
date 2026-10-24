#!/usr/bin/env python3
"""Pack the DS's separately linked battle effect modules (Makefile, ds_FXOVL) into fxmods.bin:

  pack_fxmods.py FXID.s MAIN.elf OUT MODULE.elf...   (modules in the order of gen_link's psx_fxmods indices)

Layout (little-endian words): 'FXP1', the main executable's build id, the module count, psx_fx_area's address,
then (offset, size) per module, then each module's image as linked at psx_fx_area (header first; .bss follows it
and is zeroed at load). Derived from game data: never commit it.
"""
import re
import struct
import subprocess
import sys
import tempfile


def main():
    fxid, main_elf, out, mods = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4:]
    build_id = int(re.search(r"\.4byte 0x([0-9A-F]+)", open(fxid).read()).group(1), 16)
    nm = subprocess.run(["arm-none-eabi-nm", main_elf], check=True, capture_output=True, text=True).stdout
    base = int(re.search(r"^([0-9a-f]+) \w psx_fx_area$", nm, re.M).group(1), 16)
    images = []
    with tempfile.TemporaryDirectory() as tmp:
        for i, elf in enumerate(mods):
            path = f"{tmp}/{i}.bin"
            subprocess.run(["arm-none-eabi-objcopy", "-O", "binary", "-R", ".bss", elf, path], check=True)
            images.append(open(path, "rb").read())
    header = struct.pack("<4I", 0x31505846, build_id, len(images), base)
    offset = len(header) + 8 * len(images)
    index = b""
    for img in images:
        index += struct.pack("<2I", offset, len(img))
        offset += len(img)
    with open(out, "wb") as f:
        f.write(header + index + b"".join(images))


main()
