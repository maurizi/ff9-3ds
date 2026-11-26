"""The user's FF9 discs: finding the raw image behind an ISO, BIN/CUE or IMG/CCD dump, telling which release and disc
it is, and checking it is one the port is built for (by the hashes of its files, not the image's: dumps of the same
disc differ in EDC/ECC, subchannels and pregap, the files never do)."""
import hashlib
import os
import re
import struct

RAW = 2352
USER = 2048
SYNC = b'\x00' + b'\xff' * 10 + b'\x00'



class Release:
    """One release the port is built for: its program (the decomp's VERSION), each disc's boot executable (SYSTEM.CNF),
    the executable's SHA-1 (the same file on all four discs: the decomp's config/checksums.<version>.sha1) and each
    disc's FF9.IMG SHA-1s"""

    def __init__(self, version, name, boots, exe, imgs):
        self.version, self.name, self.exe, self.imgs = version, name, exe, imgs
        self.disc_of_boot = {b: n + 1 for n, b in enumerate(boots)}


RELEASES = [
    # v1.0 and v1.1 (Greatest Hits): v1.1's code is v1.0's byte for byte, and each FF9.IMG differs from v1.0's in one
    # 8 KB picture (directory 1, file 0x2718)
    Release('us', 'NTSC-U', ('SLUS_012.51', 'SLUS_012.95', 'SLUS_012.96', 'SLUS_012.97'),
            'e37e8c4dc7c374a06375f312b366f79b8fbd1764', {
                1: ('38ec9ec7d7787985ae5befaac56ac4f88379a2e2', '17b2d43c02260537eab1a8b6352aa709a946a0dc'),
                2: ('1483c1cfb13e5fb7f53d7381648a486d08a35911', 'eb35f16284b6cfdbe47581afe66e826566445eb8'),
                3: ('4e4dab50e33f5a78bb1d02894e450e3facb56976', '48528297f953836b643d10096d30e1407cc2000e'),
                4: ('0b98b41549b5539670d957ad3579f0b677ee87f9', 'd82ba9963b49cb87fcbe99be9ce2b10d89b8efb2')}),
    Release('es', 'PAL, Spanish', ('SLES_029.69', 'SLES_129.69', 'SLES_229.69', 'SLES_329.69'),
            '8d8e26e6b30a49d2b8d1f32b69becb86e94fc77b', {
                1: ('e0314a7151f9be82c3ee1ceb74dbebad5f288813',), 2: ('28b930dc18d54b6deff97b5a1fbb518bf8cefc7f',),
                3: ('a1575e25062880eeef84e1b7f7cf83f62a8dbd29',), 4: ('816e7fd38cf04ccc406928fa50ab02b00c0c2923',)}),
    Release('it', 'PAL, Italian', ('SLES_029.68', 'SLES_129.68', 'SLES_229.68', 'SLES_329.68'),
            '85c3a5393ef5d61366e14dd0ea6e17a82e10c41d', {
                1: ('7ac6f1c6074719f2167d9865d3e1e1ec71d99571',), 2: ('5a22926b78131656078569c9c14360cd37e7a65d',),
                3: ('a6b1cd292a93995b54572db27a3e74727e776ae2',), 4: ('8ad6456f0ffaabee51cde0bff55caec1d5256e55',)}),
    Release('de', 'PAL, German', ('SLES_029.67', 'SLES_129.67', 'SLES_229.67', 'SLES_329.67'),
            '04d3269b203cbaa8dcc0ac8697999c14c92b5bc5', {
                1: ('e6a19f7d6d72f81973e4b869c8b62e9df736064f',), 2: ('e1698bf54e4e5881cf4b1fe0c860bd899238f706',),
                3: ('06f431a586d1db796725ff5ccf381433ab91741f',), 4: ('c86687accc1acd4558d92c8a8dac32ae70f06913',)}),
    Release('fr', 'PAL, French', ('SLES_029.66', 'SLES_129.66', 'SLES_229.66', 'SLES_329.66'),
            '2082305d33d0aea451efab2eb2b8c870c7b60690', {
                1: ('3d989e6cab9b86874056a1896c3f1ff8f6a2d520',), 2: ('a72c82efc64407e927dad5313a6676ae73fe144f',),
                3: ('027e9468050d7bdc6e79ea70006eea560115b561',), 4: ('e42389f639fadbd12f73cfa8ab7522e8149979e4',)}),
    # (discs 1-3 not yet hashed from a reference dump: their FF9.IMG is not checked)
    Release('en', 'PAL, English', ('SLES_029.65', 'SLES_129.65', 'SLES_229.65', 'SLES_329.65'),
            'e6fdd67c75fc0fc1c3458ee2062cd30083671589', {4: ('f3feea35ad3130d401f75a9cbdb5d2c688f09984',)}),
]
RELEASE = {r.version: r for r in RELEASES}
# FF9 releases the port isn't built for yet, by each disc's boot executable
NOT_YET = {f'SLPS_020.0{d}': 'the Japanese release' for d in '0123'}
REGION_OF_PREFIX = {'SLES': 'a European (PAL) release', 'SLPS': 'a Japanese release',
                    'SCES': 'a European release', 'SCUS': 'a different NTSC-U title', 'SLPM': 'a Japanese release'}
