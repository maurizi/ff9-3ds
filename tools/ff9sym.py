"""Symbol and layout knowledge from the decomp checkout (decomp/), shared by the port's build tools.

Everything here is read from the decomp's committed configs, except what needs the user's disc: a binary's bytes
(Binary.bytes) and the splat output (asm/, generated from the disc by `make extract split`), which only
tools/gen_relocs.py uses. Nothing is hard-coded except the boot exe's section offsets, which are fixed facts of
SLUS_012.51 (its sha1 is checked by the decomp's `make extract`).
"""
import re
from pathlib import Path

DECOMP = Path("decomp")
VERSION = "us"

# SLUS_012.51: 0x800-byte header, then .rodata, .text, .data (decomp CLAUDE.md "Layout facts").
MAIN_NAME = "SLUS_012.51"
MAIN_VRAM = 0x80010000
MAIN_HEADER = 0x800
MAIN_TEXT = 0x2FA0
MAIN_DATA = 0x56DE0

RAM_LO, RAM_HI = 0x80010000, 0x80200000  # PS1 RAM the game uses (below 0x80010000 is the BIOS/kernel)
SPAD_LO, SPAD_HI = 0x1F800000, 0x1F800400

SYM_LINE = re.compile(r"^\s*(\w+)\s*=\s*(0x[0-9A-Fa-f]+)\s*;(.*)$")
ADDR_NAME = re.compile(r"^(?:func|D|jtbl)_([0-9A-F]{8})$")


def yaml_size(path):
    """A binary's size from its splat config (the last top-level segment's `- [0xSIZE]`)."""
    size = None
    for line in path.read_text().splitlines():
        m = re.match(r"^  - \[(0x[0-9A-Fa-f]+)\]", line)
        if m:
            size = int(m.group(1), 16)
    if size is None:
        raise ValueError(f"{path}: no end offset")
    return size


class Binary:
    """One of the decomp's binaries: its file, load address, section ranges (file offsets) and size (from its
    splat config: no disc needed). Its bytes are the disc's, read when first asked for."""

    def __init__(self, name, path, vram, rodata, text, data, header=0, yaml=None):
        self.name, self.path, self.vram = name, path, vram
        self.header = header
        self.rodata, self.text, self.data = rodata, text, data  # file offsets
        self.end = yaml_size(yaml)
        self._bytes = None
        # namespace prefix: ovl_0b -> o0b_, effect module fx_0b2 -> x0b2_
        self.ns = "" if name == "main" else ("x" + name[3:] + "_") if name.startswith("fx_") else "o" + name[4:] + "_"

    @property
    def bytes(self):
        if self._bytes is None:
            self._bytes = self.path.read_bytes()
            if len(self._bytes) != self.end:
                raise ValueError(f"{self.path}: {len(self._bytes)} bytes, its config says {self.end}")
        return self._bytes

    def vram_of(self, off):
        return self.vram + off - self.header

    def off_of(self, vram):
        return vram - self.vram + self.header

    def text_range(self):
        return self.vram_of(self.text), self.vram_of(self.data)


def native_fxmods(names=None):
    """Battle effect modules the port builds natively: those named in $FXNATIVE (the Makefile passes its list)
    whose C is complete (no INCLUDE_ASM). {name: (vram, text, data)} (file offsets)."""
    import os
    wanted = set(os.environ.get("FXNATIVE", "").split()) if names is None else set(names)
    out = {}
    for line in (DECOMP / f"config/fxmods.{VERSION}.txt").read_text().splitlines():
        f = line.split("#")[0].split()
        if len(f) < 5 or f[0] not in wanted:
            continue
        if names is None:
            srcs = list((DECOMP / "src/fxmod" / f[0]).glob("*.c"))
            if not srcs or any("INCLUDE_ASM" in p.read_text(errors="replace") for p in srcs):
                continue
        out[f[0]] = (int(f[1], 16), int(f[2], 16), int(f[3], 16))
    return out


