"""The web builder's entry points (builder/web/worker.js runs them in Pyodide, in a Web Worker): the same pipeline as
the window and the command line, on the browser's files.

The worker mounts the user's disc files at DISCS (Emscripten's WORKERFS: reads go to the File objects, nothing is
uploaded), keeps the large files (the disc pack, the outputs) on BIG (builder/web/bigfs.js: the browser's private
file system, OPFS) and everything else in memory."""
import builtins
import os

from . import pipeline
from .discs import Disc, DiscError

DISCS = '/discs'
BIG = '/big'
SCRATCH = '/tmp'

_open = builtins.open


def _big_buffers(path, mode='r', buffering=-1, *a, **k):
    """open() with large buffers for the disc files: WORKERFS reads a File slice per read call"""
    if buffering == -1 and 'b' in mode and isinstance(path, str) and path.startswith(DISCS + '/'):
        buffering = 4 << 20
    return _open(path, mode, buffering, *a, **k)


builtins.open = _big_buffers


def identify(names):
    """[{name, disc, boot} or {name, error}] for the files the user chose (cue/ccd sheets and images: an image a
    sheet names is reported with the sheet, not again on its own)"""
    out, used = [], set()
    paths = [os.path.join(DISCS, n) for n in names]
    sheets = [p for p in paths if p.lower().endswith(('.cue', '.ccd'))]
    for p in sheets + [p for p in paths if p not in sheets]:
        if p in used:
            continue
        try:
            d = Disc(p)
            used.add(os.path.normpath(d.path))
            out.append({'name': os.path.basename(p), 'disc': d.number, 'boot': d.boot})
            d.close()
        except (DiscError, OSError) as e:
            if os.path.normpath(p) not in used:
                out.append({'name': os.path.basename(p), 'error': str(e)})
    return out


def build(names, fmt, log, report):
    """build from the chosen files (one per disc: identify's names) into BIG; the output's path"""
    discs = pipeline.identify([os.path.join(DISCS, n) for n in names])
    outs = pipeline.build(discs, BIG, fmt == '3dsx', fmt == 'cia', log=log, report=report, scratch=SCRATCH)
    for d in discs.values():
        d.close()
    return outs[0]
