#!/usr/bin/env python3
"""Capture BIOS-initialized low RAM at FF9's original executable entry.

Start the original PSX emulator cold and paused with your own BIOS/disc first.
Only the 64 KiB low-RAM guest region is captured. The port keeps these bytes
as data for original low-address reads.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--decomp', type=Path, default=Path(__file__).resolve().parents[2] / 'ff9-decomp')
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    output = args.out.resolve()
    if not any(output.is_relative_to(root / d) for d in ('build', 'ff9ds')) or output.exists():
        parser.error('output must be a new path under build/ or ff9ds/')
    spec = importlib.util.spec_from_file_location('ff9_emu', args.decomp / 'tools/emu.py')
    emu = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(emu)
    executable = (args.decomp / 'disks/us/SLUS_012.51').read_bytes()
    if executable[:8] != b'PS-X EXE':
        parser.error('expected the original US executable')
    entry = struct.unpack_from('<I', executable, 0x10)[0]
    load = struct.unpack_from('<I', executable, 0x18)[0]
    if emu.is_running():
        parser.error('reference emulator must be paused')
    pc = emu.bridge('status')['pc']
    if pc != entry:
        if not 0xbfc00000 <= pc < 0xbfc80000:
            parser.error('requires a cold BIOS, or a stop at the original executable entry')
        bp = emu.bridge('addBp', entry, 'Exec', 4, 'break', 1, 'capture_boot_kernel')
        try:
            emu.run_frames(3600)
            if emu.bridge('status')['pc'] != entry:
                parser.error('cold boot did not reach the executable entry')
        finally:
            emu.bridge('removeBp', bp)
    expected = executable[0x800 + entry - load:0x800 + entry - load + 32]
    if len(expected) != 32 or bytes.fromhex(emu.bridge('read', entry, 32)) != expected:
        parser.error('reference executable bytes do not match the supplied game')
    payload = bytes.fromhex(emu.bridge('read', 0x80000000, 0x10000))
    if len(payload) != 0x10000:
        parser.error('incomplete low-RAM capture')
    checksum = 2166136261
    for byte in payload:
        checksum = ((checksum ^ byte) * 16777619) & 0xffffffff
    header = struct.pack('<4sIII', b'PXR1', len(payload), checksum, entry)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(header + payload)
    metadata = dict(entry=f'{entry:08x}', bytes=len(payload),
                    payload_sha256=hashlib.sha256(payload).hexdigest(),
                    executable_sha256=hashlib.sha256(executable).hexdigest(),
                    bios_sha256=hashlib.sha256(emu.BIOS.read_bytes()).hexdigest(),
                    frame=emu.bridge('status')['frame'])
    output.with_suffix(output.suffix + '.json').write_text(json.dumps(metadata, indent=2) + '\n')
    print(json.dumps(dict(path=str(output), **metadata), indent=2))


if __name__ == '__main__':
    main()
