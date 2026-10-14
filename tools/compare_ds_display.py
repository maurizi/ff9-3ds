#!/usr/bin/env python3
"""Compare software-display pixels from two melonDS 1.1 states and matching ELFs.

Reports both the displayed PSX RGB15 source and the active DS bitmap. Frame
numbers and viewport sizes are retained so different animation phases are not
mistaken for renderer regressions. No image files or game data are committed.
"""
import argparse
import json
from pathlib import Path
import struct
import subprocess

def display(state,elf):
    data=state.read_bytes()
    if len(data)<32 or data[:4]!=b'MELN' or struct.unpack_from('<I',data,8)[0]!=len(data):
        raise ValueError('expected a complete melonDS state')
    sections={};pos=16
    while pos<len(data):
        if pos+16>len(data):raise ValueError('truncated state section')
        size=struct.unpack_from('<I',data,pos+4)[0]
        if size<16 or pos+size>len(data):raise ValueError('invalid state section size')
        sections[data[pos:pos+4]]=data[pos:pos+size];pos+=size
    symbols={line.split()[2]:int(line.split()[0],16) for line in
             subprocess.check_output(['nm','-a',str(elf)],text=True).splitlines() if len(line.split())==3}
    if 'back_buffer' not in symbols:raise ValueError('ELF must use the software display')
    ram=sections[b'NDSG'][20:20+0x1000000]
    def read(address,count):
        offset=address-0x02000000
        if offset<0 or offset+count>len(ram):raise ValueError('address outside main RAM')
        return ram[offset:offset+count]
    x,y,width,height=struct.unpack('<4h',read(symbols['disp_env'],8))
    width=min(width or 320,320);height=min(height or 224,240)
    if width<=0 or height<=0:raise ValueError('invalid viewport')
    pointer=struct.unpack('<I',read(symbols['psx_vram'],4))[0]
    vram=struct.unpack('<524288H',read(pointer,1024*512*2))
    source=[vram[((y+j)&511)*1024+((x+i)&1023)]&32767 for j in range(height) for i in range(width)]
    bgcnt=struct.unpack_from('<H',sections[b'GP2A'],26)[0]
    bank=((bgcnt>>8)&31)//8
    if bank not in (0,1):raise ValueError('expected the A/B software bitmap banks')
    start=4128+bank*131072
    if start+131072>len(sections[b'GPUG']):raise ValueError('truncated GPU VRAM banks')
    bitmap=struct.unpack_from('<65536H',sections[b'GPUG'],start)
    active=[bitmap[j*256+i] for j in range(height*4//5) for i in range(width*4//5)]
    frame=struct.unpack('<I',read(symbols['frames'],4))[0]
    return {'state':str(state),'elf':str(elf),'frame':frame,'viewport':[x,y,width,height],
            'active_bank':bank},source,active

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('baseline',type=Path);p.add_argument('candidate',type=Path)
    p.add_argument('--baseline-elf',required=True,type=Path)
    p.add_argument('--candidate-elf',required=True,type=Path)
    a=p.parse_args()
    try:
        old,old_source,old_bitmap=display(a.baseline,a.baseline_elf)
        new,new_source,new_bitmap=display(a.candidate,a.candidate_elf)
        if old['viewport'][2:]!=new['viewport'][2:]:raise ValueError('viewport dimensions differ')
    except (ValueError,KeyError,struct.error) as error:p.error(str(error))
    print(json.dumps({'baseline':old,'candidate':new,'same_frame':old['frame']==new['frame'],
        'source_rgb15_different_pixels':sum(x!=y for x,y in zip(old_source,new_source)),
        'active_bitmap_different_pixels':sum(x!=y for x,y in zip(old_bitmap,new_bitmap))},indent=2))

if __name__=='__main__':main()
