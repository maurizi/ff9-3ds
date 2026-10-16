#!/usr/bin/env python3
"""Compare GPU colour conversion with a PSX emulator using synthetic GP0 draws.

Build build/tests/raster_psx_probe first. Redux saves/restores its full state.
DuckStation requires a dedicated software-renderer instance with GDB enabled:
its CPU/RAM are restored, but the synthetic GPU state remains until reset.
Constant-colour interiors isolate quantization, modulation and blending from
polygon edge/interpolation differences. Reports stay under ignored build/.
"""
import argparse
import importlib.util
import json
from pathlib import Path
import struct
import subprocess
import time
from duckstation_gpu import DuckStation

ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--out',type=Path,required=True)
p.add_argument('--probe',type=Path,default=ROOT/'build/tests/raster_psx_probe')
p.add_argument('--backend',choices=('redux','duckstation'),default='duckstation')
p.add_argument('--port',type=int,default=19000,help='DuckStation GDB port')
a=p.parse_args()
if not a.out.resolve().is_relative_to(ROOT/'build') or a.out.exists():
    p.error('output must be a new path under build/')
if a.backend=='redux':
    spec=importlib.util.spec_from_file_location('ff9_emu',ROOT/'decomp/tools/emu.py')
    emu=importlib.util.module_from_spec(spec);spec.loader.exec_module(emu)
    if emu.is_running():p.error('pause the reference emulator first')
def words(values):return struct.pack('<'+'I'*len(values),*values)
def xy(x,y):return x|(y<<16)
vram=bytearray(1024*512*2);uploads=[]
def upload(x,y,w,h,value):
    data=struct.pack('<H',value)*(w*h)
    for row in range(h):vram[((y+row)*1024+x)*2:((y+row)*1024+x+w)*2]=data[row*w*2:(row+1)*w*2]
    uploads.extend([0xa0000000,xy(x,y),xy(w,h),*struct.unpack('<'+'I'*(len(data)//4),data)])
upload(640,256,8,32,0x1111)
upload(704,256,16,32,0x0101)
upload(768,256,32,32,0x9acb)
upload(0,480,256,1,0x9acb)
cases=[]
for kind in range(5):
    # flat/gouraud quads, rectangles, flat/gouraud horizontal lines
    for depth in [-1,0,1,2] if kind<3 else [-1]:
        for raw in ([0,1] if depth>=0 else [0]):
            for dither in [0,1]:
                for semi in [-1,0,1,2,3]:
                    colour=0x817f09
                    tp=(10+depth if depth>=0 else 0)|16|((max(depth,0))<<7)|((max(semi,0))<<5)|(dither<<9)|0x400
                    packet=[0xe1000000|tp,0xe2000000,0xe3000000,0xe407ffff,0xe5000000,0xe6000000,
                            0x02282828,0,xy(32,32)]
                    textured=depth>=0
                    if kind<2:
                        cmd=0x28|(kind<<4)|(textured<<2)|((semi>=0)<<1)|raw
                        for i,(x,y) in enumerate([(0,0),(32,0),(0,32),(32,32)]):
                            if i==0:packet.append((cmd<<24)|colour)
                            elif kind==1:packet.append(colour)
                            packet.append(xy(x,y))
                            if textured:packet.append(xy(x,y)|(((480<<6) if i==0 else (tp&0x9ff) if i==1 else 0)<<16))
                    elif kind==2:
                        packet.extend([((0x60|(textured<<2)|((semi>=0)<<1)|raw)<<24)|colour,0])
                        if textured:packet.append((480<<6)<<16)
                        packet.append(xy(32,32))
                    else:
                        for y in range(4,8):
                            packet.extend([((0x40|((kind==4)<<4)|((semi>=0)<<1))<<24)|colour,xy(0,y)])
                            if kind==4:packet.append(colour)
                            packet.append(xy(31,y))
                    cases.append(dict(kind=kind,depth=depth,raw=raw,dither=dither,semi=semi,packet=packet))
for case in list(cases):
    if case['semi']!=-1 or case['raw'] or case['kind']>=2:continue
    for colour in [0,0x070707,0x080808,0x7f7f7f,0x808080,0xffffff]:
        extra=dict(case)
        extra.update(colour=colour,packet=[(word&0xff000000)|colour if (word&0xffffff)==0x817f09 else word for word in case['packet']])
        cases.append(extra)

# Copy commands to GP0. Snapshot a 4x4 VRAM interior through the renderer API
# after execution; this avoids conflating GPU read-transfer quirks with drawing.
code=[0x3c091f80,0x35291810,0xad200004, # GPU reset (VRAM retained)
      0x8c880000,0x24840004,0xad280000,0x1485fffc,0,0]
start=0x801fee00;end=start+8*4
saved='raster_colour_'+time.strftime('%Y%m%d_%H%M%S')
if a.backend=='redux':emu.http('/state/save?name='+saved)
else:duck=DuckStation(a.port)
expected=[]
try:
    for index,case in enumerate(cases):
        packet=(uploads if index==0 else [])+case['packet']
        if a.backend=='duckstation':
            expected.append(duck.draw(packet))
            if index%100==0:print(f'DuckStation GPU cases {index+1}/{len(cases)}',flush=True)
            continue
        payload=words(packet)
        emu.bridge('write',0x801f8000,payload.hex())
        emu.bridge('write',start,words(code).hex())
        emu.lua(f'''local r=PCSX.getRegisters();r.pc={start};r.GPR.n.a0=0x801f8000;
        r.GPR.n.a1={0x801f8000+len(payload)};
        r.CP0.r[12]=0;return true''')
        bp=emu.bridge('addBp',end,'Exec',4,'break',1,'synthetic GPU colour probe')
        try:
            emu.run_frames(1)
            if emu.bridge('regs')['pc']!=end:raise RuntimeError('GPU probe did not finish')
            pixels=emu.lua('''local v=tostring(PCSX.GPU.getVRAM());local out={};
            for y=4,7 do out[#out+1]=v:sub((y*1024+4)*2+1,(y*1024+8)*2):gsub('.',function(c)
              return string.format('%02x',string.byte(c)) end) end;return table.concat(out)''')
            expected.append(bytes.fromhex(pixels))
        finally:
            emu.bridge('removeBp',bp);emu.lua('Bridge.pauseAt=nil;return true')
        if index%100==0:print(f'PSX GPU cases {index+1}/{len(cases)}',flush=True)
finally:
    if a.backend=='redux':emu.http('/state/load?name='+saved);emu.run_frames(0)
    else:duck.close()
result=subprocess.run([a.probe.resolve()],input=bytes(vram)+b''.join(words([len(c['packet'])]+c['packet']) for c in cases),capture_output=True)
if result.returncode:raise RuntimeError(result.stderr.decode())
if len(result.stdout)!=32*len(cases):raise RuntimeError('incomplete native GPU output')
if expected[0]!=result.stdout[:32]:raise RuntimeError('untextured flat control draw disagrees; inspect probe setup')
rows=[]
for i,(case,want) in enumerate(zip(cases,expected)):
    got=result.stdout[i*32:(i+1)*32]
    rows.append(dict(**case,psx=list(struct.unpack('<16H',want)),native=list(struct.unpack('<16H',got)),match=got==want))
a.out.parent.mkdir(parents=True,exist_ok=True);a.out.write_text(json.dumps(rows,indent=2)+'\n')
failures=[r for r in rows if not r['match']]
print(f"GPU colour interiors: {len(rows)-len(failures)}/{len(rows)} match; report {a.out}")
raise SystemExit(bool(failures))
