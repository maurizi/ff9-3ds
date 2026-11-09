#!/usr/bin/env python3
"""The program's relocation tables, from the user's disc: psxrel.bin, read at boot (port/psx/psxrel.c).

    gen_relocs.py LINKINFO OUT            (run in the decomp tree: DECOMP=dir, default decomp/)

The program (tools/gen_link.py) is built without the disc and carries none of its bytes; this is the half of the
link that needs them. From the disc's executable and code overlays and the decomp's splat output of them (asm/:
splat's typed `.word`s are the places a pointer can be), it finds every pointer word in the binaries' data and
what each must become: data pointers -> psx_ram, function pointers -> the native function (by its position in the
program's native index, linkinfo.json). The builder (builder/) runs this on the user's discs; `make` runs it on
the developer's.

psxrel.bin (little-endian): "FF9R", version 1, the linkinfo id, section count; per section {id, offset, size};
section ids: 0x100 main, an overlay's FF9.IMG file id, 0x1000 + an effect module's psx_fxmods index.
  main:     executable size, FNV-1a of the chunks' bytes, chunk count, chunks {psx_ram offset, size, file offset},
            relocations
  overlay:  relocations
  module:   FNV-1a of its code bytes, relocations
relocations, by offset: (psx_ram offset | kind << 29), kind 0 a PS1 data address -> psx_ram, 1 scratchpad,
2 code (then its native index position), 3 code + addend (then position, addend); 0xFFFFFFFF ends.

Positions only: it holds none of the game's bytes, but it is derived from them, so it is made on the user's
machine and never committed or published."""
import hashlib
import json
import os
import re
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import ff9sym  # noqa: E402
import psxlink  # noqa: E402

ZERO_RUN = 0x1000  # zero runs at least this long are left out of main's chunks (psx_ram starts zeroed)
VERSION = 1


def linkinfo_id(text):
    return int.from_bytes(hashlib.sha256(text.encode()).digest()[:4], "little")


def raw_ranges(b, names):
    """[lo, hi) of the data blocks config/raw_data.txt names for binary b: from their dlabel .. enddlabel in the
    decomp's split data (the vram of each word in its comment)"""
    out = []
    for name in names.get(b, ()):
        lo = hi = None
        for path in sorted((ff9sym.DECOMP / "asm").glob(f"*/{b}/data/*.s")):
            inside = False
            for line in path.read_text().splitlines():
                if line.startswith(f"dlabel {name}"):
                    inside = True
                elif line.startswith(f"enddlabel {name}"):
                    break
                elif inside:
                    m = re.match(r"\s*/\* [0-9A-F]+ ([0-9A-F]{8}) ", line)
                    if m:
                        v = int(m.group(1), 16)
                        lo = v if lo is None else lo
                        hi = v + 4
            if lo is not None:
                break
        if lo is None:
            sys.exit(f"gen_relocs.py: config/raw_data.txt names {b} {name}, not in the decomp's data")
        out.append((lo, hi))
    return out


def regions_of(b, bi):
    """file offset ranges of a binary's data: .rodata, .data, and data splat keeps inside .text"""
    regions = [(bi.rodata, bi.text), (bi.data, bi.end)]
    for s in (ff9sym.DECOMP / f"asm/{ff9sym.VERSION}/{b}/data").glob("*.s"):
        t = s.read_text()
        m = re.search(r"\.section \.text.*?nonmatching (\w+), (0x[0-9A-Fa-f]+)", t, re.S)
        if m and ".incbin" in t:  # data embedded in .text (ovl_0b's textbin table)
            named = ff9sym.ADDR_NAME.match(m.group(1))
            if named:
                vram = int(named.group(1), 16)
            else:  # a name from the symbol files
                syms = {}
                for f in ff9sym.symbol_files(b):
                    syms.update(ff9sym.read_symbols(f))
                if m.group(1) not in syms:
                    sys.exit(f"gen_relocs.py: no address for {b}'s {m.group(1)} in .text")
                vram = syms[m.group(1)][0]
            regions.append((bi.off_of(vram), bi.off_of(vram) + int(m.group(2), 16)))
    return regions


