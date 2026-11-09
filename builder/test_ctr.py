#!/usr/bin/env python3
"""Compare the builder's 3DSX and CIA writers with the reference tools' output for the same RomFS: 3dsxtool's and
makerom's (made in the build container by `make builder-test`, from random files: no game data). Everything must be
equal but the NCCH header's signature, which makerom makes and the builder keeps from its base CIA.

    builder/test_ctr.py BASE.3dsx REF.3dsx BASE.cia REF.cia ROMFS_DIR"""
import hashlib
import os
import struct
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ff9builder import ctr  # noqa: E402


def first_diff(a, b):
    for i in range(min(len(a), len(b))):
        if a[i] != b[i]:
            return i
    return None if len(a) == len(b) else min(len(a), len(b))


def main():
    base3, ref3, basec, refc, romfs = sys.argv[1:6]
    files = [(n, os.path.join(romfs, n)) for n in ('discs.bin', 'maps.bin')]
    bad = 0
    with tempfile.TemporaryDirectory() as tmp:
        out = os.path.join(tmp, 't.3dsx')
        ctr.write_3dsx(base3, files, out)
        a, b = open(out, 'rb').read(), open(ref3, 'rb').read()
        d = first_diff(a, b)
        print('3dsx:', 'identical' if d is None else f'differs at 0x{d:X} ({len(a)} vs {len(b)} bytes)')
        bad |= d is not None

        out = os.path.join(tmp, 't.cia')
        ctr.write_cia(basec, files, out)
        a, b = bytearray(open(out, 'rb').read()), bytearray(open(refc, 'rb').read())
        # the NCCH's header signature and the content id (makerom picks a random one) are the base's, as are the
        # ticket and the TMD's signature
        hsize, _, _, cc, tik, tmd = struct.unpack_from('<IHHIII', a, 0)
        o_tmd = ctr.align(ctr.align(ctr.align(hsize, 64) + cc, 64) + tik, 64)
        o_content = ctr.align(o_tmd + tmd, 64)
        rec = o_tmd + 0x140 + 0xC4 + 64 * 36
        o_tik = ctr.align(ctr.align(hsize, 64) + cc, 64)
        # makerom signs afresh and picks a random content id each run, so the hashes over those are checked in each
        # file against its own bytes, then left out of the comparison with them
        size = struct.unpack_from('<Q', a, 0x18)[0]
        info = o_tmd + 0x140 + 0xC4
        for name, buf in (('builder', a), ('reference', b)):
            if hashlib.sha256(buf[o_content:o_content + size]).digest() != bytes(buf[rec + 16:rec + 48]) or \
                    hashlib.sha256(buf[rec:rec + 48]).digest() != bytes(buf[info + 4:info + 36]) or \
                    hashlib.sha256(buf[info:info + 64 * 36]).digest() != bytes(buf[o_tmd + 0x140 + 0xA4:info]):
                print(f'cia: the {name}\'s TMD hashes don\'t match its contents')
                bad = 1
            for lo, hi in ((o_content, o_content + 0x100), (o_tik, o_tik + tik), (o_tmd, o_tmd + 0x140),
                           (o_tmd + 0x140 + 0xA4, info), (info + 4, info + 36), (rec, rec + 4), (rec + 16, rec + 48)):
                buf[lo:hi] = bytes(hi - lo)
        d = first_diff(a, b)
        print('cia:', 'identical but the NCCH signature, ticket and content id' if d is None else
              f'differs at 0x{d:X} ({len(a)} vs {len(b)} bytes)')
        bad |= d is not None
    sys.exit(bad)


if __name__ == '__main__':
    main()
