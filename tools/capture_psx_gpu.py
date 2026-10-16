#!/usr/bin/env python3
"""Record a paused original-game scene as GPUDUMP plus an emulated-time video.

Requires an existing PCSX-Redux instance started by ff9-decomp/tools/emu.py.
Outputs contain game data and must remain under this project's ignored build/.
The GPU dump starts on the next VSync; its VSync 0 is the restored preamble.
"""
import argparse
import importlib.util
import json
from pathlib import Path
import re
import subprocess
import sys

ROOT=Path(__file__).resolve().parents[1]

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--out',type=Path,required=True,help='new capture directory under build/')
    p.add_argument('--seconds',type=float,default=12)
    p.add_argument('--state',help='optional named state on the currently mounted disc')
    p.add_argument('--emu-tool',type=Path,default=ROOT.parent/'ff9-decomp/tools/emu.py')
    a=p.parse_args()
    output=a.out.resolve()
    if not output.is_relative_to(ROOT/'build') or output.exists():p.error('choose a new directory under build/')
    if not 0<a.seconds<=30:p.error('seconds must be between 0 and 30')
    if a.state and not re.fullmatch('[A-Za-z0-9_-]+',a.state):p.error('invalid state name')
    spec=importlib.util.spec_from_file_location('ff9_emu',a.emu_tool)
    emu=importlib.util.module_from_spec(spec);spec.loader.exec_module(emu)
    if emu.is_running():p.error('pause the reference emulator before capture')
    if a.state:
        if emu.bridge('status')['pc']>=0xbfc00000:emu.run_frames(1800)
        emu.http('/state/load?name='+a.state)
        emu.http('/execution-flow?function=pause','POST')
        emu.run_frames(2)
    output.mkdir(parents=True)
    (output/'before.png').write_bytes(emu.http('/screen/still'))
    before=emu.bridge('status')
    path=str(output/'trace.gpd')
    emu.lua('PCSX.GPU.startDump('+json.dumps(path)+');return true')
    try:
        subprocess.run([sys.executable,str(ROOT/'tools/psx_clip.py'),'--seconds',str(a.seconds),
                        '--out',str(output/'reference.mp4'),'--emu-tool',str(a.emu_tool)],check=True)
    finally:
        emu.lua('PCSX.GPU.stopDump();return true')
    dump=Path(path)
    if not dump.exists() or dump.stat().st_size<=16:
        raise RuntimeError('GPU capture did not arm or produced an empty trace')
    with dump.open('rb') as stream:
        if stream.read(16)!=b'PSXGPUDUMPv1r1\0\0':raise RuntimeError('unexpected GPU dump format')
    after=emu.bridge('status')
    (output/'after.png').write_bytes(emu.http('/screen/still'))
    (output/'capture.json').write_text(json.dumps(dict(state=a.state,seconds=a.seconds,
        time_base='emulated NTSC PSX VBlanks',gpu_dump_bytes=dump.stat().st_size,
        before=before,after=after),indent=2)+'\n')
    print(output)

if __name__=='__main__':main()
