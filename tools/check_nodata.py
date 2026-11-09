#!/usr/bin/env python3
"""Check that built executables carry none of the game's bytes: every 64-byte window of the disc's executable and of
the code overlays and effect modules (each at 4-byte steps, skipping windows of few distinct bytes: zeros, padding,
small tables) is looked for in each file, at any alignment.

Not counted: text (the decomp's C keeps the game's debug messages as string literals, so they are in the source), and
the ranges in ALLOWED: data the repository's own source has, which the executable also links (SDK tables).

    tools/check_nodata.py build/3ds/ff9ds.elf build/ff9.3dsx build/ff9.cia

Exit 1, with the matching offsets, when any is found. Run before publishing a build (make builder runs it)."""
import sys
from pathlib import Path

WINDOW = 64
# (file, first byte, end): source tables that the executable also holds
ALLOWED = [("SLUS_012.51", 0x586D0, 0x58930)]  # libspu's reverb presets: port/include/psx_spu_presets.h


def sources():
    root = Path(__file__).resolve().parent.parent
    us = root / "decomp/disks/us" if (root / "decomp/disks/us").exists() else root / "disks/us"
    files = sorted(us.glob("SLUS_*")) + sorted((us / "ovl").glob("*.bin")) + sorted((us / "fxmod").glob("*.bin"))
    if not files:
        sys.exit(f"check_nodata: no executable or overlays under {us}")
    return files


def main():
    targets = [Path(p) for p in sys.argv[1:]]
    if not targets:
        sys.exit(__doc__)
    windows = {}
    for src in sources():
        b = src.read_bytes()
        for off in range(0, len(b) - WINDOW + 1, 4):
            w = b[off:off + WINDOW]
            text = sum(32 <= c < 127 or c in (0, 9, 10) for c in w)
            allowed = any(src.name == n and lo <= off and off + WINDOW <= hi for n, lo, hi in ALLOWED)
            if len(set(w)) >= 24 and text < WINDOW * 7 // 8 and not allowed:
                windows.setdefault(w[:16], []).append((w, src.name, off))
    bad = 0
    for t in targets:
        data = t.read_bytes()
        hits = []
        for i in range(len(data) - WINDOW + 1):
            cands = windows.get(data[i:i + 16])
            if cands:
                for w, name, off in cands:
                    if data[i:i + WINDOW] == w:
                        hits.append((i, name, off))
                        break
        if hits:
            bad = 1
            print(f"{t}: {len(hits)} windows of game data, e.g. " +
                  ", ".join(f"0x{i:X} = {n}+0x{o:X}" for i, n, o in hits[:5]))
        else:
            print(f"{t}: no game data")
    sys.exit(bad)


if __name__ == "__main__":
    main()
