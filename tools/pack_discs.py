#!/usr/bin/env python3
"""Pack the four raw disc images into one discs.bin for the packed ROM (make ds-packed), read by port/psx/cd.c.

The images hold much the same game data four times, and most of a raw sector is framing the CD layer can rebuild,
so the pack keeps each distinct sector once, as one of two kinds of record:
  - data: the 2048 bytes of user data of a Mode 2 Form 1 sector with the usual data subheader (00 00 08 00), outside
    the movies. The reader rebuilds sync, address and subheader; EDC/ECC read back as zero (game data is read
    2048 bytes a sector; port/psx/cd.c fails loudly if a 2340-byte read reaches them).
  - raw: the whole 2352-byte sector (the movies' STR/MBG sectors, which stream at 2340 bytes a sector, and anything
    that doesn't rebuild). Its address bytes are the reader's, from the LBA.

Layout (little-endian):
  "FF9D", version 1, disc count (4), data pool offset, raw pool offset, 12 bytes reserved
  per disc: run table offset, run count, sector count (0: not in this pack)
  run tables: {first LBA, sector count | flags << 28, record offset in its pool, 0}, by LBA, covering the disc
      flags: 1 = data record (else raw), 2 = every sector of the run is the same record (else consecutive records)
  data pool (2048-byte records, 2048-aligned), raw pool (2352-byte records)

Game data: never commit the output."""
import argparse
import hashlib
import os
from pathlib import Path
import struct
import tempfile

RAW = 2352
USER = 2048
SYNC = b'\x00' + b'\xff' * 10 + b'\x00'
DATA_SUBHEADER = b'\x00\x00\x08\x00' * 2
F_DATA, F_REPEAT = 1, 2
MAX_RUN = (1 << 28) - 1


def bcd(v):
    return (v // 10) << 4 | v % 10


def msf(lba):
    a = lba + 150
    return bytes((bcd(a // 4500), bcd(a // 75 % 60), bcd(a % 75)))


def le32(b, o):
    return struct.unpack_from('<I', b, o)[0]


def movie_ranges(img):
    """LBA ranges of the STR/MBG files (ISO9660 directory walk over the raw image)."""
    def user(lba, size):
        out = bytearray()
        for i in range((size + USER - 1) // USER):
            img.seek((lba + i) * RAW + 24)
            out += img.read(USER)
        return out[:size]
    pvd = user(16, USER)
    if pvd[:6] != b'\x01CD001':
        raise SystemExit('not a raw Mode 2 ISO9660 image')
    ranges = []

    def walk(lba, size):
        d = user(lba, size)
        i = 0
        while i < len(d):
            n = d[i]
            if not n:
                i = (i // USER + 1) * USER
                continue
            e = d[i:i + n]
            i += n
            name = e[33:33 + e[32]]
            if name in (b'\0', b'\1'):
                continue
            pos, size_ = le32(e, 2), le32(e, 10)
            if e[25] & 2:
                walk(pos, size_)
            elif name.upper().endswith((b'.STR;1', b'.MBG;1')):
                ranges.append((pos, pos + (size_ + USER - 1) // USER))
    walk(le32(pvd, 158), le32(pvd, 166))
    return ranges


def image_path(disks, n):
    for p in (disks / 'us' / 'raw' / f'FF9 CD{n}.img', disks / 'raw' / f'FF9 CD{n}.img'):
        if p.exists():
            return p
    raise SystemExit(f'no image for disc {n} under {disks}')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--img', type=Path, required=True, help='folder holding raw/FF9 CDn.img (or us/raw/)')
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--discs', default='1234', help='the discs to pack (others: empty), e.g. 1 or 13')
    a = ap.parse_args()

    tmpdir = a.out.parent
    tmpdir.mkdir(parents=True, exist_ok=True)
    data_pool = tempfile.TemporaryFile(dir=tmpdir)
    raw_pool = tempfile.TemporaryFile(dir=tmpdir)
    seen = {}  # (kind, digest) -> record index in its pool
    counts = [0, 0]  # raw, data records
    discs = []
    total = 0
    for n in range(1, 5):
        if str(n) not in a.discs:
            discs.append(([], 0))
            continue
        path = image_path(a.img, n)
        sectors = os.path.getsize(path) // RAW
        runs = []
        with open(path, 'rb') as img:
            movies = movie_ranges(img)
            in_movie = bytearray(sectors)
            for lo, hi in movies:
                in_movie[lo:min(hi, sectors)] = b'\1' * (min(hi, sectors) - lo)
            img.seek(0)
            for lba in range(sectors):
                s = img.read(RAW)
                if s[:12] != SYNC or s[12:15] != msf(lba):
                    raise SystemExit(f'disc {n} sector {lba}: not a raw sector at its own address')
                data = not in_movie[lba] and s[15] == 2 and s[16:24] == DATA_SUBHEADER
                payload = s[24:24 + USER] if data else s[:12] + b'\0\0\0' + s[15:]
                key = (data, hashlib.blake2b(payload, digest_size=16).digest())
                rec = seen.get(key)
                if rec is None:
                    rec = seen[key] = counts[data]
                    counts[data] += 1
                    (data_pool if data else raw_pool).write(payload)
                flags = F_DATA if data else 0
                if runs:
                    r = runs[-1]
                    if r[3] & F_DATA == flags and r[1] < MAX_RUN:
                        if r[1] == 1 and rec == r[2]:
                            r[3] |= F_REPEAT
                        if (r[3] & F_REPEAT and rec == r[2]) or (not r[3] & F_REPEAT and rec == r[2] + r[1]):
                            r[1] += 1
                            continue
                runs.append([lba, 1, rec, flags])
        total += sectors
        discs.append((runs, sectors))
        print(f'disc {n}: {sectors} sectors, {len(runs)} runs, {len(movies)} movies; '
              f'pool now {counts[1]} data + {counts[0]} raw records')

    head = 32 + 12 * len(discs)
    tables = []
    off = head
    for runs, sectors in discs:
        tables.append(off)
        off += 16 * len(runs)
    data_base = (off + USER - 1) // USER * USER
    raw_base = data_base + counts[1] * USER
    tmp = a.out.with_suffix('.tmp')
    with open(tmp, 'wb') as out:
        out.write(b'FF9D' + struct.pack('<IIII', 1, len(discs), data_base, raw_base) + bytes(12))
        for (runs, sectors), t in zip(discs, tables):
            out.write(struct.pack('<III', t, len(runs), sectors))
        for runs, _ in discs:
            for lba, count, rec, flags in runs:
                size = USER if flags & F_DATA else RAW
                out.write(struct.pack('<IIII', lba, count | flags << 28, rec * size, 0))
        out.write(bytes(data_base - out.tell()))
        for pool in (data_pool, raw_pool):
            pool.seek(0)
            while chunk := pool.read(1 << 22):
                out.write(chunk)
        size = out.tell()
    if size >= 1 << 31:  # (the reader's offsets are a 32-bit long on the DS)
        tmp.unlink()
        raise SystemExit(f'{size} bytes: over 2 GiB')
    tmp.replace(a.out)
    print(f'{a.out}: {size / 1e6:.0f} MB for {total * RAW / 1e6:.0f} MB of images')


if __name__ == '__main__':
    main()
