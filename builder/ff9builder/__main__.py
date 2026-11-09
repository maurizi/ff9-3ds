"""ff9-3ds-builder: the window, or with discs on the command line, a build without one.

    ff9-3ds-builder                                         the window
    ff9-3ds-builder --out DIR [--no-3dsx] [--no-cia] DISC...   (any order; .cue, .ccd, .img, .bin, .iso)
    ff9-3ds-builder --self-test                              check the bundle (exit status only: CI)"""
import argparse
import os
import sys


def self_test():
    """everything the builder needs is in it and loads: the payload, the vendored tools, splat and its natives"""
    from . import pipeline
    for name in ('ff9.3dsx', 'ff9.cia', 'linkinfo.json'):
        if not pipeline.payload(name):
            sys.exit(f'self-test: no {name}')
    vendor = pipeline.vendor_dir()
    for rel in ('tools/gen_relocs.py', 'decomp/config/slus_012.51.yaml', 'decomp/tools/field_maps.py',
                'decomp/include/ff9.h'):
        if not os.path.isfile(os.path.join(vendor, rel)):
            sys.exit(f'self-test: no {rel}')
    pipeline._modules()
    import splat.segtypes  # noqa: F401  (imports the N64 types too: pygfxd's native library)
    import spimdisasm  # noqa: F401
    import rabbitizer  # noqa: F401
    import tkinter  # noqa: F401
    print('self-test: ok')


def main():
    # a windowed build (Windows) has no console: what splat and the tools print goes nowhere instead of failing
    for name in ('stdout', 'stderr'):
        if getattr(sys, name) is None:
            setattr(sys, name, open(os.devnull, 'w'))
    if sys.argv[1:] == ['--self-test']:
        return self_test()
    if len(sys.argv) >= 3 and sys.argv[1] == '--splat':  # (the builder runs splat through itself)
        from .relocs import run_splat
        return run_splat(sys.argv[2:])
    if len(sys.argv) == 1:
        from .gui import main as gui
        return gui()
    from . import __version__, pipeline
    from .discs import DiscError
    ap = argparse.ArgumentParser(prog='ff9-3ds-builder', description=__doc__.split('\n')[0])
    ap.add_argument('discs', nargs='+')
    ap.add_argument('--out', default='.')
    ap.add_argument('--no-3dsx', action='store_true')
    ap.add_argument('--no-cia', action='store_true')
    ap.add_argument('--version', action='version', version=__version__)
    a = ap.parse_args()
    state = {'pct': -1, 'inline': False}

    def report(f, msg=None):
        pct = int(f * 100)
        if msg:
            print(('\n' if state['inline'] else '') + f'[{pct:3d}%] {msg}', flush=True)
            state['inline'] = False
        elif pct != state['pct']:
            print(f'\r[{pct:3d}%]', end='', flush=True)
            state['inline'] = True
        state['pct'] = pct

    def log(m):
        print(('\n' if state['inline'] else '') + '       ' + m, flush=True)
        state['inline'] = False
    try:
        discs = pipeline.identify(a.discs)
        pipeline.build(discs, a.out, not a.no_3dsx, not a.no_cia, log=log, report=report)
    except DiscError as e:
        sys.exit(f'\nerror: {e}')
    print()


if __name__ == '__main__':
    main()
