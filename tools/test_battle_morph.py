#!/usr/bin/env python3
"""Morph projection equivalence with original MIPS and the shared production GTE."""
import ctypes
import importlib.util
from pathlib import Path
import random
import re
import struct
import subprocess

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('ribbon_oracle',ROOT/'tools/test_battle_ribbons.py')
r=importlib.util.module_from_spec(spec);spec.loader.exec_module(r)
rng=random.Random(0x800DF15C)
cases,expected=[],[]
def matrix(z):
    values=[(4096 if i%4==0 else 0)+rng.randrange(-400,401) for i in range(9)]
    return struct.pack('<9hH3i',*values,rng.getrandbits(16),rng.randrange(-100,101),rng.randrange(-100,101),z)
for kind,address in enumerate((0x800DF15C,0x800DDF10)):
    asm=ROOT/f'decomp/asm/us/ovl_0b/nonmatchings/ovl_0b_2/func_{address:08X}.s'
    image=b''.join(bytes.fromhex(w) for w in re.findall(r'/\* [0-9A-F]+ [0-9A-F]{8} ([0-9A-F]{8}) \*/',asm.read_text()))
    for trial in range(300):
        data=bytearray(rng.randbytes(8192));parts=trial%5
        index=(trial%3)|(rng.getrandbits(24)<<8);offset=(index&255)*40
        fields={8:0x200,32:0x700,92:0x300,0x11c:0x180,0x210:0x400,0x310:0x500,
                0x400+offset+20:0x600,0x400+offset+24:0x1000,0x500+offset+24:0x1400}
        for off,pointer in fields.items():struct.pack_into('<I',data,off,0x02180000+pointer)
        if kind:struct.pack_into('<h',data,90,rng.choice((-32768,-100,0,100,32767)))
        data[0x202]=parts;struct.pack_into('<H',data,0x18a,rng.randrange(64,1025))
        data[0x198:0x1b8]=matrix(rng.randrange(256,1025))
        counts=[rng.randrange(10) for _ in range(parts)]
        for i,count in enumerate(counts):
            struct.pack_into('<H',data,0x600+i*2,count)
            data[0x700+i*32:0x720+i*32]=matrix(rng.randrange(-400,401))
        for i in range(sum(counts)+1):
            for base in (0x1000,0x1400):
                struct.pack_into('<4h',data,base+i*8,*(rng.randrange(-2000,2001) for _ in range(4)))
        factor=rng.choice([0,1,4096,8192,0xffffffff,rng.getrandbits(32)])
        xy,z=rng.randbytes(512),rng.randbytes(512)
        regs=[rng.getrandbits(32) for _ in range(64)];regs[56:58]=[160<<16,112<<16]
        state=ctypes.create_string_buffer(struct.pack('<64I',*regs),256)
        cases.append(struct.pack('<3I',kind,index,factor)+data+xy+z+state.raw)
        original=bytearray(data)
        for off,pointer in fields.items():struct.pack_into('<I',original,off,0x80180000+pointer)
        mem=r.e.Mem(trial,[(address,image),(0x80180000,original),(0x8006794c,struct.pack('<I',0x80180100)),
                           (0x800711a8,xy),(0x80070520,z)])
        cpu=r.Cpu(mem,address,address+len(image),{}, {},trial,[0x80180000,index,factor,0])
        r.lib.oracle_set(state);cpu.g=r.Registers();cpu.run(address)
        assert not cpu.events and not cpu.calls,(trial,cpu.events,cpu.calls)
        r.lib.oracle_get(state)
        expected.append(bytes(mem.read(0x800711a8+i,1) for i in range(512))+
                        bytes(mem.read(0x80070520+i,1) for i in range(512))+state.raw)
result=subprocess.run([ROOT/'build/tests/battle_morph_probe'],input=b''.join(cases),capture_output=True)
assert result.returncode==0,result.stderr.decode()
size=len(expected[0]);assert len(result.stdout)==len(expected)*size
def observable(data):
    data=bytearray(data)
    # The original loads uninitialized stack padding into C4's upper half.
    # CFC2 sign-extends its 16-bit matrix element; commands ignore those bits.
    offset=1024+36*4
    value=struct.unpack_from('<h',data,offset)[0]
    struct.pack_into('<i',data,offset,value)
    return data
for trial,want in enumerate(expected):
    got=result.stdout[trial*size:(trial+1)*size]
    if trial<300:got=observable(got);want=observable(want)
    assert got==want,(trial,next(i for i,(a,b) in enumerate(zip(got,want)) if a!=b))
print('Morph/plane-clipped projection match original MIPS in 600 cases (production GTE, vertex tails/depth tags; only morph C4 padding normalized)')
