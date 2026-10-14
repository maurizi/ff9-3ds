#!/usr/bin/env python3
"""Window setup equivalence: complete record including wraparound coordinates."""
import importlib.util
from pathlib import Path
import random
import re
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('ff9_equiv', ROOT/'decomp/tools/equiv.py')
e = importlib.util.module_from_spec(spec)
spec.loader.exec_module(e)
address = 0x80063108
asm = ROOT/'decomp/asm/us/main/nonmatchings/main/func_80063108.s'
image = b''.join(bytes.fromhex(w) for w in re.findall(
    r'/\* [0-9A-F]+ [0-9A-F]{8} ([0-9A-F]{8}) \*/', asm.read_text()))

class Cpu(e.Cpu):
    def external(self, target):
        assert target == 0x800632B4
        count, pointer = e.s32(self.r[4]), self.r[5]
        values = [e.s32((self.m.read(pointer+i, 1) ^ 128)-128) for i in range(max(0, count))] if pointer else []
        self.r[2] = sum(values) & 0xffffffff

rng = random.Random(0x63118)
cases, expected = [], []
for trial in range(400):
    flags = trial % 8
    rewind = rng.choice([0, 1, 255, 256, 0xffffff00, 0xffffffff])
    data = bytearray(rng.randbytes(96))
    struct.pack_into('<h', data, 18, rng.randrange(-2, 33))
    struct.pack_into('<2I', data, 20, 0x80180020 if flags & 2 else 0,
                     0x80180040 if flags & 4 else 0)
    cases.append(struct.pack('<2I', flags, rewind) + data)
    mem = e.Mem(trial, [(address, image), (0x80180000, data)])
    cpu = Cpu(mem, address, address+len(image), {}, {}, trial,
              [0x80180000, 0x80180010 if flags & 1 else 0, rewind, 0])
    cpu.run(0x80063118)
    assert not cpu.events, cpu.events
    expected.append(bytes(mem.read(0x80180000+i, 1) for i in range(16)))
result = subprocess.run([ROOT/'build/tests/window_probe'], input=b''.join(cases), capture_output=True)
assert result.returncode == 0, result.stderr.decode()
assert result.stdout == b''.join(expected), 'window setup differs from original MIPS'
print('Window setup matches original MIPS in 400 cases (shared signed-delta sum)')