def binaries(fxmods=None):
    """{name: Binary}: main, the code overlays and the native effect modules (fxmods: exactly these, else
    native_fxmods()'s choice)"""
    cfg = DECOMP / "config"
    out = {"main": Binary("main", DECOMP / "disks/us" / MAIN_NAME, MAIN_VRAM, MAIN_HEADER, MAIN_TEXT, MAIN_DATA, header=MAIN_HEADER,
                          yaml=cfg / "slus_012.51.yaml")}
    for line in (DECOMP / f"config/overlays.{VERSION}.txt").read_text().splitlines():
        f = line.split("#")[0].split()
        if len(f) >= 4:
            name = f"ovl_{f[0]}"
            out[name] = Binary(name, DECOMP / f"disks/us/ovl/{f[0]}.bin", int(f[1], 16), 0, int(f[2], 16), int(f[3], 16),
                               yaml=cfg / f"{name}.yaml")
    for name, (vram, text, data) in native_fxmods(fxmods).items():
        out[name] = Binary(name, DECOMP / f"disks/us/fxmod/{name}.bin", vram, 0, text, data,
                           yaml=cfg / f"fxmod/{name}.yaml")
    return out


def symbol_files(binary):
    """The decomp's symbol files that describe `binary`, hand-written first."""
    cfg = DECOMP / "config"
    if binary.startswith("fx_"):
        names = [f"fxmod/{binary}.syms.txt", f"fxmod/undefined_funcs_auto.{binary}.txt",
                 f"fxmod/undefined_syms_auto.{binary}.txt"]
    elif binary == "main":
        names = [f"symbols.{VERSION}.txt", f"symbols.psyq.{VERSION}.txt",
                 f"undefined_funcs_auto.{VERSION}.txt", f"undefined_syms_auto.{VERSION}.txt"]
    else:
        names = [f"symbols.{binary}.{VERSION}.txt", f"symbols.psyq.{binary}.{VERSION}.txt",
                 f"undefined_funcs_auto.{binary}.{VERSION}.txt", f"undefined_syms_auto.{binary}.{VERSION}.txt"]
    return [cfg / n for n in names if (cfg / n).exists()]


def read_symbols(path):
    """{name: (addr, is_func)} from a splat symbol file."""
    out = {}
    for line in path.read_text().splitlines():
        m = SYM_LINE.match(line)
        if m:
            out[m.group(1)] = (int(m.group(2), 16), "type:func" in m.group(3))
    return out


def functions():
    """{binary: {name: vram}} for every function, from the decomp's function index (names) plus addresses from
    the names themselves (func_XXXXXXXX) or the symbol files. The index's address column isn't used: it is off
    for overlays."""
    named = {}
    for b in ["main"] + [p.name[8:-7] for p in (DECOMP / "config").glob(f"symbols.ovl_*.{VERSION}.txt")]:
        for f in symbol_files(b):
            for n, (a, _) in read_symbols(f).items():
                named.setdefault(b, {}).setdefault(n, a)
    out = {}
    for line in (DECOMP / f"config/functions.{VERSION}.txt").read_text().splitlines():
        if line.startswith("#") or not line.strip():
            continue
        b, name = line.split()[:2]
        if name.startswith("g_"):
            continue  # a data word splat keeps in .text (ovl_0b's g_btlbgOtShift...): data (data_symbols)
        m = ADDR_NAME.match(name)
        addr = int(m.group(1), 16) if m else named.get(b, {}).get(name)
        if addr is None:
            addr = named.get("main", {}).get(name)  # PsyQ names live in main's files
        out.setdefault(b, {})[name] = addr
    # Effect modules aren't in the function index: their symbol file's functions and the func_XXXXXXXX their C defines
    for b in native_fxmods():
        fs = out.setdefault(b, {})
        for f in symbol_files(b):
            for n, (a, is_func) in read_symbols(f).items():
                if is_func:
                    fs.setdefault(n, a)
        for src in (DECOMP / "src/fxmod" / b).glob("*.c"):
            for m in re.finditer(r"^\w[\w \*]*\b(func_([0-9A-F]{8}))\s*\(", src.read_text(errors="replace"), re.M):
                fs.setdefault(m.group(1), int(m.group(2), 16))
    return out


