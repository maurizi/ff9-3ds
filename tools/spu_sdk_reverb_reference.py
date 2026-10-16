#!/usr/bin/env python3
"""Probe original PsyQ reverb register behavior in a running, paused Redux.

Requires a warmed emulator with title_screen state. Overwrites scratch guest RAM
and CPU state; reload a state before playing again. No game bytes are exported.
Prints only synthetic inputs and hashes of SDK register results to stdout.
"""
import argparse
import importlib.util
from pathlib import Path
import struct


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--decomp', type=Path, default=Path('../ff9-decomp'))
    args = parser.parse_args()
    spec = importlib.util.spec_from_file_location('emu', args.decomp / 'tools/emu.py')
    e = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(e)
    e.http('/state/load?name=title_screen')
    e.http('/execution-flow?function=pause', 'POST')

    def call(name, arg):
        # Disable guest interrupts; call SDK; copy SPU MMIO halfwords into RAM.
        code = [0x40806000, 0, 0x0c000000 | ((e.parse_addr(name) >> 2) & 0x3ffffff), 0,
                0x3c098009, 0xad220000, 0x25290004, 0x3c081f80, 0x35081c00, 0x240a0200,
                0x950b0000, 0x25080002, 0xa52b0000, 0x25290002, 0x254affff, 0x1540fffa, 0]
        end = 0x801fee00 + 4 * len(code)
        code += [0x1000ffff, 0]
        e.write_mem(0x801fee00, struct.pack('<%dI' % len(code), *code))
        for reg, value in [('a0', arg), ('sp', 0x801fe000), ('pc', 0x801fee00)]:
            e.bridge('setReg', reg, value)
        bp = e.bridge('addBp', end, 'Exec', 4, 'break', 1, 'SDK reverb probe')
        try:
            e.run_frames(2)
            status = e.bridge('status')
            assert status['pc'] == end, status
        finally:
            e.bridge('removeBp', bp)
            e.lua('Bridge.pauseAt = nil return true')
        return struct.unpack('<I512H', e.read_mem(0x80090000, 1028))

    call('SpuSetReverbModeType', 0)
    call('SpuSetReverb', 1)
    # api 0=type, 1=mode attr, 2=depth attr, 3=enable. Deliberately sequential.
    cases = [(0, 0, mode, 0, 0, 0, 0) for mode in range(10)]
    cases += [(0, 0, mode, 0, 0, 0, 0) for mode in [-1, 10, 256+10]]
    for mode in [7, 8, 2]:
        cases += [(1, 0, mode, 12345, -23456, 61, 73)]
        for mask in [2, 4, 6, 8, 16, 24, 30, 32]:
            cases += [(1, mask, 99, -12345, 23456, 17, 83)]
    for mask in [0, 1, 2, 4, 6, 8, 31]:
        cases += [(2, mask, 99, -32768, 32767, 0, 0)]
    cases += [(3, 0, 0, 0, 0, 0, 0), (0, 0, 7, 0, 0, 0, 0)]
    for delay, feedback in [(0, 0), (127, 127), (-1, -1), (128, 128), (0x12345678, -0x12345678)]:
        cases += [(1, 24, 99, 0, 0, delay, feedback)]
    cases += [(1, 1, -1, 0, 0, 0, 0), (1, 1, 10, 0, 0, 0, 0), (3, 0, 1, 0, 0, 0, 0), (0, 0, 0, 0, 0, 0, 0)]
    print('/* Synthetic SDK inputs / original-library register hashes. See audio-validation.md. */')
    print('static const struct { int api; unsigned mask; int mode; short left,right; int delay,feedback; unsigned hash; } sdk_reverb_cases[] = {')
    for api, mask, mode, left, right, delay, feedback in cases:
        if api in (1, 2):
            e.write_mem(0x80098000, struct.pack('<Iihhii', mask, mode, left, right, delay, feedback))
        name = ['SpuSetReverbModeType', 'SpuSetReverbModeParam', 'SpuSetReverbDepth', 'SpuSetReverb'][api]
        raw = call(name, 0x80098000 if api in (1, 2) else mode & 0xffffffff)
        values = [raw[0] if api < 2 else 0, raw[1+0xc2], raw[1+0xc3], raw[1+0xd1], raw[1+0xd5] & 0x80] + list(raw[1+0xe0:1+0x100])
        h = 2166136261
        for value in values:
            h = ((h ^ value) * 16777619) & 0xffffffff
        print('    {%d,%d,%d,%d,%d,%d,%d,0x%08xu},' % (api,mask,mode,left,right,delay,feedback,h), flush=True)
    print('};')


if __name__ == '__main__':
    main()
