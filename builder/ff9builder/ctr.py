"""3DS containers: RomFS (level 3, and the IVFC hash tree an NCCH wraps it in), the 3DSX's appended RomFS, and a CIA
whose NCCH gets a RomFS. Files are streamed: the disc pack is ~1.6 GB.

Layouts follow the reference tools, so their output can be compared byte for byte (builder/test_ctr.py): a 3DSX's
RomFS as 3dsxtool writes it, a CIA's as makerom does (file order, alignment, the IVFC levels). Signatures (the NCCH
header's, the ticket's and the TMD's) are kept from the prebuilt program-only CIA; as for any homebrew CIA, they
aren't Nintendo's anyway, and a console with custom firmware doesn't check them."""
import hashlib
import os
import struct

BLOCK = 0x1000
MEDIA = 0x200
EMPTY = 0xFFFFFFFF


def align(x, a):
    return (x + a - 1) // a * a


def _buckets(n):
    """hash table length for n entries (3dstools/makerom's rule)"""
    if n < 3:
        return 3
    if n < 19:
        return n | 1
    while any(n % p == 0 for p in (2, 3, 5, 7, 11, 13, 17)):
        n += 1
    return n


def _name_hash(parent, name, buckets):
    h = parent ^ 123456789
    for c in struct.unpack(f'<{len(name) // 2}H', name):
        h = ((h >> 5) | (h << 27)) & 0xFFFFFFFF
        h ^= c
    return h % buckets


class RomFS:
    """A RomFS level 3 holding files in its root directory: [(name, path)].
    makerom=True: files chained in order, their data 16-aligned (makerom); else the chain reversed and 4-aligned
    (3dsxtool). Either way each hash bucket's chain starts with its last file."""

    def __init__(self, files, makerom):
        self.files = [(name, path, os.path.getsize(path)) for name, path in files]
        names = [n.encode('utf-16le') for n, _, _ in self.files]
        offs, at = [], 0
        for n in names:
            offs.append(at)
            at += 0x20 + align(len(n), 4)
        ftable = bytearray(at)
        fb = _buckets(len(names))
        fhash = [EMPTY] * fb
        datas, d = [], 0
        for i, (n, (_, _, size)) in enumerate(zip(names, self.files)):
            d = align(d, 16 if makerom else 4)
            datas.append(d)
            d += size
        self.data_size = d
        # the root's file chain: makerom links the files in order, 3dsxtool puts each new one first
        chain = list(range(len(names))) if makerom else list(reversed(range(len(names))))
        siblings = {i: offs[chain[k + 1]] if k + 1 < len(chain) else EMPTY for k, i in enumerate(chain)}
        for i, n in enumerate(names):
            b = _name_hash(0, n, fb)
            struct.pack_into('<IIQQII', ftable, offs[i], 0, siblings[i], datas[i], self.files[i][2], fhash[b], len(n))
            ftable[offs[i] + 0x20:offs[i] + 0x20 + len(n)] = n
            fhash[b] = offs[i]
        root_child = offs[chain[0]] if chain else EMPTY
        dtable = struct.pack('<6I', 0, EMPTY, EMPTY, root_child, EMPTY, 0)
        dhash = [EMPTY] * 3
        dhash[_name_hash(0, b'', 3)] = 0
        dh = struct.pack('<3I', *dhash)
        fh = struct.pack(f'<{fb}I', *fhash)
        o_dh = 0x28
        o_dt = o_dh + len(dh)
        o_fh = o_dt + len(dtable)
        o_ft = o_fh + len(fh)
        o_data = o_ft + len(ftable)
        if makerom:
            o_data = align(o_data, 16)
        head = struct.pack('<10I', 0x28, o_dh, len(dh), o_dt, len(dtable), o_fh, len(fh), o_ft, len(ftable), o_data)
        self.meta = (head + dh + dtable + fh + bytes(ftable)).ljust(o_data, b'\0')
        self.datas = datas
        self.size = o_data + (self.data_size if makerom else align(self.data_size, 4))

    def chunks(self, step=1 << 22):
        """the level 3's bytes, in pieces"""
        yield self.meta
        at = 0
        for (_, path, size), d in zip(self.files, self.datas):
            if d > at:
                yield bytes(d - at)
            with open(path, 'rb') as f:
                left = size
                while left:
                    b = f.read(min(step, left))
                    if not b:
                        raise IOError(f'{path} ended early')
                    left -= len(b)
                    yield b
            at = d + size
        if self.size > len(self.meta) + at:
            yield bytes(self.size - len(self.meta) - at)


