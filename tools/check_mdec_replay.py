#!/usr/bin/env python3
"""Replay stored DuckStation MDEC captures through the production decoder, without an emulator.

Reads a directory written by test_mdec_psx.py (report.json plus psx_NNNN.bin reference outputs), sends each case's
run-level input and command through build/tests/mdec_probe with the user's ovl_64 tables, and compares the pixels
byte for byte. Use it to check decoder changes against the existing PSX evidence; it adds no new reference data.
"""
import argparse
import json
from pathlib import Path
import struct
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('reference', type=Path, nargs='?', default=ROOT / 'build/validation/mdec_psx_movies')
p.add_argument('--probe', type=Path, default=ROOT / 'build/tests/mdec_probe')
p.add_argument('--overlay', type=Path, default=ROOT / 'decomp/disks/us/ovl/64.bin')
p.add_argument('--repeat', type=int, default=1, help='decode the suite this many times (timing)')
p.add_argument('--bench-out', type=Path, help='also write the real-movie cases (tables + inputs) for the DS benchmark')
a = p.parse_args()

overlay = a.overlay.read_bytes()
quant = overlay[0x8019DEC8 - 0x80198700:][:128]
scale = overlay[0x8019DF4C - 0x80198700:][:128]
if len(quant) != 128 or len(scale) != 128:
    raise SystemExit('truncated MDEC tables')
cases = json.loads((a.reference / 'report.json').read_text())
one = b''.join(struct.pack('<II', c['command'], len(c['rl'])) + struct.pack(f"<{len(c['rl'])}H", *c['rl'])
               for c in cases)
if a.bench_out:
    a.bench_out.parent.mkdir(parents=True, exist_ok=True)
    a.bench_out.write_bytes(quant + scale + b''.join(
        struct.pack('<II', c['command'], len(c['rl'])) + struct.pack(f"<{len(c['rl'])}H", *c['rl'])
        for c in cases if c['name'].startswith('disc')))
start = time.perf_counter()
out = subprocess.run([str(a.probe)], input=quant + scale + one * a.repeat, capture_output=True, check=True).stdout
elapsed = time.perf_counter() - start
pos, bad = 0, []
for i, c in enumerate(cases):
    status, used = struct.unpack_from('<iI', out, pos)
    pos += 8
    size = 768 if ((c['command'] >> 27) & 3) == 2 else 512
    got = out[pos:pos + size] if status == 0 else b''
    pos += size if status == 0 else 0
    ref = (a.reference / f'psx_{i:04}.bin').read_bytes()
    if status or used != c['consumed'] or got != ref:
        bad.append((i, c['name'], status, used, c['consumed']))
print(f'{len(cases) - len(bad)}/{len(cases)} macroblocks match the PSX captures; '
      f'{a.repeat}x suite decoded in {elapsed:.3f} s')
for row in bad[:10]:
    print('mismatch', row)
raise SystemExit(1 if bad else 0)
