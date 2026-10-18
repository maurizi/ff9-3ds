#!/usr/bin/env python3
"""Batch direct-entry scenarios and retain each failure, without an emulator.

Run inside the build container. Nonzero game exits (including missing routines)
and timeouts are failures, never hidden by a debugger's successful exit status.
This checks execution only; native DS video/PSX comparisons remain necessary.
--require-field-loaded additionally checks that the requested field's loader
returned. A frame limit by itself can be reached during loading or polling.
"""
import argparse
import json
import re
import signal
from pathlib import Path
import subprocess
import time


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('scenarios', nargs='+', type=Path)
    p.add_argument('--exe', default='build/pc/ff9ds')
    p.add_argument('--data', type=Path, help='runtime data root')
    p.add_argument('--frames', type=int, default=12000)
    p.add_argument('--timeout', type=float, default=60)
    p.add_argument('--out', type=Path, default=Path('build/validation/scenarios'))
    p.add_argument('--press', action='append', default=[])
    p.add_argument('--render', action='store_true', help='rasterise too (the host skips drawing without it)')
    p.add_argument('--require-field-loaded', action='store_true',
                   help='also require the requested field to finish fld_load (field fixtures only)')
    args = p.parse_args()
    if args.frames <= 0 or args.timeout <= 0:
        p.error('frames and timeout must be positive')
    args.out.mkdir(parents=True, exist_ok=True)
    results = []
    for index, scenario in enumerate(args.scenarios):
        log = args.out / f'{index:02d}_{scenario.stem}.log'
        command = [args.exe, '--scenario', str(scenario), '--frames', str(args.frames), '--status', '1000']
        if args.data:
            command += ['--data', str(args.data)]
        for press in args.press:
            command += ['--press', press]
        if args.render:
            command += ['--render']
        started = time.monotonic()
        with log.open('wb') as output:
            try:
                completed = subprocess.run(command, stdout=output, stderr=subprocess.STDOUT, timeout=args.timeout)
                code = completed.returncode
                reason = (f'signal {-code} ({signal.strsignal(-code)})' if code < 0
                          else f'exit {code}')
            except subprocess.TimeoutExpired:
                code, reason = None, 'timeout'
        text = log.read_text(errors='replace')
        frame_limit_reached = code == 0 and f'frame limit {args.frames} reached' in text
        requested = re.search(r'^scenario configured: disc \d+, field (\d+)$', text, re.M)
        requested_field = int(requested[1]) if requested else None
        attempts = [int(n) for n in re.findall(r'^scenario field entering: (\d+)$', text, re.M)]
        loaded = [int(n) for n in re.findall(r'^scenario field ready: (\d+)$', text, re.M)]
        requested_loaded = requested_field in loaded if requested_field is not None else None
        passed = frame_limit_reached and (not args.require_field_loaded or requested_loaded is True)
        if frame_limit_reached and not passed:
            reason = 'requested field did not finish loading'
        row = {'scenario': str(scenario), 'data': str(args.data) if args.data else None, 'pass': passed, 'returncode': code,
               'reason': reason, 'seconds': round(time.monotonic()-started, 3), 'log': str(log),
               'fatal': next((line for line in text.splitlines() if 'FATAL:' in line), None),
               'frame_limit_reached': frame_limit_reached, 'requested_field': requested_field,
               'field_attempts': attempts, 'loaded_fields': loaded,
               'requested_field_loaded': requested_loaded}
        results.append(row)
        # Retain completed cases if the sweep is interrupted later.
        pending = args.out / 'report.json.tmp'
        pending.write_text(json.dumps(results, indent=2) + '\n')
        pending.replace(args.out / 'report.json')
        print(f"{'PASS' if passed else 'FAIL'} {scenario}: {row['fatal'] or reason}", flush=True)
    return 0 if all(row['pass'] for row in results) else 1


if __name__ == '__main__':
    raise SystemExit(main())