def _block_hashes(chunks, progress=None, total=None):
    """SHA-256 of each 4 KiB block of a byte stream (the last one zero-padded)"""
    out = bytearray()
    buf = bytearray()
    done = 0
    for c in chunks:
        buf += c
        whole = len(buf) // BLOCK * BLOCK
        mv = memoryview(buf)
        for k in range(0, whole, BLOCK):
            out += hashlib.sha256(mv[k:k + BLOCK]).digest()
        mv.release()
        del buf[:whole]
        done += len(c)
        if progress and total:
            progress(done, total)
    if buf:
        out += hashlib.sha256(bytes(buf).ljust(BLOCK, b'\0')).digest()
    return bytes(out)


class IVFC:
    """The NCCH RomFS: IVFC header and master hash, level 3, then levels 1 and 2 (makerom's layout)"""

    def __init__(self, level3, progress=None):
        self.l3 = level3
        self.l2 = _block_hashes(level3.chunks(), progress, level3.size)
        self.l1 = _block_hashes([self.l2])
        self.master = _block_hashes([self.l1])
        log1, log2 = 0, align(len(self.l1), BLOCK)
        log3 = align(log2 + len(self.l2), BLOCK)
        self.header = (b'IVFC' + struct.pack('<II', 0x10000, len(self.master)) +
                       struct.pack('<QQII', log1, len(self.l1), 12, 0) + struct.pack('<QQII', log2, len(self.l2), 12, 0) +
                       struct.pack('<QQII', log3, level3.size, 12, 0) + struct.pack('<II', 0x5C, 0))
        self.header = self.header.ljust(0x60, b'\0') + self.master
        self.l3_at = align(len(self.header), BLOCK)
        self.l1_at = align(self.l3_at + level3.size, BLOCK)
        self.l2_at = align(self.l1_at + len(self.l1), BLOCK)
        self.size = align(self.l2_at + len(self.l2), BLOCK)
        self.hash_region = align(len(self.header), MEDIA)
        self.superblock_hash = hashlib.sha256(self.header.ljust(self.hash_region, b'\0')).digest()

    def chunks(self):
        yield self.header.ljust(self.l3_at, b'\0')
        yield from self.l3.chunks()
        yield bytes(self.l1_at - self.l3_at - self.l3.size)
        yield self.l1
        yield bytes(self.l2_at - self.l1_at - len(self.l1))
        yield self.l2
        yield bytes(self.size - self.l2_at - len(self.l2))


def _copy(src, dst, n, step=1 << 22):
    while n:
        b = src.read(min(step, n))
        if not b:
            raise IOError('short read')
        dst.write(b)
        n -= len(b)


def write_3dsx(base, files, out, progress=None):
    """base: a 3DSX with an SMDH and no RomFS; files: [(name, path)] for its RomFS"""
    with open(base, 'rb') as f:
        prog = f.read()
    magic, hsize = struct.unpack_from('<4sH', prog, 0)
    if magic != b'3DSX' or hsize < 0x2C:
        raise ValueError(f'{base}: not a 3DSX with an extended header')
    smdh_at, smdh_size, fs_at = struct.unpack_from('<III', prog, 0x20)
    if fs_at:
        raise ValueError(f'{base} already has a RomFS')
    romfs = RomFS(files, makerom=False)
    prog = bytearray(prog)
    struct.pack_into('<I', prog, 0x28, len(prog))  # (right after the SMDH, as 3dsxtool puts it)
    done = 0
    with open(out, 'wb') as o:
        o.write(prog)
        for c in romfs.chunks():
            o.write(c)
            done += len(c)
            if progress:
                progress(done, romfs.size)
    return len(prog) + romfs.size


