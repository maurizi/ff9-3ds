#!/usr/bin/env python3
"""Generate everything that links the decomp's 20 binaries into one ARM image. Outputs go to build/gen/.

Usage: gen_link.py OBJ_DIR      (run in the build container: it calls arm-none-eabi-nm on OBJ_DIR/src*/<bin>/*.o)

Model (docs/design.md §3.3-3.5): PS1 data lives in psx_ram[], a 2 MB image of PS1 RAM. Code is native.
- img_<bin>.s    the binary's initialised data (.rodata, .data, data inside .text) as chunks of the user's own
                 bytes, with every pointer word relocated: data pointers to psx_ram + offset, function pointers
                 to the native function. The runtime copies main's chunks into psx_ram at boot and an overlay's
                 when it is "loaded". Zero runs aren't stored (psx_ram starts zeroed).
- psx_syms.ld    PROVIDE(name = psx_ram + offset) for every data symbol the decomp knows, so C that names a PS1
                 global gets the same memory as code that uses its literal address.
- redefine_<bin>.txt  objcopy --redefine-syms maps. Overlay functions get the overlay's prefix (ovl_0b ->
                 o0b_func_...), since six overlays share 0x800A7000 and their names collide; references from one
                 binary to another binary's function are pointed at the right prefixed name.
- report.txt     everything that needs a decision: ambiguous cross-binary references (config/xrefs.txt resolves
                 them, decomp-requests.md R3), conflicting data names, pointers into the middle of code.
"""
import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import ff9sym  # noqa: E402

GEN = Path("build/gen")
XREFS = Path("config/xrefs.txt")
ZERO_RUN = 0x1000
SLOTS = {0x800A7000: 0, 0x801EDF00: 1, 0x80198700: 2}  # overlay load address -> psx_ovl_resident index  # zero runs at least this long are left out of the images


def nm(paths, *flags):
    if not paths:
        return []
    out = subprocess.run(["arm-none-eabi-nm", *flags, *map(str, paths)], capture_output=True, text=True, check=True)
    return out.stdout.splitlines()


def defined_globals(paths):
    names = set()
    for line in nm(paths, "-g", "--defined-only"):
        f = line.split()
        if len(f) == 3:
            names.add(f[2])
    return names


def undefined(paths):
    return {line.split()[-1] for line in nm(paths, "-u") if line.strip() and not line.endswith(":")}


def load_xrefs():
    """config/xrefs.txt: `<bin> <name> <target_bin>` for a named reference, `<bin>:<vram> <target_bin>` for a
    raw word in <bin>'s data."""
    named, raw = {}, {}
    if XREFS.exists():
        for line in XREFS.read_text().splitlines():
            f = line.split("#")[0].split()
            if len(f) == 3:
                named[(f[0], f[1])] = f[2]
            elif len(f) == 2 and ":" in f[0]:
                b, v = f[0].split(":")
                raw[(b, int(v, 16))] = f[1]
    return named, raw


