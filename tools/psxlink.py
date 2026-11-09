"""Which binary's function a name or a PS1 code address means, shared by tools/gen_link.py (the link, from the
decomp's committed configs and the ARM objects: no disc) and tools/gen_relocs.py (the relocation tables, from the
user's disc: the builder runs it). Both must resolve alike, so both use this."""
import re
from pathlib import Path

import ff9sym

XREFS = Path("config/xrefs.txt")
RAW_DATA = Path("config/raw_data.txt")
SLOTS = {0x800A7000: 0, 0x801EDF00: 1, 0x80198700: 2}  # overlay load address -> psx_ovl_resident index
TRAMP = "<trampoline>"


def load_xrefs(path=XREFS):
    """config/xrefs.txt: `<bin> <name> <target_bin>` for a named reference, `<bin>:<vram> <target_bin>` for a
    raw word in <bin>'s data."""
    named, raw = {}, {}
    if path.exists():
        for line in path.read_text().splitlines():
            f = line.split("#")[0].split()
            if len(f) == 3:
                named[(f[0], f[1])] = f[2]
            elif len(f) == 2 and ":" in f[0]:
                b, v = f[0].split(":")
                raw[(b, int(v, 16))] = f[1]
    return named, raw


def raw_data_names(path=RAW_DATA):
    """config/raw_data.txt: {binary: {data block name}} whose words are kept raw"""
    out = {}
    if path.exists():
        for line in path.read_text().splitlines():
            f = line.split("#")[0].split()
            if len(f) == 2:
                out.setdefault(f[0], set()).add(f[1])
    return out


class Resolver:
    """bins: {name: ff9sym.Binary}; funcs: {binary: {name: vram}}; defined: {binary: {global names its objects
    define}}. Trampolines it decides on collect in .tramps ({symbol: [(binary, target symbol)]})."""

    def __init__(self, bins, funcs, defined, xref_named, xref_raw):
        self.bins, self.funcs, self.defined = bins, funcs, defined
        self.xref_named, self.xref_raw = xref_named, xref_raw
        self.starts = {b: {a: n for n, a in fs.items() if a is not None} for b, fs in funcs.items()}
        self.tramps = {}
        self.report = []

    def qualified(self, target, name):
        return self.bins[target].ns + name

    def owner_of_name(self, src, name):
        """Which binary's `name` a reference from `src` means: its own, else main's, else a unique overlay's."""
        defined, funcs, bins = self.defined, self.funcs, self.bins
        if name in defined[src] or name in funcs.get(src, {}):
            return src
        if name in defined["main"] or name in funcs.get("main", {}):
            return "main"
        if (src, name) in self.xref_named:
            return self.xref_named[(src, name)]
        cands = [b for b in bins if b not in (src, "main") and (name in defined[b] or name in funcs.get(b, {}))]
        if len(cands) == 1:
            return cands[0]
        if cands:
            self.tramps[f"xt_{name}"] = [(c, self.qualified(c, name)) for c in cands]
            self.report.append(f"trampoline: {src} references {name}, defined in {', '.join(cands)}: dispatched on "
                               f"the resident overlay (config/xrefs.txt can pin it)")
            return TRAMP
        return None

    def owner_of_addr(self, src, vram):
        """Which binary's function a raw code address in `src`'s data means (None: not a function start)."""
        starts = self.starts
        if (src, vram) in self.xref_raw:
            return self.xref_raw[(src, vram)]
        if vram in starts.get(src, {}):
            return src
        if vram in starts.get("main", {}):
            return "main"
        cands = [b for b in self.bins if b not in (src, "main") and vram in starts.get(b, {})]
        if len(cands) == 1:
            return cands[0]
        if cands:
            self.tramps[f"xt_{vram:08X}"] = [(c, self.qualified(c, starts[c][vram])) for c in cands]
            self.report.append(f"trampoline: {src} data word 0x{vram:08X} is a function in {', '.join(cands)}: "
                               f"dispatched on the resident overlay (config/xrefs.txt `{src}:{vram:08X} <bin>` can "
                               f"pin it)")
            return TRAMP
        return None

    def possible_trampolines(self):
        """Every trampoline a data word could need (the disc decides which do; gen_relocs.py runs without the
        link): a name or a function start that more than one code overlay has. Effect modules have no slot to
        dispatch on: a word that needs one of theirs stops gen_relocs.py."""
        ovls = [b for b in self.bins if b != "main" and not b.startswith("fx_")]
        out = {}
        names = {}
        for b in ovls:
            for n in self.defined[b] | set(self.funcs.get(b, {})):
                names.setdefault(n, []).append(b)
        for n, cands in names.items():
            if len(cands) > 1:
                out[f"xt_{n}"] = [(c, self.qualified(c, n)) for c in cands]
        addrs = {}
        for b in ovls:
            for v in self.starts.get(b, {}):
                addrs.setdefault(v, []).append(b)
        for v, cands in addrs.items():
            if len(cands) > 1:
                out[f"xt_{v:08X}"] = [(c, self.qualified(c, self.starts[c][v])) for c in cands]
        return out


def native_index(bins, funcs, tramps):
    """Every native function a data word may point to, in a fixed order: the binaries' functions (qualified), then
    the trampolines. psxrel.bin stores code pointers as positions in it (gen_link.py emits it as psx_native_index)."""
    names = set()
    for b in bins:
        for n in funcs.get(b, {}):
            names.add(bins[b].ns + n)
    return sorted(names) + sorted(set(tramps) - names)


ADDEND = re.compile(r"^(\S+) \+ (-?\d+)$")
