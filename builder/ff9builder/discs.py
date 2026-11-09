"""The user's FF9 discs: finding the raw image behind an ISO, BIN/CUE or IMG/CCD dump, telling which disc it is, and
checking it is the NTSC-U release the port is built from (by the hashes of its files, not the image's: dumps of
the same disc differ in EDC/ECC, subchannels and pregap, the files never do)."""
import hashlib
import os
import re
import struct

RAW = 2352
USER = 2048
SYNC = b'\x00' + b'\xff' * 10 + b'\x00'

# SYSTEM.CNF's boot executable -> disc number (NTSC-U). The executable is the same file on all four discs.
DISC_OF_BOOT = {'SLUS_012.51': 1, 'SLUS_012.95': 2, 'SLUS_012.96': 3, 'SLUS_012.97': 4}
EXE_SHA1 = 'e37e8c4dc7c374a06375f312b366f79b8fbd1764'  # (the decomp's config/checksums.us.sha1)
IMG_SHA1 = {
    1: '38ec9ec7d7787985ae5befaac56ac4f88379a2e2',
    2: '1483c1cfb13e5fb7f53d7381648a486d08a35911',
    3: '4e4dab50e33f5a78bb1d02894e450e3facb56976',
    4: '0b98b41549b5539670d957ad3579f0b677ee87f9',
}
REGION_OF_PREFIX = {'SLES': 'the European (PAL) release', 'SLPS': 'the Japanese release',
                    'SCES': 'a European release', 'SCUS': 'a different NTSC-U title', 'SLPM': 'a Japanese release'}


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
        prefix = self.boot[:4].upper()
        if self.boot.upper() not in DISC_OF_BOOT:
            what = REGION_OF_PREFIX.get(prefix, 'another game')
            raise DiscError(f'{os.path.basename(self.path)} boots {self.boot}: {what}, not Final Fantasy IX (NTSC-U, '
                            f'SLUS-01251/01295/01296/01297), which this port is built from')
        self.number = DISC_OF_BOOT[self.boot.upper()]

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
        """Check the executable and FF9.IMG against the NTSC-U release's (DiscError if not)"""
        name = os.path.basename(self.path)
        ents = self.entries()
        if self.file_sha1(self.boot.upper().split(';')[0]) != EXE_SHA1:
            raise DiscError(f'{name}: the executable {self.boot} differs from the NTSC-U release\'s (a modified or '
                            f'damaged dump?)')
        if 'FF9.IMG' not in ents:
            raise DiscError(f'{name}: no FF9.IMG')
        if self.file_sha1('FF9.IMG', progress) != IMG_SHA1[self.number]:
            raise DiscError(f'{name}: disc {self.number}\'s FF9.IMG differs from the NTSC-U release\'s (a damaged dump '
                            f'or a patched/translated disc?)')

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
