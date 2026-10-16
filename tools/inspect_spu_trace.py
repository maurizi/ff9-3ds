#!/usr/bin/env python3
"""Validate/sample-index a host SPU journal; optionally export its LE32 stages."""
import argparse
from collections import Counter
import json
from pathlib import Path
import struct


def events(path):
    with path.open('rb') as f:
        if f.read(8) != b'FF9SPU1\0':
            raise ValueError('bad SPU trace signature')
        initialized = False
        sample = 0
        while True:
            header = f.read(8)
            if len(header) != 8:
                raise ValueError('truncated SPU trace (missing completion footer)')
            kind, size = struct.unpack('<II', header)
            if kind == 0xffffffff:
                if size or f.read(1):
                    raise ValueError('bad completion footer')
                return
            if size > 0x80008:
                raise ValueError('oversized SPU trace record')
            data = f.read(size)
            if len(data) != size:
                raise ValueError('truncated SPU trace record')
            if kind == 0 and not size:
                initialized = True
            elif not initialized:
                raise ValueError('SPU trace must start with reset')
            elif kind == 1 and size == 8:
                offset, value = struct.unpack('<II', data)
                if offset >= 0x400 or offset & 1 or value > 65535:
                    raise ValueError('invalid register event')
            elif kind == 2 and size >= 8:
                address, count = struct.unpack('<II', data[:8])
                if count != size - 8 or address > 0x80000 or count > 0x80000 - address:
                    raise ValueError('invalid RAM event')
            elif kind != 3 or size != 32:
                raise ValueError('invalid SPU trace event')
            yield sample, kind, data
            if kind == 3:
                sample += 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('journal', type=Path)
    parser.add_argument('--stages', type=Path, help='write new LE32 stage file (dry/send/wet/PCM)')
    parser.add_argument('--writes', action='store_true', help='list register events with sample indices')
    args = parser.parse_args()
    counts = Counter()
    samples = 0
    out = args.stages.open('xb') if args.stages else None
    try:
        for sample, kind, data in events(args.journal):
            counts[kind] += 1
            if kind == 3:
                if out:
                    out.write(data)
                samples += 1
            elif kind == 1 and args.writes:
                offset, value = struct.unpack('<II', data)
                print(f'{sample:10d} W {offset:03x} {value:04x}')
        print(json.dumps({'samples': samples, 'resets': counts[0], 'register_writes': counts[1],
                          'ram_events': counts[2]}, indent=2))
    finally:
        if out:
            out.close()


if __name__ == '__main__':
    main()
