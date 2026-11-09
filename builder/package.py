#!/usr/bin/env python3
"""Make the builder a single executable for this OS (PyInstaller): dist/ff9-3ds-builder-<os>[.exe].

    python builder/package.py --payload DIR      (DIR: ff9.3dsx, ff9.cia, linkinfo.json from `make payload`)

Needs builder/requirements.txt installed (PyInstaller, and splat at the decomp's version). Bundles the builder,
the payload (the program, which carries none of the game's data), and what it runs on the user's discs: the port's
pack/bake/relocation tools and the decomp's committed configs, extraction and field tools, and its C files reduced
to their INCLUDE_ASM/INCLUDE_RODATA lines (what splat reads). No disc is needed or read here."""
import argparse
import os
import platform
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DECOMP = ROOT / "decomp"
PORT_TOOLS = ["pack_discs.py", "bake_maps.py", "gen_relocs.py", "psxlink.py", "ff9sym.py"]
DECOMP_TOOLS = ["field_maps.py", "encounter_catalog.py", "ff9img.py", "effect_modules.py", "evtscript/evtdis.py",
                "evtscript/edis.py", "evtscript/optab.json"]


def tracked(repo, sub):
    """the files git tracks under repo/sub (falls back to all files there outside a git checkout)"""
    try:
        out = subprocess.run(["git", "-C", str(repo), "ls-files", sub], capture_output=True, text=True, check=True)
        return [repo / p for p in out.stdout.splitlines()]
    except (OSError, subprocess.CalledProcessError):
        return [p for p in (repo / sub).rglob("*") if p.is_file()]


def stage(payload, out):
    pkg = out / "ff9builder"
    shutil.copytree(ROOT / "builder/ff9builder", pkg, ignore=shutil.ignore_patterns("__pycache__", "payload", "vendor"))
    vendor = pkg / "vendor"
    (vendor / "tools").mkdir(parents=True)
    for t in PORT_TOOLS:
        shutil.copy(ROOT / "tools" / t, vendor / "tools" / t)
    d = vendor / "decomp"
    for t in DECOMP_TOOLS:
        (d / "tools" / t).parent.mkdir(parents=True, exist_ok=True)
        shutil.copy(DECOMP / "tools" / t, d / "tools" / t)
    for f in tracked(DECOMP, "config"):
        dst = d / f.relative_to(DECOMP)
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy(f, dst)
    for f in tracked(DECOMP, "src"):
        if f.suffix == ".c":
            dst = d / f.relative_to(DECOMP)
            dst.parent.mkdir(parents=True, exist_ok=True)
            lines = [l for l in f.read_text(errors="replace").splitlines(True)
                     if re.match(r"\s*INCLUDE_(ASM|RODATA)\b", l)]
            dst.write_text("".join(lines))
    # edis.py reads the event opcode names from the decomp's ff9.h: just those lines
    (d / "include").mkdir()
    (d / "include/ff9.h").write_text("".join(l for l in (DECOMP / "include/ff9.h").read_text().splitlines(True)
                                             if l.startswith("#define EVT_OP_")))
    (pkg / "payload").mkdir()
    for name in ("ff9.3dsx", "ff9.cia", "linkinfo.json"):
        src = Path(payload) / name
        if not src.is_file():
            sys.exit(f"package.py: no {src}")
        shutil.copy(src, pkg / "payload" / name)
    shutil.copy(ROOT / "builder/ff9-3ds-builder.py", out / "ff9-3ds-builder.py")
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--payload", required=True)
    ap.add_argument("--work", default=str(ROOT / "build/builder-pkg"))
    ap.add_argument("--dist", default=str(ROOT / "dist"))
    a = ap.parse_args()
    work = Path(a.work)
    shutil.rmtree(work, ignore_errors=True)
    staged = stage(a.payload, work / "src")
    system = {"Windows": "windows", "Darwin": "macos", "Linux": "linux"}.get(platform.system(), platform.system())
    name = f"ff9-3ds-builder-{system}-{platform.machine().lower()}"
    sep = ";" if os.name == "nt" else ":"
    cmd = [sys.executable, "-m", "PyInstaller", "--onefile", "--noconfirm", "--clean", "--name", name,
           "--distpath", a.dist, "--workpath", str(work / "build"), "--specpath", str(work),
           "--paths", str(staged), "--add-data", f"{staged / 'ff9builder'}{sep}ff9builder",
           "--hidden-import", "ff9builder.gui", "--hidden-import", "tkinter",
           "--collect-submodules", "splat", "--collect-data", "splat",
           "--collect-submodules", "spimdisasm", "--collect-data", "spimdisasm",
           "--collect-submodules", "rabbitizer", "--collect-all", "intervaltree"]
    # splat imports its N64 segment types too, and they load pygfxd's native library through ctypes, which sits
    # beside pygfxd.py where PyInstaller doesn't look
    import pygfxd
    for lib in Path(pygfxd.__file__).parent.glob("*gfxd*"):
        if lib.suffix in (".so", ".dylib", ".dll"):
            cmd += ["--add-binary", f"{lib}{sep}."]
    if system == "windows":
        cmd.append("--windowed")  # (no console window behind the builder's; the --cli form then prints nothing)
    cmd.append(str(staged / "ff9-3ds-builder.py"))
    subprocess.run(cmd, check=True)
    print(f"package.py: {a.dist}/{name}{'.exe' if system == 'windows' else ''}")


if __name__ == "__main__":
    main()
