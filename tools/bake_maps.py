#!/usr/bin/env python3
"""Bake every field's bottom-screen area map from the disc images: build/maps.bin, read by port/game/field_map.c.

The walkmesh, the exit regions and the save moogles' and shopkeepers' places come from the decomp's offline reader
(ff9-decomp tools/field_maps.py, decomp R39: ff9-decomp docs/r32/3ds-requests-9.md), so the map is the field's data,
not whatever the game had built when the bottom screen looked (exits made late, triangles switched later).

Per field (one record per field id: a field on several discs has the same walkmesh and regions):
  - walkable triangles: flags & 1, and the mask byte without 0x80 (only the scripts' walk-outs) or 0x40
  - exits: every region whose functions go to another field or the world map (whatever the story state)
  - areas: walkable triangles connected through their neighbours, a walk ending at an exit's triangles (past an exit
    is reached by leaving); each triangle carries the areas it shows in, so the player's triangle picks the map
  - markers: save moogles (menu type 4) and shopkeepers (type 2) where the script first places them

Layout (little-endian):
  "FF9M", version 1, record count, 0
  per record: u16 field, u16 triangle count, u32 offset (from the file's start)
  record: u16 triangle count, u8 area count, u8 marker count, 4 bytes 0;
          areas {s16 x0, z0, x1, z1}; triangles {s16 x[3], s16 z[3], u32 areas, u8 exit, 3 bytes 0} (by walkmesh
          index: the game's own order); markers {s16 x, s16 z, u8 kind (2 shop, 4 save), 3 bytes 0}

    tools/bake_maps.py --decomp ../ff9-decomp [--img DIR] --out build/maps.bin

Game data: never commit the output."""
import argparse
import importlib
import os
import struct
import sys

EXIT_NEAR = 48   # a triangle this near an exit polygon is shaded as the exit (some regions stop short of the mesh)
MAX_AREAS = 32
MAX_TRIS = 1024  # field_map.c's MAX_TRIS


def walkable(t):
    return t['flags'] & 1 and not (t['mask'] & 0xC0)


def xz(t):
    return [(v[0], v[2]) for v in t['world']]


def in_convex(poly, x, z):
    pos = neg = False
    n = len(poly)
    for i in range(n):
        ax, az = poly[i]
        bx, bz = poly[(i + 1) % n]
        c = (bz - az) * (x - ax) - (bx - ax) * (z - az)
        pos |= c > 0
        neg |= c < 0
    return not (pos and neg)


def near_poly(poly, x, z, d):
    if in_convex(poly, x, z):
        return True
    n = len(poly)
    for i in range(n):
        ax, az = poly[i]
        dx, dz = poly[(i + 1) % n][0] - ax, poly[(i + 1) % n][1] - az
        l2 = dx * dx + dz * dz
        t = 0 if not l2 else max(0.0, min(1.0, ((x - ax) * dx + (z - az) * dz) / l2))
        px, pz = ax + dx * t - x, az + dz * t - z
        if px * px + pz * pz <= d * d:
            return True
    return False


def bake(dump):
    w = dump['walkmesh']
    if not w or not w.get('triangles'):
        return None
    tris = w['triangles']
    if len(tris) > MAX_TRIS:
        print(f"field {dump['field']}: {len(tris)} triangles, past {MAX_TRIS}: skipped", file=sys.stderr)
        return None
    exits = [e['polygon'] for e in dump['exits'] if e.get('polygon')]
    pts = [xz(t) for t in tris]
    cen = [(sum(p[0] for p in q) / 3, sum(p[1] for p in q) / 3) for q in pts]
    ok = [walkable(t) for t in tris]
    in_exit = [ok[i] and any(in_convex(p, *cen[i]) for p in exits) for i in range(len(tris))]
    near = [ok[i] and any(any(near_poly(p, x, z, EXIT_NEAR) for (x, z) in pts[i] + [cen[i]]) for p in exits)
            for i in range(len(tris))]
    member = [0] * len(tris)
    areas = []
    for start in range(len(tris)):
        if not ok[start] or in_exit[start] or member[start] or len(areas) == MAX_AREAS:
            continue
        a = len(areas)
        seen, stack = {start}, [start]
        while stack:
            i = stack.pop()
            for nb in tris[i]['neighbors']:
                if nb < 0 or nb >= len(tris) or nb in seen or not ok[nb]:
                    continue
                seen.add(nb)
                if not in_exit[nb]:
                    stack.append(nb)
        xs = [p[0] for i in seen for p in pts[i]]
        zs = [p[1] for i in seen for p in pts[i]]
        if max(xs) <= min(xs) or max(zs) <= min(zs):
            continue
        for i in seen:
            member[i] |= 1 << a
        areas.append((min(xs), min(zs), max(xs), max(zs)))
    marks = []
    for n in dump['npcs']:
        kinds = {m[0] for m in n.get('menus', []) if isinstance(m[0], int)}
        kind = 4 if 4 in kinds else 2 if 2 in kinds else 0
        pos = next((p['pos'] for p in n.get('positions', []) if p['op'] == 'SET_POS_XZ' and None not in p['pos']), None)
        if pos is None:
            pos = next((p['pos'] for p in n.get('positions', [])
                        if p['op'] == 'SET_POS' and p['pos'][0] is not None and p['pos'][2] is not None), None)
            pos = pos and (pos[0], pos[2])
        if kind and pos:
            marks.append((pos[0], pos[1], kind))
    out = struct.pack('<HBB4x', len(tris), len(areas), len(marks))
    for a in areas:
        out += struct.pack('<4h', *a)
    for i in range(len(tris)):
        x = [p[0] for p in pts[i]]
        z = [p[1] for p in pts[i]]
        out += struct.pack('<6hIB3x', *x, *z, member[i], 1 if near[i] else 0)
    for m in marks[:16]:
        out += struct.pack('<hhB3x', *m)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--decomp', default=os.environ.get('FF9_DECOMP', '../ff9-decomp'), help='ff9-decomp checkout')
    ap.add_argument('--img', help='disc images (else the reader finds them)')
    ap.add_argument('--out', default='build/maps.bin')
    a = ap.parse_args()
    sys.path.insert(0, os.path.join(a.decomp, 'tools'))
    fm = importlib.import_module('field_maps')
    discs = fm.open_discs([1, 2, 3, 4], a.img)
    if not discs:
        sys.exit('no disc image found')
    records = {}
    for disc, img in discs:
        for ent in fm.field_files(img):
            if ent[0] in records:
                continue
            d = fm.dump_field(img, disc, ent, True)
            r = bake(d)
            if r is not None:
                records[ent[0]] = r
    fields = sorted(records)
    head = b'FF9M' + struct.pack('<III', 1, len(fields), 0)
    at = len(head) + 8 * len(fields)
    index, body = b'', b''
    for f in fields:
        r = records[f]
        index += struct.pack('<HHI', f, struct.unpack_from('<H', r)[0], at + len(body))
        body += r
    os.makedirs(os.path.dirname(a.out) or '.', exist_ok=True)
    with open(a.out, 'wb') as fp:
        fp.write(head + index + body)
    print(f'{a.out}: {len(fields)} fields, {len(head) + len(index) + len(body)} bytes')


if __name__ == '__main__':
    main()
