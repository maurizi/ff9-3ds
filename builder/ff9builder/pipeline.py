"""From the four discs to ff9-full.3dsx / ff9-full.cia: verify the discs, pack them (discs.bin), bake the bottom
screen's field maps (maps.bin), and put both in the RomFS of the prebuilt, data-free program."""
import importlib
import os
import shutil
import struct
import sys
import tempfile

from . import ctr, relocs
from .discs import Disc, DiscError

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))  # the repository, when run from a checkout


def _resource(*parts):
    """a bundled file (PyInstaller unpacks next to sys._MEIPASS), else the package's own, else the checkout's"""
    for base in (getattr(sys, '_MEIPASS', None) and os.path.join(sys._MEIPASS, 'ff9builder'), HERE):
        if base and os.path.exists(os.path.join(base, *parts)):
            return os.path.join(base, *parts)
    return None


def payload(name, version='us'):
    """the prebuilt program for a release (discs.Release.version): ff9.3dsx, ff9.cia and linkinfo.json, in a bundle's
    payload/ (us) or payload/<version>/, in a checkout's build/ or build/<version>/ (make VERSION=...)"""
    sub = () if version == 'us' else (version,)
    p = _resource('payload', *sub, name)
    if p:
        return p
    build = os.path.join(ROOT, 'build', *sub)
    for p in (os.path.join(build, name), os.path.join(build, 'gen', name)):
        if os.path.isfile(p):
            return p
    return None


def vendor_dir():
    """the tools the builder runs (tools/ and decomp/{tools,config,src,include}): bundled, else the checkout"""
    return _resource('vendor') or ROOT


def _modules():
    """pack_discs, bake_maps, the decomp's field_maps, and banner3d"""
    vendor = vendor_dir()
    paths = [os.path.join(vendor, 'tools'), os.path.join(vendor, 'decomp', 'tools')]
    for p in reversed(paths):
        if p not in sys.path:
            sys.path.insert(0, p)
    return (importlib.import_module('pack_discs'), importlib.import_module('bake_maps'),
            importlib.import_module('field_maps'), importlib.import_module('banner3d'))


def identify(paths):
    """{disc number: Disc} from dump files in any order (DiscError on a wrong or doubled disc, or discs of two
    releases)"""
    out = {}
    for p in paths:
        d = Disc(p)
        other = next(iter(out.values()), None)
        if other and other.release is not d.release:
            raise DiscError(f'{os.path.basename(p)} is the {d.release.name} release\'s disc {d.number}, '
                            f'{os.path.basename(other.source)} the {other.release.name} release\'s: all four discs '
                            f'must be of one release')
        if d.number in out:
            raise DiscError(f'{os.path.basename(p)} and {os.path.basename(out[d.number].source)} are both disc {d.number}')
        out[d.number] = d
    return out


class Progress:
    """weighted steps -> one 0..1 fraction; report(fraction, message) is the caller's"""

    def __init__(self, report, steps):
        self.report, self.steps = report, steps
        self.total = sum(w for _, w in steps)
        self.at = 0
        self.cur = None

    def step(self, name, message):
        if self.cur is not None:
            self.at += dict(self.steps)[self.cur]
        self.cur = name
        self.report(self.at / self.total, message)

    def part(self, done, total, message=None):
        w = dict(self.steps)[self.cur]
        self.report((self.at + w * (done / total if total else 1)) / self.total, message)


def make_banner(banner3d, discs, imgs, bases, log):
    """(banner, SMDH) made from the discs (tools/banner3d.py: the heroes' models, the opening movie's logo) for the program's own (bannertool's flat banner, a plain icon); (None, None) keeps those if
    anything fails: a nicer banner is not worth a failed build"""
    try:
        if bases.get('ff9.cia'):
            with open(bases['ff9.cia'], 'rb') as f:
                exefs = ctr.cia_exefs(f.read())
            banner, smdh = exefs['banner'], exefs['icon']
        else:
            banner, smdh = None, smdh_of_3dsx(bases['ff9.3dsx'])
        d1 = discs[1]
        sectors = banner3d.movie_sectors(d1.raw_sectors, d1.movie_lba(*banner3d.LOGO_MOVIE))
        notes = []
        out = banner3d.build(dict(imgs), sectors, banner, smdh, exe=d1.file_view(d1.boot).read(),
                             notes=notes, version=d1.release.version)
        log('HOME Menu banner and icon made from the discs.')
        for note in notes:
            log(note[0].upper() + note[1:] + '.')
        return out
    except Exception as e:  # noqa: BLE001 (any failure keeps the plain banner)
        log(f'Kept the plain HOME Menu banner ({type(e).__name__}: {e}).')
        return None, None


def smdh_of_3dsx(path):
    with open(path, 'rb') as f:
        prog = f.read()
    at, size = struct.unpack_from('<II', prog, 0x20)
    return prog[at:at + size]


def full_name(release, ext):
    """the output's name: ff9-full.cia, another release's ff9-es-full.cia ... (the program looks for its .3dsx by it, and
    keeps its folder on the SD card by the same name: two releases side by side)"""
    return f'ff9-full.{ext}' if release.version == 'us' else f'ff9-{release.version}-full.{ext}'