def write_cia(base, files, out, progress=None):
    """base: a CIA (one content, an unencrypted NCCH without a RomFS) made by makerom; files: [(name, path)] for the
    RomFS it gets. progress(done, total, phase) with phase 'hash' (reading the files once for the hash tree), then
    'write'."""
    with open(base, 'rb') as f:
        cia = f.read()
    hsize, _, _, cert_size, tik_size, tmd_size, meta_size, content_size = struct.unpack_from('<IHHIIIIQ', cia, 0)
    o_cert = align(hsize, 64)
    o_tik = align(o_cert + cert_size, 64)
    o_tmd = align(o_tik + tik_size, 64)
    o_content = align(o_tmd + tmd_size, 64)
    o_meta = align(o_content + content_size, 64)
    tmd = bytearray(cia[o_tmd:o_tmd + tmd_size])
    sig = struct.unpack_from('>I', tmd, 0)[0]
    body = {0x10003: 0x240, 0x10004: 0x140, 0x10005: 0x80}.get(sig)
    if body is None or struct.unpack_from('>H', tmd, body + 0x9E)[0] != 1:
        raise ValueError(f'{base}: not a single-content CIA')
    rec = body + 0xC4 + 64 * 36
    ctype = struct.unpack_from('>H', tmd, rec + 6)[0]
    if ctype & 1:
        raise ValueError(f'{base}: its content is encrypted')
    ncch = bytearray(cia[o_content:o_content + content_size])
    if ncch[0x100:0x104] != b'NCCH' or struct.unpack_from('<I', ncch, 0x1B0)[0]:
        raise ValueError(f'{base}: not an NCCH without a RomFS')
    if not ncch[0x18F] & 4:
        raise ValueError(f'{base}: its NCCH is encrypted')
    exefs_at, exefs_size = struct.unpack_from('<II', ncch, 0x1A0)
    ncch_end = (exefs_at + exefs_size) * MEDIA
    romfs_at = align(ncch_end, BLOCK)

    level3 = RomFS(files, makerom=True)
    ivfc = IVFC(level3, (lambda d, t: progress(d, t, 'hash')) if progress else None)
    struct.pack_into('<IIII', ncch, 0x1B0, romfs_at // MEDIA, ivfc.size // MEDIA, ivfc.hash_region // MEDIA, 0)
    ncch[0x1E0:0x200] = ivfc.superblock_hash
    new_size = romfs_at + ivfc.size
    struct.pack_into('<I', ncch, 0x104, new_size // MEDIA)
    head = bytes(ncch[:ncch_end]).ljust(romfs_at, b'\0')

    struct.pack_into('>Q', tmd, rec + 8, new_size)
    h = hashlib.sha256(head)
    with open(out, 'wb') as o:
        o.write(cia[:o_content])
        o.write(head)
        done = 0
        for c in ivfc.chunks():
            o.write(c)
            h.update(c)
            done += len(c)
            if progress:
                progress(done, ivfc.size, 'write')
        o.write(bytes(align(o_content + new_size, 64) - (o_content + new_size)))
        o.write(cia[o_meta:o_meta + meta_size])
        tmd[rec + 16:rec + 48] = h.digest()
        # the content info record covering it hashes the chunk records; the header hashes the info records
        tmd[body + 0xC4 + 4:body + 0xC4 + 36] = hashlib.sha256(tmd[rec:rec + 48]).digest()
        tmd[body + 0xA4:body + 0xC4] = hashlib.sha256(tmd[body + 0xC4:body + 0xC4 + 64 * 36]).digest()
        o.seek(o_tmd)
        o.write(tmd)
        o.seek(0x18)
        o.write(struct.pack('<Q', new_size))
    return o_meta - content_size + align(new_size, 64) + meta_size
