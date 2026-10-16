#!/usr/bin/env python3
"""Exercise the production journal writer and reject incomplete/invalid traces."""
import importlib.util
from pathlib import Path
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('inspect_spu_trace', ROOT/'tools/inspect_spu_trace.py')
trace = importlib.util.module_from_spec(spec)
spec.loader.exec_module(trace)


def main():
    with tempfile.TemporaryDirectory(dir=ROOT/'build/tests', prefix='spu-journal-') as tmp:
        path = Path(tmp)/'journal.bin'
        exe = ROOT/'build/tests/spu_journal_fixture'
        result = subprocess.run([str(exe), str(path)], check=True, capture_output=True, text=True)
        frames, nonzero, expected_hash = result.stdout.split()
        rows = list(trace.events(path))
        samples = [struct.unpack('<8i', data) for _, kind, data in rows if kind == 3]
        assert len(samples) == int(frames) == 7000
        h = 2166136261
        for sample in samples:
            for value in sample[6:]:
                h = ((h ^ (value & 65535)) * 16777619) & 0xffffffff
        assert h == int(expected_hash, 16)
        assert sum(value != 0 for sample in samples for value in sample[6:]) == int(nonzero)
        repeated = [(index, data) for index, kind, data in rows
                    if kind == 1 and struct.unpack('<II', data) == (2, 0x2000)]
        assert [index for index, _ in repeated] == [0, 5000, 5000]
        ram = [(index, data) for index, kind, data in rows if kind == 2]
        assert any(index == 5000 and struct.unpack('<II', data[:8]) == (0x1000, 16)
                   and data[12] == 0x8f for index, data in ram)
        original = path.read_bytes()
        retry = subprocess.run([str(exe), str(path)], capture_output=True)
        assert retry.returncode != 0 and path.read_bytes() == original
        invalid = [original[:-8], original[:50], b'badmagic',
                   b'FF9SPU1\0'+struct.pack('<4I', 0, 0, 99, 0),
                   b'FF9SPU1\0'+struct.pack('<6I', 0, 0, 2, 8, 0x80001, 0),
                   b'FF9SPU1\0'+struct.pack('<4I', 1, 8, 0, 0),
                   original+b'extra']
        bad = Path(tmp)/'invalid.bin'
        for value in invalid:
            bad.write_bytes(value)
            try:
                list(trace.events(bad))
            except ValueError:
                pass
            else:
                raise AssertionError('invalid trace accepted')
    print('SPU journal: output PCM, event order, RAM updates, no-overwrite and truncation checks PASS')


if __name__ == '__main__':
    main()
