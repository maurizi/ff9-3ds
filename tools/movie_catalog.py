#!/usr/bin/env python3
"""Inventory and validate all ISO9660 STR/MBG extents with the C frame assembler.

Reads the user's raw 2352-byte disc image without extracting entire movies.
The JSON contains only names/extents/measurements, not game payloads.
"""
import argparse
import json
from pathlib import Path
import struct
import subprocess

ROOT=Path(__file__).resolve().parents[1]
def le32(data,offset):
    return struct.unpack_from('<I',data,offset)[0]

def movie_extents(path):
    with path.open('rb') as disc:
        disc.seek(0,2)
        disc_bytes=disc.tell()
        def read(lba,size):
            if lba<0 or size<0 or (lba+(size+2047)//2048)*2352>disc_bytes:
                raise ValueError('ISO extent outside raw disc')
            result=bytearray()
            for offset in range((size+2047)//2048):
                disc.seek((lba+offset)*2352+24)
                result.extend(disc.read(2048))
            return result[:size]
        header=read(16,2048)
        if header[:7]!=b'\x01CD001\x01':
            raise ValueError('not a raw Mode 2 ISO9660 disc')
        visited=set()
        def walk(lba,size,prefix):
            if lba in visited:
                raise ValueError('repeated ISO directory extent')
            visited.add(lba)
            if size>1024*1024:
                raise ValueError('unexpectedly large ISO directory')
            directory=read(lba,size)
            offset=0
            while offset<len(directory):
                length=directory[offset]
                if not length:
                    offset=(offset//2048+1)*2048
                    continue
                entry=directory[offset:offset+length]
                offset+=length
                if len(entry)<34 or 33+entry[32]>len(entry):
                    raise ValueError('truncated ISO directory entry')
                name=entry[33:33+entry[32]].decode('ascii')
                if name in ('\0','\1'):
                    continue
                position,bytes_=le32(entry,2),le32(entry,10)
                if entry[25]&2:
                    yield from walk(position,bytes_,prefix+name+'/')
                elif name.endswith(('.STR;1','.MBG;1')):
                    if bytes_%2048 or not bytes_:
                        raise ValueError('movie extent is not whole ISO sectors')
                    sectors=bytes_//2048
                    if sectors%10 or (position+sectors)*2352>disc_bytes:
                        raise ValueError('movie extent is not complete ten-sector frames')
                    yield dict(name=prefix+name,lba=position,sectors=sectors)
        yield from walk(le32(header,158),le32(header,166),'')

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('image',type=Path)
    p.add_argument('--disc',type=int,choices=range(1,5),required=True)
    p.add_argument('--probe',type=Path,default=ROOT/'build/tests/movie_stream_probe')
    p.add_argument('--out',type=Path,required=True)
    p.add_argument('--vlc-table',type=Path,help="also expand every frame using the user's 216-byte FF9 AC table")
    a=p.parse_args()
    out=a.out.resolve()
    if not out.is_relative_to(ROOT/'build') or out.exists():
        p.error('output must be a new path under build/')
    rows=[]
    for entry in movie_extents(a.image):
        command=[str(a.probe.resolve()),str(a.image.resolve()),str(entry['lba']),str(entry['sectors'])]
        if a.vlc_table:command+=['--vlc-table',str(a.vlc_table.resolve())]
        result=subprocess.run(command,capture_output=True,text=True)
        if result.returncode:
            raise RuntimeError(f"{entry['name']}: {result.stderr.strip()} (exit {result.returncode})")
        stats=json.loads(result.stdout)
        if stats['frames']*10+stats['padding_sectors']!=entry['sectors']:
            raise RuntimeError(f"{entry['name']}: incomplete movie scan")
        if a.vlc_table and stats['vlc_frames']!=stats['frames']:
            raise RuntimeError(f"{entry['name']}: incomplete VLC scan")
        rows.append(dict(disc=a.disc,**entry,**stats))
        print(f"disc {a.disc} {entry['name']}: {stats['frames']} frames",flush=True)
    if not rows:
        raise RuntimeError('disc has no movie extents')
    out.parent.mkdir(parents=True,exist_ok=True)
    out.write_text(json.dumps(rows,indent=2)+'\n')
    print(f'{len(rows)} movies validated; {out}')

if __name__=='__main__':
    main()
