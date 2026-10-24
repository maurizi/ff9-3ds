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
    if name in ('port_pc_header', 'port_pc_main', 'port_pc_itcm'):
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


def attribute(base, n, table):
    words = struct.unpack('<%dH' % n, ram(symbols[table], 2 * n))
    for i, c in enumerate(words):
        if not c:
            continue
        mid = base + i * 64 + 32
        k = bisect.bisect_right(starts, mid) - 1
        name = '?%08X' % (base + i * 64)
        if k >= 0:
            s, size, fname = funcs[k]
            if mid < s + max(size, 64):
                name = fname
        counts[name] += c


attribute(main_lo, main_n, 'port_pc_main')
attribute(itcm_lo, itcm_n, 'port_pc_itcm')
print(f'{total} samples ({total / 8000:.1f} s), {outside} outside main RAM and ITCM (BIOS waits for VBlank and the like)')
for name, c in counts.most_common(a.top):
    print(f'{100 * c / total:6.2f}%  {c:7d}  {name}')