def data_symbols_of(binary, splat=True):
    """{name: addr} of the non-function symbols one binary's symbol files (and splat data labels, with splat)
    define."""
    funcs = {n for fs in functions().values() for n in fs}
    out = {}
    for f in symbol_files(binary):
        for n, (a, is_func) in read_symbols(f).items():
            if (not is_func or n.startswith("g_")) and n not in funcs:
                out[n] = a
    asm = DECOMP / f"asm/{VERSION}/{binary}"
    for s in asm.rglob("*.s") if splat else ():
        for m in re.finditer(r"^dlabel (\w+)\n\s*/\* [0-9A-F]+ ([0-9A-F]{8}) ", s.read_text(errors="replace"), re.M):
            out.setdefault(m.group(1), int(m.group(2), 16))
    return out


def data_symbols(bins, splat=True):
    """{name: {addr, ...}} for every non-function symbol any binary knows: symbol files (and splat's data labels,
    with splat). More than one address for a name is a conflict the caller reports."""
    out = {}

    def add(name, addr):
        out.setdefault(name, set()).add(addr)

    funcs = {n for fs in functions().values() for n in fs}
    for b in bins:
        for f in symbol_files(b):
            for n, (a, is_func) in read_symbols(f).items():
                if (not is_func or n.startswith("g_")) and n not in funcs:
                    add(n, a)
        asm = DECOMP / f"asm/{VERSION}/{'main' if b == 'main' else b}"
        for s in asm.rglob("*.s") if splat else ():
            text = s.read_text(errors="replace")
            for m in re.finditer(r"^dlabel (\w+)\n\s*/\* [0-9A-F]+ ([0-9A-F]{8}) ", text, re.M):
                add(m.group(1), int(m.group(2), 16))
    return out


WORD_LINE = re.compile(r"/\* ([0-9A-F]+) ([0-9A-F]{8}) ([0-9A-F]{8}) \*/\s+\.word\s+(\S+)")


def data_words(binary):
    """(vram, value, operand) for every `.word` splat emitted in `binary`'s data/rodata: these are the only
    places a pointer can be (splat types everything else as .short/.byte/.asciz)."""
    asm = DECOMP / f"asm/{VERSION}/{binary}"
    seen = {}
    for s in sorted(asm.rglob("*.s")):
        for m in WORD_LINE.finditer(s.read_text(errors="replace")):
            vram = int(m.group(2), 16)
            raw = bytes.fromhex(m.group(3))
            seen[vram] = (vram, int.from_bytes(raw, "little"), m.group(4))
    return [seen[k] for k in sorted(seen)]


_ASM_INDEX = {}


def asm_path(binary, address, root=Path(".")):
    """The splat .s file of the function at `address` in `binary` (e.g. "ovl_0b"), whatever the decomp names it.

    Tests read original instructions by address, so they keep working when a function gets a name.
    """
    if binary not in _ASM_INDEX:
        index = {}
        for path in (root / DECOMP / "asm" / VERSION / binary / "nonmatchings").rglob("*.s"):
            first = re.search(r"/\* [0-9A-F]+ ([0-9A-F]{8}) [0-9A-F]{8} \*/", path.read_text())
            if first:
                index[int(first.group(1), 16)] = path
        _ASM_INDEX[binary] = index
    try:
        return _ASM_INDEX[binary][address]
    except KeyError:
        raise FileNotFoundError(f"no asm for {binary} 0x{address:08X} (matched in C, or `make extract` not run)")