def relocs_of(b, bi, bins, res, raw_names):
    """{vram: target} for binary b: "psx_ram + 0x..", "psx_scratchpad + 0x..", or a native symbol (+ addend)"""
    funcs, starts = res.funcs, res.starts
    regions = regions_of(b, bi)
    in_region = lambda v: any(bi.vram_of(lo) <= v < bi.vram_of(hi) for lo, hi in regions)
    relocs = {}
    raw = raw_ranges(b, raw_names)
    for vram, value, operand in ff9sym.data_words(b):
        if not in_region(vram) or vram % 4 or operand.startswith(".L") or any(lo <= vram < hi for lo, hi in raw):
            continue
        name = re.match(r"[A-Za-z_]\w*", operand)
        name = name.group(0) if name and not operand.startswith("0x") else None
        is_func = name is not None and any(name in funcs.get(x, {}) for x in bins)
        if not is_func and name is None and ff9sym.RAM_LO <= value < ff9sym.RAM_HI:
            own_lo, own_hi = bi.vram_of(0 if b != "main" else bi.header), bi.vram_of(bi.end)
            own_text = bi.text_range()
            if own_lo <= value < own_hi and not own_text[0] <= value < own_text[1] and b != "main":
                pass  # into the overlay's own rodata/data: a data pointer
            else:
                target = res.owner_of_addr(b, value)
                if target == psxlink.TRAMP:
                    relocs[vram] = f"xt_{value:08X}"
                    continue
                if target:
                    name, is_func = starts[target][value], True
                elif own_text[0] <= value < own_text[1] or (b != "main" and bins["main"].text_range()[0] <= value
                                                             < bins["main"].text_range()[1]):
                    res.report.append(f"mid-code: {b} data word at 0x{vram:08X} = 0x{value:08X} (kept raw)")
                    continue
        if is_func:
            target = res.owner_of_name(b, name)
            if target == psxlink.TRAMP:
                relocs[vram] = f"xt_{name}"
                continue
            if target is None:
                res.report.append(f"unresolved function pointer: {b} 0x{vram:08X} -> {name} (kept raw)")
                continue
            addend = value - funcs[target][name] if funcs[target].get(name) is not None else 0
            relocs[vram] = res.qualified(target, name) + (f" + {addend}" if addend else "")
        elif ff9sym.RAM_LO <= value < ff9sym.RAM_HI:
            relocs[vram] = f"psx_ram + 0x{value & 0x1FFFFF:X}"
        elif ff9sym.SPAD_LO <= value < ff9sym.SPAD_HI:
            relocs[vram] = f"psx_scratchpad + 0x{value & 0x3FF:X}"

    # Second pass: pointers splat typed as .short/.byte (inside struct-like data). Any aligned word not yet
    # classified whose value is a function start, or an address inside this binary's or main's data/rodata,
    # is relocated too, and listed in the report.
    typed = {vram for vram, _, _ in ff9sym.data_words(b)}

    def data_ranges(x):
        xb = bins[x]
        return [(xb.vram_of(xb.rodata if x != "main" else xb.header), xb.vram_of(xb.text)),
                (xb.vram_of(xb.data), xb.vram_of(xb.end))]
    known_data = data_ranges(b) + (data_ranges("main") if b != "main" else [])
    main_zero = 0x800A7000  # main's .data range continues into the zero region/overlay slots: not "data"
    for lo, hi in regions:
        for off in range(lo, hi - 3, 4):
            vram = bi.vram_of(off)
            if vram in relocs or vram in typed or any(lo <= vram < hi for lo, hi in raw):
                continue
            value = int.from_bytes(bi.bytes[off:off + 4], "little")
            if not ff9sym.RAM_LO <= value < ff9sym.RAM_HI:
                continue
            target = res.owner_of_addr(b, value) if (value in starts.get(b, {}) or value in starts.get("main", {})) else None
            if target and target != psxlink.TRAMP:
                relocs[vram] = res.qualified(target, starts[target][value])
                res.report.append(f"untyped pointer: {b} 0x{vram:08X} -> function {relocs[vram]}")
            elif any(rl <= value < rh for rl, rh in known_data) and not (
                    main_zero <= value < bins["main"].vram_of(bins["main"].end)
                    and not any(rl <= value < rh for rl, rh in data_ranges(b)) or (b == "main" and value >= main_zero)):
                relocs[vram] = f"psx_ram + 0x{value & 0x1FFFFF:X}"
                res.report.append(f"untyped pointer: {b} 0x{vram:08X} -> data 0x{value:08X}")
    return relocs, regions


