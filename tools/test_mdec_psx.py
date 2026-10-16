#!/usr/bin/env python3
"""Compare production macroblock pixels with a dedicated DuckStation's MDEC.

Requires interpreter/GDB port 19000, a booted BIOS (a parked synthetic EXE is
sufficient), and build/tests/mdec_probe. CPU, test RAM and MDEC/DMA state are
overwritten; use a dedicated emulator and close it after testing. Captures stay
under build/. This is a decoder comparison, not synchronized movie playback.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
from duckstation_gpu import DuckStation,words

ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--port',type=int,default=19000)
p.add_argument('--probe',type=Path,default=ROOT/'build/tests/mdec_probe')
p.add_argument('--overlay',type=Path,default=ROOT/'decomp/disks/us/ovl/64.bin')
p.add_argument('--out',type=Path,required=True)
p.add_argument('--limit',type=int,default=0,help='diagnostic case limit; zero runs the full suite')
p.add_argument('--movie-references',type=Path,nargs='+',default=[],
               help='passing VLC reference directories; sample each frame and reconstruct three full frames')
p.add_argument('--make-boot-exe',action='store_true',help='generate a disc-free parked PS-X EXE, then exit')
a=p.parse_args()
out=a.out.resolve()
if not out.is_relative_to(ROOT/'build') or out.exists():p.error('output must be a new directory under build/')
if a.make_boot_exe:
    out.mkdir(parents=True)
    image=bytearray(0x1000);image[:8]=b'PS-X EXE'
    for offset,value in [(0x10,0x80010000),(0x18,0x80010000),(0x1c,0x800),
                         (0x30,0x801fff00),(0x800,0x1000ffff)]:
        struct.pack_into('<I',image,offset,value)
    (out/'park.exe').write_bytes(image)
    print(f'Boot {out}/park.exe in the dedicated reference emulator, then run the suite.')
    raise SystemExit(0)
overlay=a.overlay.read_bytes()
quant=overlay[0x8019dec8-0x80198700:0x8019dec8-0x80198700+128]
scale=overlay[0x8019df4c-0x80198700:0x8019df4c-0x80198700+128]
if len(quant)!=128 or len(scale)!=128:raise RuntimeError('truncated MDEC tables')
cases=[]
def add(name,rl):
    for command in [0x30000000,0x34000000,0x38000000,0x3a000000,0x3c000000,0x3e000000]:
        cases.append(dict(name=name,rl=rl,command=command))
def dc_blocks(values,q=1):
    return [w for value in values for w in [(q<<10)|(value&1023),0xfe00]]
add('neutral',dc_blocks([0]*6))
add('quadrants',dc_blocks([0,0,32,64,-32,-64]))
for value in [-512,-128,-1,1,127,511]:
    # q=63/DC=-512 is FE00 itself, reserved as padding rather than a DC.
    add(f'dc_{value}',dc_blocks([value]*6,62 if value==-512 else 63))
# Every AC rank in every block; rotate signs, levels and q scales.
for which in range(6):
    for rank in range(1,64):
        rl=[]
        for block in range(6):
            rl.append(((0 if rank%4==0 else 63 if rank%4==1 else 1)<<10)|((block*17)&1023))
            if block==which:
                level=[1,-1,511,-512,31,-30][rank%6]
                rl.append(((rank-1)<<10)|(level&1023))
            rl.append(0xfe00)
        # Two output depths cover every impulse; mode flags covered above.
        for command in [0x30000000,0x38000000]:
            cases.append(dict(name=f'impulse_b{which}_k{rank}',rl=rl,command=command))
full_images={}
if a.movie_references:
    frames=[]
    for directory in a.movie_references:
        report=json.loads((directory/'report.json').read_text())
        for index,row in enumerate(report):
            if not row['match']:raise RuntimeError('movie coefficient reference must have passed')
            frames.append((directory,index,row))
    chosen={max(range(len(frames)),key=lambda i:frames[i][2]['bytes']),
            max(range(len(frames)),key=lambda i:frames[i][2]['bits'])}
    mbg=[i for i,(_,_,row) in enumerate(frames) if '.MBG' in row['movie'] and row['frame']>1]
    if mbg:chosen.add(mbg[len(mbg)//2])
    for frame_index,(directory,index,row) in enumerate(frames):
        data=(directory/f'psx_{index:03}.rl').read_bytes()
        if hashlib.sha256(data).hexdigest()!=row['reference_sha256']:raise RuntimeError('changed coefficient reference')
        codes=struct.unpack('<'+'H'*(len(data)//2),data)
        blocks=[];at=2
        count=((row.get('width',320)+15)//16)*((row.get('height',224)+15)//16)
        for macro in range(count):
            begin=at
            for block in range(6):
                while codes[at]==0xfe00:at+=1
                at+=1;rank=0
                while rank<63:
                    rank+=(codes[at]>>10)+1;at+=1
            blocks.append(list(codes[begin:at]))
        selected=range(count) if frame_index in chosen else sorted({0,count//2,count-1})
        label=f'frame_{frame_index:03}'
        if frame_index in chosen:
            full_images[label]={**row,'width':row.get('width',320),'height':row.get('height',224)}
        for macro in selected:
            for command in [0x30000000,0x38000000]:
                cases.append(dict(name=f'disc{row["disc"]}_{row["movie"]}_{row["frame"]}_mb{macro}',
                                  rl=blocks[macro],command=command,image=label if frame_index in chosen else None,
                                  macroblock=macro))
if a.limit:cases=cases[:a.limit]
out.mkdir(parents=True)
payload=quant+scale+b''.join(words([c['command'],len(c['rl'])])+struct.pack('<'+'H'*len(c['rl']),*c['rl']) for c in cases)
native=subprocess.run([str(a.probe.resolve())],input=payload,capture_output=True,check=True).stdout
position=0;rows=[]
images={}
duck=DuckStation(a.port)
try:
    for index,case in enumerate(cases):
        status,consumed=struct.unpack_from('<iI',native,position);position+=8
        if status:raise RuntimeError(f'native decoder returned {status}')
        size=768 if ((case['command']>>27)&3)==2 else 512
        actual=native[position:position+size];position+=size
        rl=case['rl']+([0xfe00] if len(case['rl'])%2 else [])
        command=case['command']|len(rl)//2
        writes=[(0x1f801824,0x80000000),(0x1f801824,0x60000000),(0x1f801820,0x40000001)]
        writes += [(0x1f801820,w) for w in struct.unpack('<32I',quant)]
        writes += [(0x1f801820,0x60000000)]+[(0x1f801820,w) for w in struct.unpack('<32I',scale)]
        writes += [(0x1f801820,command)]+[(0x1f801820,w) for w in struct.unpack('<'+'I'*(len(rl)//2),struct.pack('<'+'H'*len(rl),*rl))]
        writes += [(0x1f8010f0,0x88),(0x1f801090,0x1f0000),
                   (0x1f801094,((size//4//32)<<16)|32),(0x1f801098,0x01000200)]
        data=words([w for pair in writes for w in pair])
        duck.write(0x801f8000,data)
        duck.write(0x801f0000,b'\xa5'*(size+4))
        # Generic MMIO write loop, then wait for DMA1 output completion.
        code=[0x8c880000,0x8c890004,0x24840008,0xad090000,0x1485fffb,0,
              0x3c081f80,0x3c0a0100,0x8d091098,0,0x012a4824,0x1520fffc,0]
        duck.execute(code,0x801f8000,0x801f8000+len(data))
        reference=duck.read(0x801f0000,size)
        guard=duck.read(0x801f0000+size,4)
        (out/f'psx_{index:04}.bin').write_bytes(reference)
        (out/f'port_{index:04}.bin').write_bytes(actual)
        differences=sum(x!=y for x,y in zip(actual,reference))
        row=dict(**case,consumed=consumed,bytes=size,different_bytes=differences,
                 match=len(actual)==size and not differences and guard==b'\xa5'*4,
                 guard=guard.hex(),reference_sha256=hashlib.sha256(reference).hexdigest(),
                 native_sha256=hashlib.sha256(actual).hexdigest())
        rows.append(row)
        (out/'report.json').write_text(json.dumps(rows,indent=2)+'\n')
        if not row['match']:
            print(row,flush=True)
            raise RuntimeError('MDEC pixels differ; inspect paired outputs')
        label=case.get('image')
        if label and case['command']==0x30000000:
            width,height=full_images[label]['width'],full_images[label]['height']
            for source,data_ in [('psx',reference),('port',actual)]:
                bitmap=images.setdefault((label,source),bytearray(width*height*3))
                column,macro_y=divmod(case['macroblock'],(height+15)//16)
                for y in range(16):
                    line=(macro_y*16+y)*width*3+column*16*3
                    bitmap[line:line+48]=data_[y*48:(y+1)*48]
        if index%50==0:print(f'MDEC cases {index+1}/{len(cases)} match',flush=True)
finally:
    duck.close()
if position!=len(native):raise RuntimeError('unexpected trailing native output')
if images:
    from PIL import Image
    for (label,source),bitmap in images.items():
        width,height=full_images[label]['width'],full_images[label]['height']
        Image.frombytes('RGB',(width,height),bytes(bitmap)).save(out/f'{label}_{source}.png')
    (out/'full_frames.json').write_text(json.dumps(full_images,indent=2)+'\n')
print(f'{len(rows)} macroblocks match PSX MDEC DMA output byte-for-byte')
