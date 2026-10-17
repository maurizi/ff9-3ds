#!/usr/bin/env python3
"""Compare the PS1 RAM of a DS run with the host's at the same frame, to find DS-only divergence.

The game's C keeps native pointers in PS1 RAM, so raw images always differ. Each word is normalised first: a
pointer into psx_ram becomes its RAM offset, and a pointer to a symbol of the build (code, port data) becomes
symbol+offset. What still differs is reported as ranges, named by the PS1 data symbol that holds them.

  tools/docker.sh build/pc/ff9ds --scenario S --data D --frames N+1 --ram-dump N:build/x/host.bin
  make ds AUTORUN=1 PROFILE=1 CAPTURE_FRAME=N SCENARIO=sd:/...; tools/ds_profile.py ... --frame N --out x.ml8
  python3 tools/ram_diff.py --host build/x/host.bin --host-elf build/pc/ff9ds --ds-state x.ml8 --ds-elf x.elf
"""
import argparse
import bisect
import struct
import subprocess

RAM = 0x200000


def symbols(elf):
    rows = []
    for line in subprocess.check_output(['nm', '-S', '-C', elf], text=True).splitlines():
        parts = line.split(None, 3)
        if len(parts) == 4:
            rows.append((int(parts[0], 16), int(parts[1], 16), parts[3]))
        elif len(parts) == 3 and parts[1] in 'AaTtDdBbRr':  # no size (the host's absolute psx_ram)
            rows.append((int(parts[0], 16), 0, parts[2]))
    rows.sort()
    return rows


class Names:
    def __init__(self, rows):
        self.rows = rows
        self.starts = [r[0] for r in rows]
        self.by_name = {r[2]: r[0] for r in rows}

    def find(self, addr):
        i = bisect.bisect_right(self.starts, addr) - 1
        for j in range(i, max(i - 16, -1), -1):  # an enclosing symbol may start a few entries back
            start, size, name = self.rows[j]
            if start <= addr < start + max(size, 1):
                return name, addr - start
        return None


def normalise(word, base, names):
    """Every reading of a word (a word may be both a code address and a link); words match if any agree."""
    out = {('raw', word), ('rebased', (word + base) & 0xFFFFFFFF)}  # an address minus psx_ram
    if base <= word < base + RAM:
        out.add(('ram', word - base))
    hit = names.find(word)
    if hit:
        out.add(('sym', hit[0], hit[1] & ~1))  # DS code pointers carry the Thumb bit
    # ordering-table and packet links: a length byte over a 24-bit native address (PSX_OT_PTR)
    low, base24 = word & 0xFFFFFF, base & 0xFFFFFF
    if base24 <= low < base24 + RAM and (base & 0xFF000000) == ((base + RAM - 1) & 0xFF000000):
        out.add(('link', word >> 24, low - base24))
    return out


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--host', required=True)
    p.add_argument('--host-elf', required=True)
    p.add_argument('--ds-state', required=True)
    p.add_argument('--ds-elf', required=True)
    p.add_argument('--limit', type=int, default=60)
    a = p.parse_args()
    host_rows, ds_rows = symbols(a.host_elf), symbols(a.ds_elf)
    host_names, ds_names = Names(host_rows), Names(ds_rows)
    host_base, ds_base = host_names.by_name['psx_ram'], ds_names.by_name['psx_ram']
    host = open(a.host, 'rb').read()
    state = open(a.ds_state, 'rb').read()
    ds = state[36 + ds_base - 0x02000000:36 + ds_base - 0x02000000 + RAM]
    # PS1 data symbols (linked into psx_ram) name the differing offsets
    ram_syms = sorted((s - host_base, size, n) for s, size, n in host_rows if host_base <= s < host_base + RAM)
    ram_starts = [r[0] for r in ram_syms]

    def owner(off):
        i = bisect.bisect_right(ram_starts, off) - 1
        if i >= 0:
            s, size, n = ram_syms[i]
            return f'{n}+{off - s:#x}' + ('' if off < s + max(size, 1) else ' (past end)')
        return '?'

    hw = struct.unpack(f'<{RAM // 4}I', host)
    dw = struct.unpack(f'<{RAM // 4}I', ds)
    ranges = []
    for i in range(RAM // 4):
        if hw[i] == dw[i]:
            continue
        if normalise(hw[i], host_base, host_names) & normalise(dw[i], ds_base, ds_names):
            continue
        if ranges and ranges[-1][1] >= i - 1:
            ranges[-1][1] = i
        else:
            ranges.append([i, i])
    total = sum(e - s + 1 for s, e in ranges)
    print(f'{total} differing words in {len(ranges)} ranges')
    for s, e in ranges[:a.limit]:
        off = s * 4
        print(f'  {off:06x}..{e * 4 + 3:06x} ({e - s + 1} words) {owner(off)}: host {hw[s]:08x} ds {dw[s]:08x}')


if __name__ == '__main__':
    main()