def encode(relocs, index):
    out = []
    for v in sorted(relocs):
        target, off = relocs[v], v & 0x1FFFFF
        if target.startswith("psx_ram + "):
            out.append(off)
        elif target.startswith("psx_scratchpad + "):
            out.append(off | 1 << 29)
        else:
            m = psxlink.ADDEND.match(target)
            name, addend = (m.group(1), int(m.group(2))) if m else (target, 0)
            if name not in index:
                sys.exit(f"gen_relocs.py: {target} (a data word at 0x{v:08X}) isn't in the program's native index: "
                         f"the program must be rebuilt for this decomp")
            out += [off | 3 << 29, index[name], addend & 0xFFFFFFFF] if addend else [off | 2 << 29, index[name]]
    out.append(0xFFFFFFFF)
    return struct.pack(f"<{len(out)}I", *out)


def fnv(data, h=0x811C9DC5):
    for byte in data:
        h = ((h ^ byte) * 0x01000193) & 0xFFFFFFFF
    return h


def main_chunks(bi, relocs, regions):
    """main's byte ranges to copy (psx_ram offset, size, file offset), long zero runs without a relocation left
    out"""
    table = []
    for lo, hi in sorted(regions):
        off = lo
        while off < hi:
            run = off
            while run < hi and bi.bytes[run:run + 4] == b"\0\0\0\0" and bi.vram_of(run) not in relocs:
                run += 4
            if run - off >= ZERO_RUN or run >= hi:
                off = run
                continue
            start = off
            while off < hi:
                run = off
                while run < hi and bi.bytes[run:run + 4] == b"\0\0\0\0" and bi.vram_of(run) not in relocs:
                    run += 4
                if run - off >= ZERO_RUN:
                    break
                off += 4
            table.append((bi.vram_of(start) & 0x1FFFFF, off - start, start))
    return table


def build(linkinfo_path, out_path, report_path=None):
    text = Path(linkinfo_path).read_text()
    info = json.loads(text)
    bins = ff9sym.binaries(info["fxmods"])
    if sorted(bins) != sorted(info["bins"]):
        sys.exit("gen_relocs.py: the decomp's binaries aren't the program's (another decomp revision?)")
    funcs = {b: {n: a for n, a in fs.items()} for b, fs in info["funcs"].items()}
    defined = {b: set(v) for b, v in info["defined"].items()}
    xref_named = {tuple(k.split(" ")): v for k, v in info["xref_named"].items()}
    xref_raw = {(k.split(" ")[0], int(k.split(" ")[1], 16)): v for k, v in info["xref_raw"].items()}
    raw_names = {b: set(v) for b, v in info["raw_data"].items()}
    res = psxlink.Resolver(bins, funcs, defined, xref_named, xref_raw)
    index = {n: i for i, n in enumerate(info["index"])}

    sections = []
    for b in sorted(bins):
        bi = bins[b]
        relocs, regions = relocs_of(b, bi, bins, res, raw_names)
        if b == "main":
            chunks = main_chunks(bi, relocs, regions)
            h = 0x811C9DC5
            for _, n, at in chunks:
                h = fnv(bi.bytes[at:at + n], h)
            body = struct.pack("<III", bi.end, h, len(chunks))
            body += b"".join(struct.pack("<III", *c) for c in chunks)
            sections.append((0x100, body + encode(relocs, index)))
        elif b.startswith("fx_"):
            body = struct.pack("<I", fnv(bi.bytes[bi.text:bi.data]))
            sections.append((0x1000 + info["fxmods"].index(b), body + encode(relocs, index)))
        else:
            sections.append((int(b[4:], 16), encode(relocs, index)))
    head = b"FF9R" + struct.pack("<III", VERSION, linkinfo_id(text), len(sections))
    at = len(head) + 12 * len(sections)
    table, blob = b"", b""
    for sid, body in sections:
        table += struct.pack("<III", sid, at + len(blob), len(body))
        blob += body
        blob += bytes(-len(blob) % 4)
    Path(out_path).write_bytes(head + table + blob)
    if report_path:
        Path(report_path).write_text("".join(r + "\n" for r in sorted(set(res.report))))
    return len(head + table + blob)


if __name__ == "__main__":
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    if os.environ.get("DECOMP"):
        ff9sym.DECOMP = Path(os.environ["DECOMP"])
    n = build(sys.argv[1], sys.argv[2], sys.argv[3] if len(sys.argv) > 3 else None)
    print(f"gen_relocs: {sys.argv[2]}, {n} bytes")
