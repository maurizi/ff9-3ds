#!/usr/bin/env python3
"""Compare environment packets with original PsyQ MIPS, including its helpers."""
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
start, end = 0x80013ef0, 0x80014544
image = bytearray(end-start)
for name in ('SetDrawEnv', 'func_800140E8', 'func_80014358', 'func_80014378',
             'func_80014410', 'func_800144A8', 'func_800144C4'):
    asm = (ROOT/f'decomp/asm/us/main/nonmatchings/main/{name}.s').read_text()
    for address, word in re.findall(r'/\* [0-9A-F]+ ([0-9A-F]{8}) ([0-9A-F]{8}) \*/', asm):
        off = int(address, 16)-start
        image[off:off+4] = bytes.fromhex(word)
rng = random.Random(0x140e8)
cases, expected = [], []
for trial in range(512):
    env = bytearray(rng.randbytes(92))
    # Mix arbitrary saved structures with normal aligned/unaligned draw areas.
    if trial % 3:
        values = [-1, 0, 1, 8, 63, 64, 112, 200, 256, 320, 511, 512, 1023, 1024, 32767, -32768]
        struct.pack_into('<6h', env, 0, *(rng.choice(values) for _ in range(6)))
    env[24] = trial % 4 != 0
    for immediate in (0, 1):
        address = 0x800140e8 if immediate else start
        mem = e.Mem(trial, [(start, bytes(image)), (0x80180000, bytes(env)),
                           (0x80066660, struct.pack('<2h', 1024, 512))])
        cpu = e.Cpu(mem, start, end, {}, {}, trial,
                    [0x8018001c, 0x80180000, 0, 0])
        cpu.run(address)
        assert not cpu.events and not cpu.calls, (trial, cpu.events, cpu.calls)
        want = bytearray(mem.read(0x8018001c+i, 1) for i in range(64))
        if immediate:
            want[:3] = b'\xff'*3  # PutDrawEnv terminates the packet before submission.
        cases.append(struct.pack('<I', immediate)+env)
        expected.append(want)
result = subprocess.run([ROOT/'build/tests/gpu_env_probe'], input=b''.join(cases), capture_output=True)
assert result.returncode == 0, result.stderr.decode()
assert len(result.stdout) == 64*len(expected)
for trial, want in enumerate(expected):
    got = result.stdout[trial*64:(trial+1)*64]
    assert got == want, (trial, got.hex(), want.hex())
print(f'Draw environments match original PsyQ instructions in {len(expected)} cases')
