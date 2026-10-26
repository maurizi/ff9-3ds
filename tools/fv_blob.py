#!/usr/bin/env python3
"""Pack the FastVideoDS decoder blob for the DS (port/ds/movie_fv.c), from its link (port/ds/fv/fv_blob.ld, -q).

fv.bin: 'FVBL', image bytes, list bytes (the display list's size), list prefix bytes (its non-zero head), blob
relocation count, list relocation count; then the blob relocations (offsets of words to add the blob's load address
to), the list relocations (words to add the list's address to, less 0x10000000), the image, the list prefix.
Usage: fv_blob.py BLOB.elf OUT.bin
"""
import os, re, struct, subprocess, sys

elf, out = sys.argv[1:3]
LIST = 0x10000000
heads = subprocess.run(['arm-none-eabi-objdump', '-h', elf], capture_output=True, text=True, check=True).stdout
sizes = {m[1]: int(m[2], 16) for m in re.finditer(r'^\s*\d+\s+(\S+)\s+([0-9a-f]+)', heads, re.M)}
def section(name):
    subprocess.run(['arm-none-eabi-objcopy', '-O', 'binary', '-j', name, elf, out + '.tmp'], check=True)
    data = open(out + '.tmp', 'rb').read()
    return data
image, mclist = section('.image'), section('.mclist')
assert len(image) == sizes['.image'] and len(mclist) == sizes['.mclist'], 'section extraction'
rels = subprocess.run(['arm-none-eabi-readelf', '-rW', elf], capture_output=True, text=True, check=True).stdout
blob_rel, list_rel = [], []
PC_RELATIVE = {'R_ARM_CALL', 'R_ARM_JUMP24', 'R_ARM_PC24', 'R_ARM_V4BX', 'R_ARM_THM_CALL', 'R_ARM_THM_JUMP24',
               'R_ARM_PREL31', 'R_ARM_REL32'}
for line in rels.splitlines():
    f = line.split()
    if len(f) < 3 or not re.fullmatch(r'[0-9a-f]{8}', f[0]):
        continue
    offset, kind = int(f[0], 16), f[2]
    if kind in PC_RELATIVE:
        continue
    assert kind == 'R_ARM_ABS32', f'{kind} at {offset:#x}'
    if offset >= LIST:
        raise SystemExit(f'fv_blob.py: a relocation inside the display list at {offset:#x}')
    word = struct.unpack_from('<I', image, offset)[0]
    (list_rel if word >= LIST else blob_rel).append(offset)
prefix = len(mclist.rstrip(b'\0') + b'\0\0\0') & ~3
with open(out, 'wb') as o:
    o.write(struct.pack('<4s5I', b'FVBL', len(image), len(mclist), prefix, len(blob_rel), len(list_rel)))
    o.write(struct.pack(f'<{len(blob_rel)}I', *blob_rel))
    o.write(struct.pack(f'<{len(list_rel)}I', *list_rel))
    o.write(image)
    o.write(mclist[:prefix])
os.remove(out + '.tmp')
print(f'fv.bin: {len(image)} bytes of code and tables, a {len(mclist)}-byte display list, '
      f'{len(blob_rel)}+{len(list_rel)} relocations')