SUPPORTED = ', '.join(r.name for r in RELEASES)


def release_of_boot(boot):
    """(Release, disc number) of a boot executable, else None"""
    for r in RELEASES:
        if boot.upper() in r.disc_of_boot:
            return r, r.disc_of_boot[boot.upper()]
    return None


class DiscError(Exception):
    pass


def _find_beside(base, name):
    """`name` in `base`'s folder, also when its case differs (a cue written on Windows, read on Linux)"""
    folder = os.path.dirname(os.path.abspath(base))
    p = os.path.join(folder, name)
    if os.path.isfile(p):
        return p
    low = os.path.basename(name).lower()
    for f in os.listdir(folder):
        if f.lower() == low:
            return os.path.join(folder, f)
    return None


def raw_image(path):
    """The raw-sector image file a dump's file names (a .cue's first track, a .ccd's .img, else the file itself)."""
    ext = os.path.splitext(path)[1].lower()
    if ext == '.cue':
        text = open(path, 'r', errors='replace').read()
        current = None
        for line in text.splitlines():
            m = re.match(r'\s*FILE\s+"?(.*?)"?\s+(\w+)\s*$', line, re.I)
            if m:
                current = m.group(1)
                continue
            m = re.match(r'\s*TRACK\s+(\d+)\s+(\S+)', line, re.I)
            if m and int(m.group(1)) == 1:
                mode = m.group(2).upper()
                if mode.endswith('/2048'):
                    raise DiscError(f'{os.path.basename(path)}: its data track is in 2048-byte sectors, which lose the '
                                    f'movies and their sound. Dump the disc with raw 2352-byte sectors (BIN/CUE).')
                if current is None:
                    break
                p = _find_beside(path, current)
                if not p:
                    raise DiscError(f'{os.path.basename(path)} names "{current}", which isn\'t beside it')
                return p
        raise DiscError(f'{os.path.basename(path)}: no track 1 with a file')
    if ext == '.ccd':
        p = _find_beside(path, os.path.splitext(os.path.basename(path))[0] + '.img')
        if not p:
            raise DiscError(f'{os.path.basename(path)}: no .img of the same name beside it')
        return p
    return path


