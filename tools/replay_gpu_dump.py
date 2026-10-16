#!/usr/bin/env python3
"""Compare a captured PSX GPUDUMP frame with the production software renderer.

Requires build/tests/gpu_replay and a DEDICATED DuckStation software-renderer
instance with GDB enabled. This destroys its running game's RAM/GPU state.
The dump and all reports must stay under ignored build/: they contain game data.
Frame 0 is the preamble; frame N is immediately after N recorded VSyncs.
Non-interlaced VRAM transfers, including partial read batches, are supported.
GPU information-query reads and reads past a transfer end are rejected.
GP0 packets may divide commands, including image uploads, at arbitrary words.
Specification: https://github.com/ps1dev/standards/blob/main/GPUDUMP.md
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
from duckstation_gpu import DuckStation, words

ROOT=Path(__file__).resolve().parents[1]
MAGIC=b'PSXGPUDUMPv1r1\0\0'

def command_length(w):
    cmd=w[0]>>24
    if 0x20<=cmd<0x40:
        nv=4 if cmd&8 else 3
        return 1+nv*(1+bool(cmd&4))+(nv-1 if cmd&16 else 0)
    if 0x40<=cmd<0x60:
        if cmd&8:
            # Terminators are checked at vertex/color boundaries after two
            # vertices, never at arbitrary parameter words.
            first,step=(4,2) if cmd&16 else (3,1)
            for i in range(first,len(w),step):
                if w[i]&0xf000f000==0x50005000:return i+1
            return None
        return 4 if cmd&16 else 3
    if 0x60<=cmd<0x80:return 2+bool(cmd&4)+not_fixed_size(cmd)
    if cmd==2:return 3
    if 0x80<=cmd<0xa0:return 4
    if 0xa0<=cmd<0xc0:
        if len(w)<3:return None
        width=((w[2]-1)&1023)+1;height=(((w[2]>>16)-1)&511)+1
        return 3+(width*height+1)//2
    if 0xc0<=cmd<0xe0:return 3
    return 1

def not_fixed_size(cmd):return int((cmd>>3)&3==0)

def normalize(data,frame):
    if data[:16]!=MAGIC:raise ValueError('unsupported GPUDUMP magic')
    records=[];writes=[];pending=[];position=16;begun=False;vsyncs=0
    display_origin=0;display_mode=0;cancelled=0;remaining_reads=0;read_words=0;read_commands=0
    while position<len(data):
        if position+4>len(data):raise ValueError('truncated packet header')
        h,=struct.unpack_from('<I',data,position);position+=4
        kind,n=h>>24,h&0xffffff
        end=position+4*n
        if end>len(data):raise ValueError('truncated packet payload')
        payload=list(struct.unpack_from('<'+'I'*n,data,position));position=end
        if kind==0:
            writes.extend((0,w) for w in payload);pending.extend(payload)
            offset=0
            while offset<len(pending):
                size=command_length(pending[offset:])
                if size is None or size>len(pending)-offset:break
                command=pending[offset:offset+size]
                if 0xc0<=command[0]>>24<0xe0:
                    width=((command[2]-1)&1023)+1;height=(((command[2]>>16)-1)&511)+1
                    remaining_reads=(width*height+1)//2;read_commands+=1
                records.append((0,command));offset+=size
            pending=pending[offset:]
        elif kind==1:
            for word in payload:
                cmd=word>>24
                if cmd in (0,1):
                    cancelled+=len(pending);pending=[];remaining_reads=0
                if 0x10<=cmd<0x20:raise ValueError('GPU information queries not supported')
                if cmd==0:display_origin=display_mode=0
                if cmd==5:display_origin=word&0x7ffff
                if cmd==8:
                    display_mode=word&255
                    if display_mode&0x20:raise ValueError('interlaced GPU dumps not supported')
            records.append((1,payload));writes.extend((1,w) for w in payload)
        elif kind==2:
            if not begun or n>2:raise ValueError('invalid VSync packet')
            vsyncs+=1
        elif kind in (3,4):
            # Both packets consume GPUREAD words; a normal dump player can
            # discard type 3's result. Compare those words here as well.
            if n!=1 or not 0<=payload[0]<=remaining_reads:
                raise ValueError('read count exceeds active VRAM transfer')
            remaining_reads-=payload[0];read_words+=payload[0]
            records.append((kind,payload));writes.append((2,payload[0]))
        elif kind==5:
            if begun or n or pending:raise ValueError('invalid trace begin')
            begun=True
        # Metadata and unknown extension types are skipped by packet size.
        if begun and vsyncs==frame:
            if pending:raise ValueError('selected VSync splits a GP0 command; choose a later frame')
            if remaining_reads:raise ValueError('selected VSync has an unfinished VRAM read; choose a later frame')
            return records,writes,dict(frame=frame,display_origin=[display_origin&1023,display_origin>>10],
                display_mode=display_mode,cancelled_words=cancelled,read_words=read_words,read_commands=read_commands)
    raise ValueError('requested frame is beyond captured trace')

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('dump',type=Path);p.add_argument('--frame',type=int,required=True)
    p.add_argument('--out',type=Path,required=True)
    p.add_argument('--port',type=int,default=19000)
    p.add_argument('--reference',type=Path,help='reuse a previous comparison directory with identical captured commands')
    p.add_argument('--probe',type=Path,default=ROOT/'build/tests/gpu_replay')
    a=p.parse_args()
    if a.frame<0:p.error('frame must be nonnegative')
    if not a.dump.resolve().is_relative_to(ROOT/'build'):p.error('dump must stay under build/')
    if not a.out.resolve().is_relative_to(ROOT/'build') or a.out.exists():p.error('choose a new output directory under build/')
    try:records,writes,metadata=normalize(a.dump.read_bytes(),a.frame)
    except ValueError as error:p.error(str(error))
    stream=b''.join(words([kind,len(payload)]+payload) for kind,payload in records)+words([2,0])
    digest=hashlib.sha256(stream).hexdigest()
    metadata['command_sha256']=digest
    output=subprocess.run([a.probe.resolve()],input=stream,capture_output=True,check=True).stdout
    read_bytes=metadata['read_words']*4
    if len(output)!=1048576+read_bytes:raise RuntimeError('incomplete native VRAM/readback output')
    native_reads,native=output[:read_bytes],output[read_bytes:]
    if a.reference:
        saved=json.loads((a.reference/'comparison.json').read_text())
        if saved.get('command_sha256')!=digest:raise ValueError('reference captured commands differ')
        reference=(a.reference/'psx.vram').read_bytes()
        reference_reads=(a.reference/'psx.reads').read_bytes() if read_bytes else b''
    else:
        print(f'Replaying {sum(port<2 for port,_ in writes)} register writes at VSync {a.frame}',flush=True)
        duck=DuckStation(a.port)
        try:
            reference_reads=duck.replay_writes(writes)
            reference=duck.read_vram()
        finally:duck.close()
    if len(reference)!=1048576:raise RuntimeError('incomplete reference VRAM snapshot')
    if len(reference_reads)!=read_bytes:raise RuntimeError('incomplete reference readback output')
    a.out.mkdir(parents=True)
    (a.out/'native.vram').write_bytes(native);(a.out/'psx.vram').write_bytes(reference)
    (a.out/'native.reads').write_bytes(native_reads);(a.out/'psx.reads').write_bytes(reference_reads)
    want=struct.unpack('<524288H',reference);got=struct.unpack('<524288H',native)
    metadata.update(dump=str(a.dump),commands=len(records),register_writes=sum(port<2 for port,_ in writes),
        different_pixels=sum(x!=y for x,y in zip(want,got)),
        different_rgb15_pixels=sum((x&32767)!=(y&32767) for x,y in zip(want,got)),
        different_read_words=sum(native_reads[i:i+4]!=reference_reads[i:i+4] for i in range(0,read_bytes,4)))
    (a.out/'comparison.json').write_text(json.dumps(metadata,indent=2)+'\n')
    print(json.dumps(metadata,indent=2))
    return bool(metadata['different_pixels'] or metadata['different_read_words'])

if __name__=='__main__':raise SystemExit(main())
