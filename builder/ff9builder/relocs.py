"""psxrel.bin from the user's discs: the half of the program's link that needs the game's bytes (tools/gen_relocs.py).

The executable, the code overlays and the native effect modules are extracted from disc 1 into a decomp-shaped
tree with the decomp's committed configs and its C files reduced to their INCLUDE_ASM/INCLUDE_RODATA lines (all
splat reads of them), split by splat exactly as the decomp's `make split` does (so the asm is the developer's, file
for file), and gen_relocs.py reads that with the program's linkinfo.json."""
import builtins
import hashlib
import importlib
import os
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

SECTOR = 2048


def disc_open(views):
    """an `open` for the decomp tools' modules: the names in `views` are files on the disc, anything else is real"""
    def _open(path, mode='r', *a, **k):
        if path in views:
            return views[path]()
        return builtins.open(path, mode, *a, **k)
    return _open


def splat_command(yamls):
    """splat over some configs, run by this program itself (a frozen build has no python to run it with)"""
    if getattr(sys, 'frozen', False):
        return [sys.executable, '--splat'] + yamls
    return [sys.executable, '-m', 'ff9builder', '--splat'] + yamls


def run_splat(yamls):
    """the --splat entry point: `python -m splat split YAML` for each, in this process, each with a fresh splat
    (its state is module globals: they are dropped between configs). One process for many configs: a single-file
    build unpacks itself at each start."""
    import runpy
    for y in yamls:
        for m in [m for m in sys.modules if m.split('.')[0] in ('splat', 'spimdisasm')]:
            del sys.modules[m]
        sys.argv = ['splat', 'split', y]
        try:
            runpy.run_module('splat', run_name='__main__')
        except SystemExit as e:
            if e.code:
                raise
        print(f'split {y}', flush=True)


def _sha1_lines(path):
    out = {}
    for line in open(path):
        f = line.split()
        if len(f) == 2:
            out[os.path.basename(f[1])] = f[0]
    return out


def build(disc1, vendor, linkinfo, tree, out, log=print, progress=None):
    """disc1: a discs.Disc; vendor: the bundled tools; tree: an empty work directory; writes psxrel.bin to out"""
    import json
    info = json.load(open(linkinfo))
    decomp = os.path.join(vendor, 'decomp')
    tools = os.path.join(vendor, 'tools')
    for p in (os.path.join(decomp, 'tools'), tools):
        if p not in sys.path:
            sys.path.insert(0, p)
    # the decomp tree: its configs and C stubs (copied: splat writes beside them), and the disc's binaries
    import shutil
    for d in ('config', 'src'):
        shutil.copytree(os.path.join(decomp, d), os.path.join(tree, d))
    us = os.path.join(tree, 'disks', 'us')
    os.makedirs(os.path.join(us, 'ovl'))
    os.makedirs(os.path.join(us, 'fxmod'))

    exe = disc1.boot.upper().split(';')[0]
    lba, size, _ = disc1.entries()[exe]
    with open(os.path.join(us, 'SLUS_012.51'), 'wb') as f:
        f.write(disc1.user(lba, size))

    ff9img = importlib.import_module('ff9img')
    effect_modules = importlib.import_module('effect_modules')
    ff9img.open = disc_open({'disc1:FF9.IMG': lambda: disc1.file_view('FF9.IMG')})
    img = ff9img.Img('disc1:FF9.IMG')
    want = _sha1_lines(os.path.join(decomp, 'config', 'checksums.ovl.us.sha1'))
    ovls = [b[4:] for b in info['bins'] if b.startswith('ovl_')]
    for fid, sec, n in img.files(img.dirs[0]):
        name = f'{fid:02x}'
        if name in ovls:
            data = img.read(sec * SECTOR, n)
            if want.get(f'ovl_{name}.bin') not in (None, hashlib.sha1(data).hexdigest()):
                raise ValueError(f'overlay {name} differs from the NTSC-U release\'s')
            with open(os.path.join(us, 'ovl', name + '.bin'), 'wb') as f:
                f.write(data)
    log('Extracted the executable and code overlays.')
    sha = {}
    for line in open(os.path.join(decomp, 'config', 'fxmods.us.txt')):
        f = line.split('#')[0].split()
        if len(f) >= 6:
            sha[f[0]] = f[5]
    mods = effect_modules.scan_disc('disc1:FF9.IMG')
    found = 0
    for m in mods.values():
        name = effect_modules.mod_name(m)
        if name in info['fxmods']:
            if sha.get(name) != m['sha1']:
                raise ValueError(f'effect module {name} differs from the NTSC-U release\'s')
            with open(os.path.join(us, 'fxmod', name + '.bin'), 'wb') as f:
                f.write(m['image'])
            found += 1
    if found != len(info['fxmods']):
        raise ValueError(f'found {found} of the {len(info["fxmods"])} effect modules on disc 1')
    log(f'Extracted {found} battle effect modules.')

    # splat, as the decomp's make split (the executable and overlays in turn, the modules 8 at a time)
    yamls = ['config/slus_012.51.yaml'] + [f'config/ovl_{o}.yaml' for o in ovls]
    fx = [f'config/fxmod/{m}.yaml' for m in info['fxmods']]
    total = len(yamls) + len(fx)
    done = [0]
    env = dict(os.environ, PYTHONPATH=os.pathsep.join([os.path.dirname(os.path.dirname(os.path.abspath(__file__)))]
                                                       + os.environ.get('PYTHONPATH', '').split(os.pathsep)))

    def split(batch):
        p = subprocess.Popen(splat_command(batch), cwd=tree, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                             text=True, env=env, creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        tail = []
        for line in p.stdout:
            tail = (tail + [line])[-40:]
            if line.startswith('split '):
                done[0] += 1
                if progress:
                    progress(done[0], total * 1.25)
        if p.wait():
            raise RuntimeError(f'splat failed in {batch[0]}..{batch[-1]}:\n{"".join(tail)}')
    split(yamls)
    workers = min(8, os.cpu_count() or 1)
    with ThreadPoolExecutor(workers) as ex:
        list(ex.map(split, [fx[i::workers] for i in range(workers)]))
    log('Split the binaries (splat).')

    ff9sym = importlib.import_module('ff9sym')
    gen_relocs = importlib.import_module('gen_relocs')
    from pathlib import Path
    ff9sym.DECOMP = Path(tree)
    n = gen_relocs.build(linkinfo, out)
    if progress:
        progress(1, 1)
    log(f'Relocation tables: {n} bytes.')
