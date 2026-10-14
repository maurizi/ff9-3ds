#!/usr/bin/env python3
"""Wait for a DS game frame, then save a checkpoint and record its animation.

Linux/melonDS fast-memory backend only. Requires the running ROM's matching ELF.
Uses the game's frame counter rather than a wall-clock guess. Recording starts
when the threshold is observed, after an optional checkpoint has been saved.
"""
import argparse
import os
from pathlib import Path
import struct
import subprocess
import sys
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pid', type=int, required=True)
    parser.add_argument('--window', required=True)
    parser.add_argument('--rom', type=Path, required=True)
    parser.add_argument('--elf', type=Path, required=True)
    parser.add_argument('--frame', type=int, required=True)
    parser.add_argument('--seconds', type=float, default=30)
    parser.add_argument('--timeout', type=float, default=600)
    parser.add_argument('--state-out', type=Path)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    if args.pid <= 0 or args.frame <= 0 or args.timeout <= 0 or not 0 < args.seconds <= 60:
        parser.error('PID, frame and timeout must be positive; recording must be at most 60 seconds')
    for output in (args.out, args.state_out):
        if output and (not output.resolve().is_relative_to(root / 'build') or output.exists()):
            parser.error('outputs must be new paths under build/')
    if args.out.suffix != '.mp4':
        parser.error('--out must end in .mp4')
    proc = Path(f'/proc/{args.pid}')
    command = proc.joinpath('cmdline').read_bytes().split(b'\0')
    if os.fsencode(str(args.rom.resolve())) not in command:
        parser.error('PID is not running the specified ROM')
    symbols = {}
    for row in subprocess.check_output(['nm', '-a', str(args.elf)], text=True).splitlines():
        parts = row.split()
        if len(parts) == 3:
            symbols[parts[2]] = int(parts[0], 16)
    address = symbols.get('frames', 0)
    if not 0x02000000 <= address < 0x03000000:
        parser.error('matching ELF must expose a main-RAM frames counter')
    elf = args.elf.read_bytes()
    if elf[:6] != b'\x7fELF\x01\x01':
        parser.error('expected a little-endian ELF32 executable')
    code_address = symbols.get('plat_wait_frame', 0) & ~1
    code = None
    shoff = struct.unpack_from('<I', elf, 32)[0]
    shsize, shnum = struct.unpack_from('<2H', elf, 46)
    for i in range(shnum):
        section = struct.unpack_from('<10I', elf, shoff + i * shsize)
        if section[1] == 1 and section[3] <= code_address and code_address + 64 <= section[3] + section[5]:
            start = section[4] + code_address - section[3]
            code = elf[start:start + 64]
            break
    if code is None or not 0x02000000 <= code_address < 0x03000000:
        parser.error('matching ELF must contain plat_wait_frame in main RAM')
    # melonDS maps the same RAM at several aliases. Offset-zero mappings of
    # its fast-memory backing file start with the physical main-RAM image.
    bases = []
    for row in proc.joinpath('maps').read_text().splitlines():
        parts = row.split()
        lo, hi = (int(x, 16) for x in parts[0].split('-'))
        if ('melondsfastmem' in row and parts[1].startswith('rw') and
                int(parts[2], 16) == 0 and hi - lo >= 16 * 1024 * 1024):
            bases.append(lo)
    if not bases:
        parser.error('no melonDS fast-memory mapping found')
    with proc.joinpath('mem').open('rb', buffering=0) as memory:
        def read_at(base, symbol):
            memory.seek(base + symbols[symbol] - 0x02000000)
            value = memory.read(4)
            if len(value) != 4:
                sys.exit('emulator exited while reading its game frame')
            return struct.unpack('<I', value)[0]

        def has_native_code(base):
            memory.seek(base + code_address - 0x02000000)
            return memory.read(len(code)) == code

        # Check linked native instructions and a live VRAM allocation before
        # injecting input; address ranges alone cannot identify the build.
        bases = [base for base in bases if has_native_code(base) and 'psx_vram' in symbols and
                 0x02000000 <= read_at(base, 'psx_vram') < 0x03000000]
        if not bases:
            parser.error('matching ELF/RAM layout could not be verified')
        base = bases[0]
        previous = read_at(base, 'frames')
        if previous >= args.frame:
            parser.error(f'frame {previous} has already passed the requested capture threshold')
        print(f'Waiting for game frame {args.frame}; current frame {previous}', flush=True)
        deadline = time.monotonic() + args.timeout
        while previous < args.frame:
            if time.monotonic() >= deadline:
                sys.exit(f'timed out at frame {previous}; no capture started')
            time.sleep(0.05)
            current = read_at(base, 'frames')
            if current < previous:
                sys.exit('game frame moved backwards; no capture started')
            previous = current
        print(f'Triggered at game frame {previous}', flush=True)
    common = [sys.executable]
    if args.state_out:
        subprocess.run(common + [str(root / 'tools/save_ds_state.py'), '--window', args.window,
                       '--rom', str(args.rom), '--elf', str(args.elf), '--out', str(args.state_out)], check=True)
    subprocess.run(common + [str(root / 'tools/capture.py'), '--window', args.window,
                   '--seconds', str(args.seconds), '--out', str(args.out)], check=True)


if __name__ == '__main__':
    main()
