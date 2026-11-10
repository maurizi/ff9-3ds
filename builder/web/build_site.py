#!/usr/bin/env python3
"""Assemble the web builder (a static site: GitHub Pages) into OUT.

    python builder/web/build_site.py --payload DIR --rabbitizer WHEEL --out DIR

DIR (payload): ff9.3dsx, ff9.cia, linkinfo.json (make payload). WHEEL: rabbitizer built for Pyodide
(builder/web/build_rabbitizer.sh). Needs npm (Pyodide's runtime, pinned in PYODIDE_VERSION) and pip (the pure-Python
wheels, pinned in builder/requirements.txt), and the network for those. No disc is read: the site carries the
program (no game data) and the tools that run on the user's discs, in their browser."""
import argparse
import hashlib
import json
import shutil
import subprocess
import sys
import tempfile
import urllib.request
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
sys.path.insert(0, str(ROOT / "builder"))
import package  # noqa: E402  (stage(): the same bundle the desktop builder has)

PYODIDE_VERSION = (HERE / "PYODIDE_VERSION").read_text().strip()
RUNTIME = ["pyodide.mjs", "pyodide.js", "pyodide.asm.mjs", "pyodide.asm.js", "pyodide.asm.wasm", "python_stdlib.zip",
           "pyodide-lock.json"]
PYODIDE_PACKAGES = ["pyyaml", "tqdm", "sortedcontainers"]  # (from Pyodide's own index: the lock file's hashes)
SITE_FILES = ["index.html", "app.js", "worker.js", "bigfs.js", "n64_stubs.py"]


def pinned(name):
    for line in (ROOT / "builder/requirements.txt").read_text().splitlines():
        line = line.split("#")[0].strip()
        if line.split("[")[0].split("==")[0] == name:
            return line
    sys.exit(f"build_site.py: {name} isn't pinned in builder/requirements.txt")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--payload", required=True)
    ap.add_argument("--rabbitizer", required=True)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    out = Path(a.out)
    shutil.rmtree(out, ignore_errors=True)
    out.mkdir(parents=True)
    for f in SITE_FILES:
        shutil.copy(HERE / f, out / f)

    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        # Pyodide's runtime (npm) and the packages of its index the builder needs
        subprocess.run(["npm", "pack", f"pyodide@{PYODIDE_VERSION}", "--silent"], cwd=tmp, check=True,
                       stdout=subprocess.DEVNULL)
        tgz = next(tmp.glob("pyodide-*.tgz"))
        shutil.unpack_archive(str(tgz), str(tmp / "npm"))
        rt = out / "pyodide"
        rt.mkdir()
        for f in RUNTIME:
            src = tmp / "npm/package" / f
            if src.exists():
                shutil.copy(src, rt / f)
        lock = json.loads((rt / "pyodide-lock.json").read_text())
        for name in PYODIDE_PACKAGES:
            pkg = lock["packages"][name]
            url = f"https://cdn.jsdelivr.net/pyodide/v{PYODIDE_VERSION}/full/{pkg['file_name']}"
            data = urllib.request.urlopen(url).read()
            if hashlib.sha256(data).hexdigest() != pkg["sha256"]:
                sys.exit(f"build_site.py: {pkg['file_name']}'s hash isn't the lock file's")
            (rt / pkg["file_name"]).write_bytes(data)
            for dep in pkg.get("depends", []):
                if dep not in PYODIDE_PACKAGES:
                    sys.exit(f"build_site.py: {name} needs {dep}, which the site doesn't carry")

        # the pure-Python wheels at the desktop builder's versions, and rabbitizer for Pyodide
        wheels = out / "wheels"
        wheels.mkdir()
        reqs = [pinned("splat64").replace("[mips]", ""), pinned("spimdisasm"), "intervaltree==3.1.0",
                "colorama==0.4.6", "pylibyaml==0.1.0"]
        subprocess.run([sys.executable, "-m", "pip", "wheel", "-q", "--no-deps", "-w", str(wheels), *reqs], check=True)
        shutil.copy(a.rabbitizer, wheels)
        (wheels / "wheels.json").write_text(json.dumps(sorted(p.name for p in wheels.glob("*.whl"))))

        # the builder, its tools and the program (the desktop builder's bundle)
        staged = package.stage(a.payload, tmp / "stage")
        with zipfile.ZipFile(out / "bundle.zip", "w", zipfile.ZIP_DEFLATED) as z:
            for p in sorted((staged / "ff9builder").rglob("*")):
                if p.is_file() and "__pycache__" not in p.parts:
                    z.write(p, p.relative_to(staged))
    size = sum(p.stat().st_size for p in out.rglob("*") if p.is_file())
    print(f"build_site.py: {out}, {size / 1e6:.1f} MB")


if __name__ == "__main__":
    main()
