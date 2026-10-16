#!/usr/bin/env python3
"""Compare BS v2 expansion with FF9's original MIPS routine in paused Redux.

Runs beginning/middle/end frames from every ISO movie on the selected discs.
Saves/restores full emulator state. Inputs/reference coefficients remain in
ignored build/. No decoded pixels or sound are compared by this test.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import subprocess
import time
from movie_catalog import movie_extents

ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--raw-dir',type=Path,default=ROOT.parent/'ff9-decomp/disks/us/raw')
p.add_argument('--overlay',type=Path,default=ROOT/'decomp/disks/us/ovl/64.bin')
p.add_argument('--emu',type=Path,default=ROOT.parent/'ff9-decomp/tools/emu.py')
p.add_argument('--probe',type=Path,default=ROOT/'build/tests/movie_vlc_probe')
p.add_argument('--discs',type=int,nargs='+',choices=range(1,5),default=[1,2,3,4])
p.add_argument('--out',type=Path,required=True)
p.add_argument('--warmup-frames',type=int,default=0,
               help='run BIOS frames before injection on a fresh paused emulator; full state is restored')
a=p.parse_args()
if not 0<=a.warmup_frames<=600:p.error('warmup must be between 0 and 600 frames')
out=a.out.resolve()
if not out.is_relative_to(ROOT/'build') or out.exists():
    p.error('output must be a new directory under build/')
spec=importlib.util.spec_from_file_location('ff9_emu',a.emu)
emu=importlib.util.module_from_spec(spec);spec.loader.exec_module(emu)
if emu.is_running():p.error('reference emulator must be paused')
overlay=a.overlay.resolve()
table=overlay.read_bytes()[0x8019dcfc-0x80198700:0x8019dcfc-0x80198700+216]
if len(table)!=216:raise RuntimeError('truncated overlay table')
out.mkdir(parents=True)
cases=[]
for disc in a.discs:
    path=a.raw_dir/f'FF9 CD{disc}.img'
    for movie in movie_extents(path):
        with path.open('rb') as f:
            f.seek(movie['lba']*2352+24+0x60)
            total=struct.unpack('<I',f.read(4))[0]
            if not 0<total<=movie['sectors']//10:raise RuntimeError('invalid movie length')
            for number in sorted({1,(total+1)//2,total}):
                f.seek((movie['lba']+(number-1)*10)*2352)
                raw=f.read(23520)
                video=b''.join(raw[i*2352+56:i*2352+24+2324] for i in range(9,1,-1))
                size=struct.unpack_from('<I',raw,2*2352+24+12)[0]*4
                if not 8<=size<=len(video):raise RuntimeError('invalid compressed size')
                width,height=struct.unpack_from('<HH',raw,2*2352+24+16)
                cases.append(dict(disc=disc,movie=movie['name'],frame=number,
                                  width=width,height=height,bs=video[:size]))
def words(values):return struct.pack('<'+'I'*len(values),*values)
def jal(address):return 0x0c000000|((address>>2)&0x3ffffff)
start=0x80070000
code=[0x3c048008,jal(0x8019ae44),0,0x24040000,jal(0x8019ae98),0,
      0x3c048009,0x3c05800a,0x3c068008,jal(0x8019aec0),0]
end=start+len(code)*4
code += [0x1000ffff,0]
saved='movie_vlc_'+str(time.time_ns())
emu.http('/state/save?name='+saved)
rows=[]
try:
    if a.warmup_frames:emu.run_frames(a.warmup_frames)
    emu.lua(f'''local f=assert(io.open({json.dumps(str(overlay))},'rb'));
    local d=f:read('*a');f:close();require('ffi').copy(PCSX.getMemPtr()+0x198700,d,#d);
    PCSX.invalidateCache();return true''')
    emu.bridge('write',start,words(code).hex())
    for i,case in enumerate(cases):
        bs=case['bs'];path=out/f'input_{i:03}.bs';path.write_bytes(bs)
        emu.lua(f'''local f=assert(io.open({json.dumps(str(path))},'rb'));
        local d=f:read('*a');f:close();local ffi=require('ffi');
        ffi.copy(PCSX.getMemPtr()+0x90000,d,#d);ffi.fill(PCSX.getMemPtr()+0xa0000,0x10004,0xa5);
        local r=PCSX.getRegisters();r.pc={start};r.GPR.n.sp=0x801ff000;
        r.CP0.r[12]=0x40000000;PCSX.invalidateCache();return true''')
        bp=emu.bridge('addBp',end,'Exec',4,'break',1,'FF9 VLC return')
        try:
            emu.run_frames(30)
            regs=emu.bridge('regs')
            if regs['pc']!=end or regs['v0']!=0:
                raise RuntimeError(f'original VLC did not complete: {regs}')
            ram=emu.ram()
        finally:
            emu.bridge('removeBp',bp);emu.lua('Bridge.pauseAt=nil;return true')
        size=4+struct.unpack_from('<H',bs)[0]*4
        reference=ram[0xa0000:0xa0000+size]
        (out/f'psx_{i:03}.rl').write_bytes(reference)
        native=subprocess.run([str(a.probe.resolve())],
                              input=table+words([len(bs)])+bs,capture_output=True,check=True).stdout
        status,halfwords,blocks,bits=struct.unpack_from('<iIII',native)
        actual=native[16:]
        (out/f'port_{i:03}.rl').write_bytes(actual)
        expected_blocks=((case['width']+15)//16)*((case['height']+15)//16)*6
        guard=ram[0xa0000+size:0xa0000+size+4]
        match=(status==0 and halfwords*2==size and actual==reference and
               blocks==expected_blocks and guard==b'\xa5'*4)
        row={k:v for k,v in case.items() if k!='bs'}
        row.update(match=match,status=status,bytes=size,blocks=blocks,bits=bits,
                   reference_sha256=hashlib.sha256(reference).hexdigest(),
                   native_sha256=hashlib.sha256(actual).hexdigest(),
                   output_guard=guard.hex())
        rows.append(row)
        (out/'report.json').write_text(json.dumps(rows,indent=2)+'\n')
        print(f"{i+1}/{len(cases)} disc {row['disc']} {row['movie']} frame {row['frame']}: {match}",flush=True)
        if not match:raise RuntimeError('coefficient mismatch; inspect report and paired outputs')
finally:
    emu.http('/state/load?name='+saved);emu.run_frames(0)
print(f'{len(rows)} original FF9 coefficient streams match byte-for-byte')
