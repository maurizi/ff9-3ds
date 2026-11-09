"""The builder's container writers, checked on their own output (CI runs this on each OS, with the payload in
payload/ or $FF9_PAYLOAD; builder/test_ctr.py compares them with makerom and 3dsxtool instead): random files in,
then the RomFS read back by name as libctru does, and every hash of the CIA (IVFC levels, NCCH superblock, TMD)
recomputed."""
import hashlib
import os
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ff9builder import ctr  # noqa: E402

PAYLOAD = os.environ.get('FF9_PAYLOAD', 'payload')


def romfs_read(f, at, name):
    """a file of a level 3 RomFS at `at`, found through the hash table as libctru finds it"""
    f.seek(at)
    h = struct.unpack('<10I', f.read(0x28))
    f.seek(at)
    meta = f.read(h[9])
    u = name.encode('utf-16le')
    buckets = h[6] // 4
    b = ctr._name_hash(0, u, buckets)
    off = struct.unpack_from('<I', meta, h[5] + 4 * b)[0]
    while off != ctr.EMPTY:
        parent, sib, data, size, nxt, nl = struct.unpack_from('<IIQQII', meta, h[7] + off)
        if meta[h[7] + off + 32:h[7] + off + 32 + nl] == u:
            f.seek(at + h[9] + data)
            return f.read(size)
        off = nxt
    return None


class Writers(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.files = []
        for name, n in (('discs.bin', 3 * 1024 * 1024 + 77), ('maps.bin', 10001), ('psxrel.bin', 4096)):
            p = os.path.join(cls.tmp.name, name)
            with open(p, 'wb') as f:
                f.write(os.urandom(n))
            cls.files.append((name, p))

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def need(self, name):
        p = os.path.join(PAYLOAD, name)
        if not os.path.isfile(p):
            self.skipTest(f'no {p}')
        return p

    def test_3dsx(self):
        out = os.path.join(self.tmp.name, 't.3dsx')
        ctr.write_3dsx(self.need('ff9.3dsx'), self.files, out)
        with open(out, 'rb') as f:
            hdr = f.read(0x2C)
            fs_at = struct.unpack_from('<I', hdr, 0x28)[0]
            self.assertEqual(fs_at, os.path.getsize(self.need('ff9.3dsx')))
            for name, p in self.files:
                self.assertEqual(romfs_read(f, fs_at, name), open(p, 'rb').read(), name)

    def test_cia(self):
        out = os.path.join(self.tmp.name, 't.cia')
        ctr.write_cia(self.need('ff9.cia'), self.files, out)
        c = open(out, 'rb').read()
        hsize, _, _, cc, tik, tmd_size, meta, size = struct.unpack_from('<IHHIIIIQ', c, 0)
        o_tmd = ctr.align(ctr.align(ctr.align(hsize, 64) + cc, 64) + tik, 64)
        o_ncch = ctr.align(o_tmd + tmd_size, 64)
        self.assertEqual(len(c), ctr.align(o_ncch + size, 64) + meta)
        tmd = c[o_tmd:o_tmd + tmd_size]
        body, rec = 0x140, 0x140 + 0xC4 + 64 * 36
        self.assertEqual(struct.unpack_from('>Q', tmd, rec + 8)[0], size)
        self.assertEqual(hashlib.sha256(c[o_ncch:o_ncch + size]).digest(), tmd[rec + 16:rec + 48])
        self.assertEqual(hashlib.sha256(tmd[rec:rec + 48]).digest(), tmd[body + 0xC4 + 4:body + 0xC4 + 36])
        self.assertEqual(hashlib.sha256(tmd[body + 0xC4:rec]).digest(), tmd[body + 0xA4:body + 0xC4])
        ncch = c[o_ncch:o_ncch + size]
        self.assertEqual(struct.unpack_from('<I', ncch, 0x104)[0] * 0x200, size)
        r_at, r_size, r_hash = (x * 0x200 for x in struct.unpack_from('<III', ncch, 0x1B0))
        romfs = ncch[r_at:r_at + r_size]
        self.assertEqual(romfs[:4], b'IVFC')
        self.assertEqual(hashlib.sha256(romfs[:r_hash]).digest(), ncch[0x1E0:0x200])
        # the hash tree: master -> level 1 -> level 2 -> level 3 (level 3 first in the file, at 0x1000)
        master_size = struct.unpack_from('<I', romfs, 8)[0]
        lv = [struct.unpack_from('<QQI', romfs, 0xC + 24 * i) for i in range(3)]
        l3_at = ctr.align(0x60 + master_size, 0x1000)
        l1_at = ctr.align(l3_at + lv[2][1], 0x1000)
        l2_at = ctr.align(l1_at + lv[0][1], 0x1000)
        data = {0: romfs[l1_at:l1_at + lv[0][1]], 1: romfs[l2_at:l2_at + lv[1][1]], 2: romfs[l3_at:l3_at + lv[2][1]]}
        self.assertEqual(ctr._block_hashes([data[0]]), romfs[0x60:0x60 + master_size])
        self.assertEqual(ctr._block_hashes([data[1]]), data[0])
        self.assertEqual(ctr._block_hashes([data[2]]), data[1])
        with tempfile.TemporaryFile() as f:
            f.write(data[2])
            for name, p in self.files:
                self.assertEqual(romfs_read(f, 0, name), open(p, 'rb').read(), name)


if __name__ == '__main__':
    unittest.main()