def main():
    obj_dir = Path(sys.argv[1])
    GEN.mkdir(parents=True, exist_ok=True)
    bins = ff9sym.binaries()
    funcs = ff9sym.functions()
    starts = {b: {a: n for n, a in fs.items() if a is not None} for b, fs in funcs.items()}
    xref_named, xref_raw = load_xrefs()
    report = []

    objs = {b: sorted(obj_dir.glob(f"src/{b}/*.o")) + sorted(obj_dir.glob(f"src_equiv/{b}/*.o"))
            + sorted(p for p in obj_dir.glob(f"docs/wip/{b}_*.o") if re.fullmatch(rf"{b}_[0-9A-F]{{8}}", p.stem))
            for b in bins}
    defined = {b: defined_globals(objs[b]) for b in bins}
    defined_in_src = set().union(*(defined_globals(sorted(obj_dir.glob(f"src/{b}/*.o"))) for b in bins))
    defined_in_equiv = set().union(*(defined_globals(sorted(obj_dir.glob(f"src_equiv/{b}/*.o"))) for b in bins))

    def owner_of_name(src, name):
        """Which binary's `name` a reference from `src` means: its own, else main's, else a unique overlay's."""
        if name in defined[src] or name in funcs.get(src, {}):
            return src
        if name in defined["main"] or name in funcs.get("main", {}):
            return "main"
        if (src, name) in xref_named:
            return xref_named[(src, name)]
        cands = [b for b in bins if b not in (src, "main") and (name in defined[b] or name in funcs.get(b, {}))]
        if len(cands) == 1:
            return cands[0]
        if cands:
            tramps[f"xt_{name}"] = [(c, qualified(c, name)) for c in cands]
            report.append(f"trampoline: {src} references {name}, defined in {', '.join(cands)}: dispatched on the "
                          f"resident overlay (config/xrefs.txt can pin it)")
            return TRAMP
        return None

    def owner_of_addr(src, vram):
        """Which binary's function a raw code address in `src`'s data means (None: not a function start)."""
        if (src, vram) in xref_raw:
            return xref_raw[(src, vram)]
        if vram in starts.get(src, {}):
            return src
        if vram in starts.get("main", {}):
            return "main"
        cands = [b for b in bins if b not in (src, "main") and vram in starts.get(b, {})]
        if len(cands) == 1:
            return cands[0]
        if cands:
            tramps[f"xt_{vram:08X}"] = [(c, qualified(c, starts[c][vram])) for c in cands]
            report.append(f"trampoline: {src} data word 0x{vram:08X} is a function in {', '.join(cands)}: dispatched "
                          f"on the resident overlay (config/xrefs.txt `{src}:{vram:08X} <bin>` can pin it)")
            return TRAMP
        return None

    def qualified(target, name):
        return bins[target].ns + name

    TRAMP = "<trampoline>"
    tramps = {}  # trampoline symbol -> [(binary, target symbol)]

    def d_is_function(src, vram):
        """A D_XXXXXXXX that `src`'s C uses is another binary's function: it starts one there, outside `src`'s own
        image (an overlay's data at an address where another overlay has code is the overlay's data)."""
        bi = bins[src]
        if src != "main" and bi.vram_of(0) <= vram < bi.vram_of(bi.end):
            return False
        return any(vram in starts.get(x, {}) for x in bins if x != src)

    aliases = {}  # placeholder -> {target symbol}: link-time aliases (below)
    # The decomp's own aliases for main's calls into overlays (config/xrefs.us.txt: `name = 0xADDR; /* ovl_xx ... */`),
    # which its PS1 link script defines: names no binary defines, resolved to the function at that address
    decomp_xrefs = {}
    xref_file = ff9sym.DECOMP / f"config/xrefs.{ff9sym.VERSION}.txt"
    if xref_file.exists():
        for m in re.finditer(r"^(\w+)\s*=\s*0x([0-9A-Fa-f]+);\s*(?:/\*\s*(ovl_\w+))?", xref_file.read_text(), re.M):
            decomp_xrefs[m.group(1)] = (int(m.group(2), 16), m.group(3))
    # Data names two binaries define at different addresses (the opening's and the ending's g_movieState): each
    # binary's references are qualified like its functions, and each gets its own PROVIDE (psx_syms.ld).
    data_of = {b: ff9sym.data_symbols_of(b) for b in bins}
    data_owners = {}
    for b, names in data_of.items():
        for n, a in names.items():
            data_owners.setdefault(n, {})[b] = a
    shared_data = {n: owners for n, owners in data_owners.items() if len(set(owners.values())) > 1}

    # --- redefine maps --------------------------------------------------------------------------------------
    for b in bins:
        mapping = {}
        if b != "main":
            for name in defined[b] | set(funcs.get(b, {})):
                mapping[name] = qualified(b, name)
        for name in undefined(objs[b]):
            if name in shared_data and b in shared_data[name] and b != "main":
                mapping[name] = qualified(b, name)
                continue
            if name in mapping or name.startswith("__"):
                continue
            if name in defined[b] or name in funcs.get(b, {}):
                continue
            target = owner_of_name(b, name) if any(name in funcs.get(x, {}) or name in defined[x] for x in bins) else None
            if target is None and name in decomp_xrefs:
                vram, ovl = decomp_xrefs[name]
                owner = ovl if ovl in bins and vram in starts.get(ovl, {}) else owner_of_addr(b, vram)
                if owner and owner != TRAMP and owner != b and vram in starts.get(owner, {}):
                    mapping[name] = qualified(owner, starts[owner][vram])
                    report.append(f"decomp alias: {b} {name} -> {owner} {starts[owner][vram]}")
                    continue
            if target is None and (m := ff9sym.ADDR_NAME.match(name)) and \
                    (name.startswith("func_") or (name.startswith("D_") and d_is_function(b, int(m.group(1), 16)))):
                # a placeholder name for a function another binary has since named, or a function the C calls
                # through a D_ name (main's D_800BED6C, ovl_0b's btl_findUnitByBit): resolve it by its address
                vram = int(m.group(1), 16)
                target = owner_of_addr(b, vram)
                if target == TRAMP:
                    mapping[name] = f"xt_{vram:08X}"
                elif target and target != b:
                    mapping[name] = qualified(target, starts[target][vram])
                    report.append(f"by address: {b} {name} -> {target} {starts[target][vram]}")
                continue
            if target == TRAMP:
                mapping[name] = f"xt_{name}"
            elif target and target not in ("main", b):
                mapping[name] = qualified(target, name)
        # a placeholder resolved by address whose target the binary also references by name: objcopy can't rename
        # two symbols to one, so the placeholder becomes a link-time alias (psx_syms.ld) instead
        targets = {}
        for k, v in mapping.items():
            targets.setdefault(v, []).append(k)
        for v, ks in targets.items():
            if len(ks) > 1:
                for k in ks:
                    if ff9sym.ADDR_NAME.match(k):
                        aliases.setdefault(k, set()).add(v)
                        del mapping[k]
        (GEN / f"redefine_{b}.txt").write_text("".join(f"{k} {v}\n" for k, v in sorted(mapping.items())))

    # --- data images ----------------------------------------------------------------------------------------
    for b, bi in bins.items():
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
                        sys.exit(f"gen_link.py: no address for {b}'s {m.group(1)} in .text")
                    vram = syms[m.group(1)][0]
                regions.append((bi.off_of(vram), bi.off_of(vram) + int(m.group(2), 16)))
        in_region = lambda v: any(bi.vram_of(lo) <= v < bi.vram_of(hi) for lo, hi in regions)

        relocs = {}
        for vram, value, operand in ff9sym.data_words(b):
            if not in_region(vram) or vram % 4 or operand.startswith(".L"):
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
                    target = owner_of_addr(b, value)
                    if target == TRAMP:
                        relocs[vram] = f"xt_{value:08X}"
                        continue
                    if target:
                        name, is_func = starts[target][value], True
                    elif own_text[0] <= value < own_text[1] or (b != "main" and bins["main"].text_range()[0] <= value
                                                                 < bins["main"].text_range()[1]):
                        report.append(f"mid-code: {b} data word at 0x{vram:08X} = 0x{value:08X} (kept raw)")
                        continue
            if is_func:
                target = owner_of_name(b, name)
                if target == TRAMP:
                    relocs[vram] = f"xt_{name}"
                    continue
                if target is None:
                    report.append(f"unresolved function pointer: {b} 0x{vram:08X} -> {name} (kept raw)")
                    continue
                addend = value - funcs[target][name] if funcs[target].get(name) is not None else 0
                relocs[vram] = qualified(target, name) + (f" + {addend}" if addend else "")
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
                if vram in relocs or vram in typed:
                    continue
                value = int.from_bytes(bi.bytes[off:off + 4], "little")
                if not ff9sym.RAM_LO <= value < ff9sym.RAM_HI:
                    continue
                target = owner_of_addr(b, value) if (value in starts.get(b, {}) or value in starts.get("main", {})) else None
                if target and target != TRAMP:
                    relocs[vram] = qualified(target, starts[target][value])
                    report.append(f"untyped pointer: {b} 0x{vram:08X} -> function {relocs[vram]}")
                elif any(rl <= value < rh for rl, rh in known_data) and not (
                        main_zero <= value < bins["main"].vram_of(bins["main"].end)
                        and not any(rl <= value < rh for rl, rh in data_ranges(b)) or (b == "main" and value >= main_zero)):
                    relocs[vram] = f"psx_ram + 0x{value & 0x1FFFFF:X}"
                    report.append(f"untyped pointer: {b} 0x{vram:08X} -> data 0x{value:08X}")

        if b != "main":
            # An overlay's bytes reach psx_ram from the disc (the game's own CD loads); only its pointer words
            # change. Entries, sorted by offset: (psx_ram offset | kind << 29), and for code pointers the native
            # address after it. Kind 0: a PS1 data address in that word -> psx_ram; 1: scratchpad; 2: code.
            out = [f"/* Generated by tools/gen_link.py from {bi.path}: derived from game data, never commit. */",
                   f'    .section .rodata.psxrel.{b},"a"', "    .balign 4", f"    .global psxrel_{b}", f"psxrel_{b}:"]
            for v in sorted(relocs):
                target = relocs[v]
                off = v & 0x1FFFFF
                if target.startswith("psx_ram + "):
                    out.append(f"    .4byte 0x{off:X}")
                elif target.startswith("psx_scratchpad + "):
                    out.append(f"    .4byte 0x{off | 1 << 29:X}")
                else:
                    out.append(f"    .4byte 0x{off | 2 << 29:X}, {target}")
            out += ["    .4byte 0xFFFFFFFF", ""]
            (GEN / f"img_{b}.s").write_text("\n".join(out))
            continue
        # main's image is read once, at boot: on the DS it is overlay 0xFF of slot 0 (psx_ovl_areas.ld), loaded from
        # ovls.bin into that place and copied to psx_ram before any overlay is (port/psx/mem.c psx_run_game)
        out = [f"/* Generated by tools/gen_link.py from {bi.path}: game data, never commit. */",
               f'    .section {".psxovl_ff.image" if b == "main" else ".rodata.psximg." + b},"a"', "    .balign 4"]
        table = []
        for lo, hi in sorted(regions):
            off = lo
            while off < hi:
                # skip a long zero run that carries no relocation
                run = off
                while run < hi and bi.bytes[run:run + 4] == b"\0\0\0\0" and bi.vram_of(run) not in relocs:
                    run += 4
                if run - off >= ZERO_RUN or run >= hi:
                    off = run
                    continue
                label = f"psximg_{b}_{len(table)}"
                start = off
                out.append(f"{label}:")
                words = []
                while off < hi:
                    run = off
                    while run < hi and bi.bytes[run:run + 4] == b"\0\0\0\0" and bi.vram_of(run) not in relocs:
                        run += 4
                    if run - off >= ZERO_RUN:
                        break
                    v = bi.vram_of(off)
                    words.append(relocs.get(v) or f"0x{int.from_bytes(bi.bytes[off:off + 4], 'little'):08X}")
                    off += 4
                for i in range(0, len(words), 8):
                    out.append("    .4byte " + ", ".join(words[i:i + 8]))
                table.append((bi.vram_of(start) & 0x1FFFFF, off - start, label))
        out += ["", f'    .section .rodata.psximg.{b},"a"', "    .balign 4", f"    .global psximg_{b}", f"psximg_{b}:"]
        out += [f"    .4byte 0x{o:X}, 0x{n:X}, {lbl}" for o, n, lbl in table]
        out += ["    .4byte 0, 0, 0", ""]
        (GEN / f"img_{b}.s").write_text("\n".join(out))

    # --- data symbol addresses -------------------------------------------------------------------------------
    syms = ff9sym.data_symbols(bins)
    lines = ["/* Generated by tools/gen_link.py: PS1 data symbols -> psx_ram. */"]
    for name in sorted(syms):
        if name.startswith("func_"):
            continue  # a code label splat emitted as data (BIOS call stubs): never place code in psx_ram
        addrs = syms[name]
        if len(addrs) > 1:
            if name in shared_data:
                for ob, a in sorted(shared_data[name].items()):
                    lines.append(f"PROVIDE({bins[ob].ns + name} = psx_ram + 0x{a & 0x1FFFFF:X});")
                continue
            report.append(f"conflict: data symbol {name} has addresses {', '.join(f'0x{a:08X}' for a in sorted(addrs))}")
            continue
        a = next(iter(addrs))
        if ff9sym.RAM_LO <= a < ff9sym.RAM_HI or 0x80000000 <= a < ff9sym.RAM_LO:
            lines.append(f"PROVIDE({name} = psx_ram + 0x{a & 0x1FFFFF:X});")
        elif ff9sym.SPAD_LO <= a < ff9sym.SPAD_HI:
            lines.append(f"PROVIDE({name} = psx_scratchpad + 0x{a & 0x3FF:X});")
    # D_XXXXXXXX names the symbol files don't list yet: the name is the address
    for name in sorted(set().union(*(undefined(objs[b]) for b in bins)) - set(syms)):
        m = re.match(r"^D_([0-9A-F]{8})$", name)
        if m and ff9sym.RAM_LO <= int(m.group(1), 16) < ff9sym.RAM_HI:
            lines.append(f"PROVIDE({name} = psx_ram + 0x{int(m.group(1), 16) & 0x1FFFFF:X}); /* from its name */")
            report.append(f"placed by name: {name} (not in the decomp's symbol files)")
    dups = sorted(defined_in_src & defined_in_equiv)
    for name in dups:
        report.append(f"duplicate: {name} is in both src/ and src_equiv/ (the matched src/ one is used)")
    for name, vs in sorted(aliases.items()):
        if len(vs) > 1:
            sys.exit(f"gen_link.py: {name} resolves to {sorted(vs)} in different binaries")
        lines.append(f"PROVIDE({name} = {next(iter(vs))});")
    (GEN / "psx_syms.ld").write_text("\n".join(lines) + "\n")

    # --- trampolines for references whose target depends on the resident overlay ---------------------------
    # psx_ovl_resident[slot] holds the file id of the overlay loaded in that slot (port/psx/mem.c). The veneers
    # only use a scratch register (ARM ip, x86 eax), so arguments reach the target untouched, as on the PS1.
    def table(directive):
        out = ["", '    .section .rodata.psx_overlays,"a"', "    .balign 4", "    .global psx_overlays",
               "psx_overlays:"]
        for b, bi in sorted(bins.items()):
            if b != "main" and not b.startswith("fx_"):
                out.append(f"    {directive} 0x{int(b[4:], 16):X}, {SLOTS[bi.vram]}, psxrel_{b}")
        out += [f"    {directive} 0, 0, 0"]
        # Native battle effect modules (port/game/effect_native.c): load address, code range, an FNV-1a hash of
        # the code bytes that identifies the image once it has landed, the relocations, and the module's functions
        # as (PS1 address, native address) pairs. Code bytes are never relocated, so the hash stays valid.
        # On the DS (ARM) the modules' code isn't linked in: each module is linked on its own at psx_fx_area and
        # loaded from SD when its image lands (tools/pack_fxmods.py, effect_native.c); the table gives its index in
        # the pack instead, and the relocations and functions travel in the module's header (fxhead_<module>.s).
        overlay = directive == ".4byte"
        fx = [b for b in sorted(bins) if b.startswith("fx_")]
        out += ["", '    .section .rodata.psx_fxmods,"a"', "    .balign 4", "    .global psx_fxmods", "psx_fxmods:"]
        for index, b in enumerate(fx):
            bi = bins[b]
            lo, hi = bi.text_range()
            h = 0x811C9DC5
            for byte in bi.bytes[bi.text:bi.data]:
                h = ((h ^ byte) * 0x01000193) & 0xFFFFFFFF
            fs = sorted((a, n) for n, a in funcs.get(b, {}).items() if a is not None)
            if overlay:
                out.append(f"    {directive} 0x{bi.vram:X}, 0x{lo:X}, 0x{hi:X}, 0x{h:X}, {index}")
            else:
                out.append(f"    {directive} 0x{bi.vram:X}, 0x{lo:X}, 0x{hi:X}, 0x{h:X}, psxrel_{b}, {len(fs)}, psxfx_{b}")
        out += [f"    {directive} 0, 0, 0, 0, 0" if overlay else f"    {directive} 0, 0, 0, 0, 0, 0, 0"]
        for b in fx:
            fs = sorted((a, n) for n, a in funcs.get(b, {}).items() if a is not None)
            pairs = [f"    {directive} 0x{a:X}, {qualified(b, n)}" for a, n in fs]
            if not overlay:
                out += [f"psxfx_{b}:"] + pairs
                continue
            # the module's header, first in its image: magic, its zero-filled range, relocations, functions
            (GEN / f"fxhead_{b}.s").write_text("\n".join([
                "/* Generated by tools/gen_link.py: an effect module's header (port/game/effect_native.c). */",
                '    .section .fxhead,"a"', "    .balign 4", "    .global psx_fxhead", "psx_fxhead:",
                f"    .4byte 0x314D5846, __fx_bss_start, __fx_bss_end, psxrel_{b}, {len(fs)}"] + pairs) + "\n")
        return out

    arm = ["/* Generated by tools/gen_link.py: calls whose target is whichever overlay is resident. */",
           "    .syntax unified", "    .arm", '    .section .text.psx_tramp,"ax"']
    x86 = ["/* Generated by tools/gen_link.py: calls whose target is whichever overlay is resident. */",
           '    .section .text.psx_tramp,"ax"']
    for sym, targets in sorted(tramps.items()):
        arm += ["", "    .balign 4", f"    .global {sym}", f"    .type {sym}, %function", f"{sym}:"]
        x86 += ["", "    .balign 4", f"    .global {sym}", f"    .type {sym}, @function", f"{sym}:"]
        for binary, target in targets:
            slot, ovl_id = SLOTS[bins[binary].vram], int(binary[4:], 16)
            arm += ["    ldr ip, =psx_ovl_resident", f"    ldrb ip, [ip, #{slot}]", f"    cmp ip, #0x{ovl_id:X}",
                    f"    ldreq ip, ={target}", "    bxeq ip"]
            x86 += [f"    movzbl psx_ovl_resident+{slot}, %eax", f"    cmpl $0x{ovl_id:X}, %eax", f"    je {target}"]
        arm += [f"    ldr r0, ={sym}_name", "    b psx_bad_overlay_call", "    .pool",
                '    .section .rodata.psx_tramp_names,"a"', f'{sym}_name: .asciz "{sym}"',
                '    .section .text.psx_tramp,"ax"']
        x86 += [f"    pushl ${sym}_name", "    call psx_bad_overlay_call",
                '    .section .rodata.psx_tramp_names,"a"', f'{sym}_name: .asciz "{sym}"',
                '    .section .text.psx_tramp,"ax"']
    (GEN / "tramp_arm.s").write_text("\n".join(arm + table(".4byte")) + "\n")
    # The DS keeps one slot's overlays in one place (the Makefile prefixes their sections .psxovl_<id>): these
    # OVERLAY statements, spliced into the ARM9 linker script before its heap (Makefile ff9ds_arm9.ld), put each
    # slot's overlays over each other outside the load segments, so ndstool leaves them out; port/ds/ovl_pack.c
    # reads the one the game loads.
    # The other slots' overlays are small: each one's native code goes in its own MIPS code range in psx_ram (dead PS1
    # RAM while it is resident), 8-byte aligned, linked at that address; port/ds/ovl_pack.c reads it there again when
    # disc sectors land on it. Slot 0's MIPS code ranges hold the hybrid's buffers in battle (ovl0b_layout.h).
    slots = {}
    in_place = []
    for b, bi in sorted(bins.items()):
        if b != "main" and not b.startswith("fx_"):
            if SLOTS[bi.vram] == 0:
                slots.setdefault(0, []).append(b[4:])
            else:
                lo, hi = bi.text_range()
                in_place.append((b[4:], (lo + 7) & ~7, hi))
    ld = ["    /* Generated by tools/gen_link.py: the code overlays' native areas (Makefile ds link). */"]
    for slot, ids in sorted(slots.items()):
        ld += [f"    OVERLAY ALIGN(32) : NOCROSSREFS", "    {"]
        ld += [f"        .psxovl_{i} {{ *(.psxovl_{i}.*) }}" for i in ids]
        ld += ["    } >ewram :NONE"]
        if slot == 0:
            # main's image (above) at slot 0's place too; outside the OVERLAY, which forbids its references to
            # those overlays' functions (function tables in main's data)
            first = ids[0]
            ld += ["    __psxovl_slot0_end = .;",
                   f"    .psxovl_ff ADDR(.psxovl_{first}) : AT(0x0F000000) {{ *(.psxovl_ff.*) }} :NONE",  # an LMA of its own: never loaded
                   "    . = MAX(., __psxovl_slot0_end);",
                   f'    ASSERT(ADDR(.psxovl_ff) + SIZEOF(.psxovl_ff) <= __psxovl_slot0_end, "main\'s image is larger than slot 0")']
            if "0a" in ids:
                # gx_hybrid.c use_place: the field's second scene buffer goes to slot 0's room after ovl_0a's native
                # code, and only that leaves ovl_0a's MIPS range room for the field models' ring (without it, no
                # native models: every field in software, silently)
                layout = (Path(__file__).resolve().parent.parent / "port/include/ovl0a_layout.h").read_text()
                prims = int(re.search(r"#define OVL0A_FIELD_PRIMS\s+(\d+)", layout).group(1))
                ld += [f"    ASSERT(__psxovl_slot0_end - (ADDR(.psxovl_0a) + SIZEOF(.psxovl_0a)) >= {prims * 48 + 32}, "
                       f'"ovl_0a\'s native code leaves slot 0 no room for a field scene buffer ({prims} primitives)")']
    ld += ["    __psxovl_areas_end = .;"]
    for k, (i, lo, hi) in enumerate(in_place):  # LMAs of their own: never loaded
        ld += [f"    .psxovl_{i} (psx_ram + 0x{lo & 0x1FFFFF:X}) : AT(0x0F{k + 1:02X}0000) {{ *(.psxovl_{i}.*) }} :NONE",
               f'    ASSERT(SIZEOF(.psxovl_{i}) <= 0x{hi - lo:X}, "ovl_{i}\'s native code is larger than its MIPS code")']
    ld += ["    . = __psxovl_areas_end;"]
    (GEN / "psx_ovl_areas.ld").write_text("\n".join(ld) + "\n")
    x86 += table(".long")
    (GEN / "tramp_x86.s").write_text("\n".join(x86) + "\n")

    (GEN / "report.txt").write_text("".join(r + "\n" for r in sorted(set(report))))
    kinds = {}
    for r in set(report):
        kinds[r.split(":")[0]] = kinds.get(r.split(":")[0], 0) + 1
    print("gen_link:", ", ".join(f"{k} {v}" for k, v in sorted(kinds.items())) or "clean")


if __name__ == "__main__":
    main()
