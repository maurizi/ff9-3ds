#!/usr/bin/env python3
"""Pack the DS's native code overlays (sections .psxovl_<id> of the main executable, placed by gen_link.py's
psx_ovl_areas.ld outside its load segments) into ovls.bin:

  pack_overlays.py FXID.s MAIN.elf OUT

Layout (little-endian words): 'OVP1', the main executable's build id, the overlay count, then (id, address, size,
offset) per overlay, then the images. port/ds/ovl_pack.c reads one when the game loads that overlay. Derived from
game data: never commit it.
"""
import re
import struct
import subprocess
import sys
import tempfile


def main():
    fxid, elf, out = sys.argv[1:4]
    build_id = int(re.search(r"\.4byte 0x([0-9A-F]+)", open(fxid).read()).group(1), 16)
    heads = subprocess.run(["arm-none-eabi-objdump", "-h", elf], check=True, capture_output=True, text=True).stdout
    # (.itcmovl_<id>: that overlay's ITCM part, entry 0x100 + id; .ramovl_<id>: its part in psx_ram, 0x200 + id)
    sections = [((m.group(1), m.group(2)), int(m.group(4), 16), int(m.group(3), 16))
                for m in re.finditer(r"^\s*\d+\s+\.(psx|itcm|ram)ovl_([0-9a-f]+)\s+([0-9a-f]+)\s+([0-9a-f]+)", heads, re.M)]
    images = []
    with tempfile.TemporaryDirectory() as tmp:
        for name, vma, size in sections:
            kind, ovl = name
            if not size:
                continue
            path = f"{tmp}/{kind}{ovl}.bin"
            subprocess.run(["arm-none-eabi-objcopy", "-O", "binary", "-j", f".{kind}ovl_{ovl}", elf, path], check=True)
            data = open(path, "rb").read()
            if len(data) != size:
                sys.exit(f"pack_overlays.py: .{kind}ovl_{ovl} is {size} bytes but extracted {len(data)}")
            images.append((int(ovl, 16) | {"itcm": 0x100, "ram": 0x200}.get(kind, 0), vma, data))
    header = struct.pack("<3I", 0x3150564F, build_id, len(images))
    offset = len(header) + 16 * len(images)
    index = b""
    for ovl, vma, data in images:
        index += struct.pack("<4I", ovl, vma, len(data), offset)
        offset += len(data)
    with open(out, "wb") as f:
        f.write(header + index + b"".join(d for _, _, d in images))


main()