def build(discs, out_dir, make_3dsx=True, make_cia=True, log=print, report=lambda f, m=None: None, scratch=None):
    """discs: {number: Disc} (all four); writes out_dir/ff9-full.3dsx and/or ff9-full.cia; returns their paths.
    scratch: where the split of disc 1 goes (thousands of small files: the web builder keeps them in memory, its
    out_dir is for large ones), else out_dir's work folder"""
    missing = [n for n in range(1, 5) if n not in discs]
    if missing:
        raise DiscError('missing disc ' + ', '.join(map(str, missing)))
    release = discs[1].release
    bases = {}
    for want, name in ((make_3dsx, 'ff9.3dsx'), (make_cia, 'ff9.cia'), (True, 'linkinfo.json')):
        if want:
            bases[name] = payload(name, release.version)
            if not bases[name]:
                raise DiscError(f'the prebuilt program {name} for the {release.name} release is missing from this '
                                f'builder')
    pack_discs, bake_maps, fm, banner3d = _modules()
    os.makedirs(out_dir, exist_ok=True)
    steps = [('verify', 20), ('relocs', 30), ('pack', 45), ('maps', 10), ('banner', 3)]
    if make_3dsx:
        steps.append(('3dsx', 10))
    if make_cia:
        steps.append(('cia', 25))
    pr = Progress(report, steps)
    work = tempfile.mkdtemp(prefix='.ff9-build-', dir=out_dir)
    outputs = []
    try:
        pr.step('verify', 'Checking the discs')
        sizes = {n: os.path.getsize(discs[n].path) for n in discs}
        done = 0
        unchecked = []
        for n in sorted(discs):
            log(f'Checking disc {n} ({os.path.basename(discs[n].path)})')
            if not discs[n].verify(lambda d, t, base=done: pr.part(base + d, sum(sizes.values()))):
                unchecked.append(n)
            done += sizes[n]
        if unchecked:
            log(f'All four discs are Final Fantasy IX ({release.name}); FF9.IMG of disc '
                f'{", ".join(map(str, unchecked))} not checked (no reference for it in this builder yet).')
        else:
            log(f'All four discs are Final Fantasy IX ({release.name}), unmodified.')

        pr.step('relocs', 'Analysing the executable')
        rel_path = os.path.join(work, 'psxrel.bin')
        tree = tempfile.mkdtemp(prefix='decomp-', dir=scratch) if scratch else os.path.join(work, 'decomp')
        os.makedirs(tree, exist_ok=True)
        relocs.build(discs[1], vendor_dir(), bases['linkinfo.json'], tree, rel_path, log=log, progress=pr.part,
                     version=release.version)
        shutil.rmtree(tree, ignore_errors=True)

        pr.step('pack', 'Packing the discs')
        pack_path = os.path.join(work, 'discs.bin')
        try:
            pack_discs.pack({n: discs[n].path for n in discs}, pack_path, log=log, progress=pr.part)
        except SystemExit as e:  # (the tool's own errors)
            raise DiscError(str(e))

        pr.step('maps', 'Baking the field maps')
        ec = importlib.import_module('encounter_catalog')
        ec.open = relocs.disc_open({f'disc{n}:FF9.IMG': (lambda d=discs[n]: d.file_view('FF9.IMG')) for n in discs})
        imgs = [(n, fm.Img(f'disc{n}:FF9.IMG')) for n in sorted(discs)]
        data, fields = bake_maps.bake_all(fm, imgs, pr.part)
        maps_path = os.path.join(work, 'maps.bin')
        with open(maps_path, 'wb') as f:
            f.write(data)
        log(f'{fields} field maps baked.')
        files = [('discs.bin', pack_path), ('maps.bin', maps_path), ('psxrel.bin', rel_path)]

        pr.step('banner', 'Making the HOME Menu banner')
        banner, smdh = make_banner(banner3d, discs, imgs, bases, log)

        if make_3dsx:
            out = os.path.join(out_dir, full_name(release, '3dsx'))
            pr.step('3dsx', f'Writing {os.path.basename(out)}')
            ctr.write_3dsx(bases['ff9.3dsx'], files, out + '.part', pr.part, smdh=smdh)
            os.replace(out + '.part', out)
            outputs.append(out)
            log(f'Wrote {out}')
        if make_cia:
            out = os.path.join(out_dir, full_name(release, 'cia'))
            pr.step('cia', f'Writing {os.path.basename(out)}')
            ctr.write_cia(bases['ff9.cia'], files, out + '.part',
                          lambda d, t, phase: pr.part(d / 2 + (t / 2 if phase == 'write' else 0), t),
                          exefs={'banner': banner, 'icon': smdh} if banner else None)
            os.replace(out + '.part', out)
            outputs.append(out)
            log(f'Wrote {out}')
        report(1.0, 'Done')
        return outputs
    finally:
        shutil.rmtree(work, ignore_errors=True)
        for out in (full_name(release, '3dsx'), full_name(release, 'cia')):
            try:
                os.unlink(os.path.join(out_dir, out + '.part'))
            except OSError:
                pass
