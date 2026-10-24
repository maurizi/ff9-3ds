#!/usr/bin/env python3
"""Functions by share of samples from a PROFILE=3 build's sampling profiler (port/ds/pc_sampler.c).

    tools/ds_pc_profile.py STATE.ml8 --elf ff9ds.elf [--overlay 0a] [--top 40]

STATE is a melonDS state saved by tools/ds_profile.py. Code in the overlay slots is attributed to the resident
overlay's symbols (--overlay, slot 0's: 0a field, 0b battle, 0c world map). Samples are 64-byte buckets, so a
bucket that straddles two small functions goes to the one at its middle.
"""
import argparse
import bisect
import re
import collections
import struct
import subprocess
from pathlib import Path

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('state', type=Path)
p.add_argument('--elf', required=True, type=Path)
p.add_argument('--overlay', default='0a')
p.add_argument('--top', type=int, default=40)
a = p.parse_args()
data = a.state.read_bytes()
if data[:4] != b'MELN':
    p.error('expected a melonDS state')

symbols = {}
funcs = []
for line in subprocess.check_output(['objdump', '-t', str(a.elf)], text=True).splitlines():
    m = re.match(r'([0-9a-f]{8}) (.{7}) (\S+)\s+([0-9a-f]{8})\s+(?:\.hidden )?(\S+)$', line)
    if not m:
        continue
    addr, flags, section, size, name = int(m[1], 16), m[2], m[3], int(m[4], 16), m[5]
    if name in ('port_pc_header', 'port_pc_main', 'port_pc_itcm', 'port_pc_memset'):
        symbols[name] = addr
    if 'F' not in flags:
        continue
    if section.startswith('.psxovl_') and section != '.psxovl_' + a.overlay and section[len('.psxovl_'):] < '32':
        continue  # another overlay of the slot (slots 1 and 2, 32 up, sit at their own MIPS addresses)
    funcs.append((addr & ~1, size, name))
funcs.sort()
starts = [f[0] for f in funcs]
for name in ('port_pc_header', 'port_pc_main', 'port_pc_itcm'):
    if name not in symbols:
        p.error('the ELF has no %s: build with PROFILE=3' % name)


def ram(addr, size):
    off = 36 + addr - 0x02000000
    return data[off:off + size]


head = struct.unpack('<8I', ram(symbols['port_pc_header'], 32))
if head[0] != 0x53504331:
    p.error('no sampler header in the state')
_, main_lo, main_n, itcm_lo, itcm_n, total, outside, _ = head
counts = collections.Counter()


# a function without a size (assembly) runs to the next symbol
funcs = [(s, size or ((funcs[i + 1][0] - s) if i + 1 < len(funcs) else 64), name) for i, (s, size, name) in enumerate(funcs)]


def attribute(base, n, table):
    """each bucket's samples split over the functions it overlaps, by bytes"""
    words = struct.unpack('<%dH' % n, ram(symbols[table], 2 * n))
    for i, c in enumerate(words):
        if not c:
            continue
        lo, hi = base + i * 64, base + i * 64 + 64
        k = max(bisect.bisect_right(starts, lo) - 1, 0)
        parts = []
        while k < len(funcs) and funcs[k][0] < hi:
            s, size, fname = funcs[k]
            overlap = min(hi, s + size) - max(lo, s)
            if overlap > 0:
                parts.append((overlap, fname))
            k += 1
        if not parts:
            counts['?%08X' % lo] += c
            continue
        total_overlap = sum(o for o, _ in parts)
        for o, fname in parts:
            counts[fname] += c * o / total_overlap


attribute(main_lo, main_n, 'port_pc_main')
attribute(itcm_lo, itcm_n, 'port_pc_itcm')
print(f'{total} samples ({total / 8000:.1f} s), {outside} outside main RAM and ITCM (BIOS waits for VBlank and the like)')
for name, c in counts.most_common(a.top):
    print(f'{100 * c / total:6.2f}%  {c:9.0f}  {name}')

if 'port_pc_memset' in symbols:
    rows = struct.unpack('<768I', ram(symbols['port_pc_memset'], 3072))
    callers = [(rows[i * 3 + 2], rows[i * 3 + 1], rows[i * 3]) for i in range(256) if rows[i * 3]]
    if callers:
        print('memset callers (16 bytes or more): bytes, calls, caller')
    for nbytes, calls, caller in sorted(callers, key=lambda r: -r[1])[:16]:
        k = bisect.bisect_right(starts, caller & ~1) - 1
        print(f'{nbytes:12d} {calls:8d}  {funcs[k][2] if k >= 0 else "?"}+0x{(caller & ~1) - funcs[k][0]:x}')