class Disc:
    """A raw (2352 bytes a sector) Mode 2 image of one FF9 disc"""

    def __init__(self, path):
        self.source = path
        self.path = raw_image(path)
        if not os.path.isfile(self.path):
            raise DiscError(f'{self.path}: no such file')
        self.f = open(self.path, 'rb')
        size = os.path.getsize(self.path)
        self.f.seek(16 * RAW)
        s = self.f.read(RAW)
        if s[:12] != SYNC or s[24:30] != b'\x01CD001':
            self.f.seek(16 * USER)
            if self.f.read(6) == b'\x01CD001':
                raise DiscError(f'{os.path.basename(self.path)} is an ISO of 2048-byte sectors: that loses the movies '
                                f'and their sound (FF9 streams them from sectors an ISO leaves out). Dump the disc with '
                                f'raw 2352-byte sectors (BIN/CUE or IMG/CCD).')
            raise DiscError(f'{os.path.basename(self.path)}: not a PlayStation disc image (no ISO9660 volume in raw '
                            f'2352-byte sectors)')
        self.sectors = size // RAW
        self.boot = self._boot()
        found = release_of_boot(self.boot)
        if not found:
            b = self.boot.upper()
            if b in NOT_YET:
                raise DiscError(f'{os.path.basename(self.path)} boots {self.boot}: Final Fantasy IX, {NOT_YET[b]}, '
                                f'which this port is not built for yet (it is for {SUPPORTED})')
            what = REGION_OF_PREFIX.get(b[:4], 'another game')
            raise DiscError(f'{os.path.basename(self.path)} boots {self.boot}: {what}, not a release of Final Fantasy IX '
                            f'this port is built for ({SUPPORTED})')
        self.release, self.number = found

    def close(self):
        self.f.close()

    def user(self, lba, size):
        out = bytearray()
        for i in range((size + USER - 1) // USER):
            self.f.seek((lba + i) * RAW + 24)
            out += self.f.read(USER)
        return bytes(out[:size])

    def entries(self, lba=None, size=None):
        """{NAME: (lba, size, is_dir)} of a directory (the root's by default)"""
        if lba is None:
            pvd = self.user(16, USER)
            lba, size = struct.unpack_from('<I', pvd, 158)[0], struct.unpack_from('<I', pvd, 166)[0]
        d = self.user(lba, size)
        out = {}
        i = 0
        while i < len(d):
            n = d[i]
            if not n:
                i = (i // USER + 1) * USER
                continue
            e = d[i:i + n]
            i += n
            name = e[33:33 + e[32]].decode('latin-1')
            if name not in ('\0', '\1'):
                out[name.split(';')[0].upper()] = (struct.unpack_from('<I', e, 2)[0], struct.unpack_from('<I', e, 10)[0],
                                                   bool(e[25] & 2))
        return out

    def _boot(self):
        ents = self.entries()
        if 'SYSTEM.CNF' not in ents:
            raise DiscError(f'{os.path.basename(self.path)}: no SYSTEM.CNF (not a PlayStation game disc)')
        cnf = self.user(*ents['SYSTEM.CNF'][:2]).decode('latin-1')
        m = re.search(r'cdrom:\\?([^;\s]+)', cnf, re.I)
        if not m:
            raise DiscError(f'{os.path.basename(self.path)}: SYSTEM.CNF has no boot file')
        return m.group(1)

    def file_sha1(self, name, progress=None):
        lba, size, _ = self.entries()[name]
        h = hashlib.sha1()
        done = 0
        step = 256  # sectors a read
        self.f.seek(lba * RAW)
        while done < size:
            block = self.f.read(RAW * step)
            if not block:
                raise DiscError(f'{os.path.basename(self.path)}: ends inside {name} (an incomplete copy?)')
            for k in range(0, len(block), RAW):
                n = min(USER, size - done)
                if n <= 0:
                    break
                h.update(block[k + 24:k + 24 + n])
                done += n
            if progress:
                progress(done, size)
        return h.hexdigest()

    def verify(self, progress=None):
        """Check the executable and FF9.IMG against the release's (DiscError if not); False when this disc's FF9.IMG
        has no reference hash yet (not checked), else True"""
        name = os.path.basename(self.path)
        ents = self.entries()
        r = self.release
        if self.file_sha1(self.boot.upper().split(';')[0]) != r.exe:
            raise DiscError(f'{name}: the executable {self.boot} differs from the {r.name} release\'s (a modified or '
                            f'damaged dump?)')
        if 'FF9.IMG' not in ents:
            raise DiscError(f'{name}: no FF9.IMG')
        if self.number not in r.imgs:
            return False
        if self.file_sha1('FF9.IMG', progress) not in r.imgs[self.number]:
            raise DiscError(f'{name}: disc {self.number}\'s FF9.IMG differs from the {r.name} release\'s (a damaged '
                            f'dump or a patched/translated disc?)')
        return True

    def raw_sectors(self, lba, count):
        """whole 2352-byte sectors (a movie's: their subheaders and Form 2 data)"""
        self.f.seek(lba * RAW)
        return [self.f.read(RAW) for _ in range(count)]

    def movie_lba(self, folder, name):
        """the first sector of a file in a directory of the root (a movie: OPENING/FMVD001.STR)"""
        lba, size, _ = self.entries()[folder]
        return self.entries(lba, size)[name][0]

    def file_view(self, name):
        """A read-only file object over one of the disc's files (its user data, sector by sector)"""
        lba, size, _ = self.entries()[name]
        return _FileView(self.path, lba, size)


class _FileView:
    def __init__(self, path, lba, size):
        self.f = open(path, 'rb')
        self.lba, self.size, self.pos = lba, size, 0

    def seek(self, off, whence=0):
        self.pos = off if whence == 0 else self.pos + off if whence == 1 else self.size + off
        return self.pos

    def tell(self):
        return self.pos

    def read(self, n=-1):
        if n < 0:
            n = self.size - self.pos
        n = max(0, min(n, self.size - self.pos))
        out = bytearray()
        while n > 0:
            sec, within = divmod(self.pos, USER)
            take = min(n, USER - within)
            self.f.seek((self.lba + sec) * RAW + 24 + within)
            out += self.f.read(take)
            self.pos += take
            n -= take
        return bytes(out)

    def close(self):
        self.f.close()
