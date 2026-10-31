#!/usr/bin/env python3
"""Compare the 3DS renderer with the host's software renderer (the reference) on a scenario, frame by frame.

  tools/render_diff.py tests/scenarios/X.cfg [--every 120] [--frames 3600] [--threshold 3]

Runs the host build (--dump) and a 3DS build in Azahar (DUMP_EVERY, SPUSYNC=1: the SPU in step with the game, so
both runs show the same frames), then reports, per dumped frame, the share of pixels that differ by more than 48 in
some channel (small sub-pixel and filtering differences don't count), and writes build/render_diff/<scenario>.png
with the worst frames side by side (3DS above, host below). Run from the host (it drives Docker and Azahar)."""
import argparse, glob, os, shutil, subprocess, sys
from pathlib import Path
from PIL import Image, ImageChops, ImageDraw

ROOT = Path(__file__).resolve().parent.parent
SD = Path.home() / ".var/app/org.azahar_emu.Azahar/data/azahar-emu/sdmc/3ds/ff9"

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("scenario", type=Path)
    ap.add_argument("--every", type=int, default=120)
    ap.add_argument("--frames", type=int, default=3600)
    ap.add_argument("--threshold", type=float, default=3.0, help="percent of differing pixels that flags a frame")
    ap.add_argument("--worst", type=int, default=4)
    a = ap.parse_args()
    name = a.scenario.stem
    out = ROOT / "build/render_diff" / name
    host = out / "host"
    shutil.rmtree(out, ignore_errors=True)
    host.mkdir(parents=True)
    (SD / "scenarios").mkdir(parents=True, exist_ok=True)
    shutil.copy(a.scenario, SD / "scenarios" / a.scenario.name)
    subprocess.run([str(ROOT / "tools/docker.sh"), "build/pc/ff9ds", "--scenario", str(a.scenario), "--frames",
                    str(a.frames), "--status", "0", "--dump", str(a.every), "--dump-dir", str(host.relative_to(ROOT))],
                   cwd=ROOT, check=False, capture_output=True)
    subprocess.run([str(ROOT / "tools/docker.sh"), "make", "-j12", "3ds", f"SCENARIO=sdmc:/3ds/ff9/scenarios/{a.scenario.name}",
                    f"DUMP_EVERY={a.every}", "SPUSYNC=1"], cwd=ROOT, check=True, capture_output=True)
    dump = SD / "dump"
    dump.mkdir(exist_ok=True)
    for f in dump.glob("f*.ppm"):
        f.unlink()
    exe = out / "ff9.3dsx"
    shutil.copy(ROOT / "build/ff9-test.3dsx", exe)
    subprocess.run([str(ROOT / "tools/azahar.sh"), "-t", str(a.frames // 60 + 12), "-o", str(out / "azahar"), str(exe)],
                   cwd=ROOT, check=False, capture_output=True)
    rows = []
    for f in range(a.every, a.frames + 1, a.every):
        try:
            d3 = Image.open(dump / f"f{f:06d}.ppm").convert("RGB")
            hp = Image.open(host / f"frame_{f:06d}.ppm").convert("RGB")
        except (OSError, ValueError):
            continue
        w, h = min(d3.width, hp.width), min(d3.height, hp.height)
        if w < 64 or h < 64:
            continue
        diff = ImageChops.difference(d3.crop((0, 0, w, h)), hp.crop((0, 0, w, h)))
        big = sum(1 for px in diff.getdata() if max(px) > 48) * 100.0 / (w * h)
        rows.append((f, big, d3, hp))
    if not rows:
        print(f"{name}: no frames to compare")
        return 1
    flagged = [r for r in rows if r[1] > a.threshold]
    print(f"{name}: {len(rows)} frames, {len(flagged)} over {a.threshold}% (" +
          ", ".join(f"{r[0]}:{r[1]:.1f}%" for r in rows) + ")")
    worst = sorted(rows, key=lambda r: -r[1])[:a.worst]
    sheet = Image.new("RGB", (324 * len(worst), 2 * 244 + 14), (30, 30, 30))
    draw = ImageDraw.Draw(sheet)
    for i, (f, big, d3, hp) in enumerate(worst):
        sheet.paste(d3, (i * 324, 14))
        sheet.paste(hp, (i * 324, 14 + 244))
        draw.text((i * 324 + 4, 1), f"frame {f}: {big:.1f}%", fill=(255, 255, 0))
    sheet.save(out.parent / f"{name}.png")
    return 1 if flagged else 0

if __name__ == "__main__":
    sys.exit(main())
