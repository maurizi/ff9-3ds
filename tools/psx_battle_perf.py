#!/usr/bin/env python3
"""A battle fixture's frame rate on the original PSX (the decomp's PCSX-Redux): the host's bootstrapped party and
story exported (FF9_SCENARIO_EXPORT), the battle entered by tools/psx_battle_fixture.py from a field state on the
same disc, then VBlanks per game frame counted as tools/psx_field_fixture.py counts them (no breakpoint set).
Battles pace at 4 VBlanks a frame (vsyncMode 4) when they keep up. Redux's timing is approximate (see
psx_field_fixture.py)."""
import argparse
import importlib.util
import json
from pathlib import Path
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from psx_field_fixture import BASE_STATES, GAME_STATE, PERF_LUA  # noqa: E402


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('scenario', type=Path)
    p.add_argument('--base-state')
    p.add_argument('--settle', type=int, default=900)
    p.add_argument('--measure', type=int, default=600)
    p.add_argument('--nominal', type=float, default=4.0)
    p.add_argument('--decomp', type=Path, default=ROOT.parent / 'ff9-decomp')
    p.add_argument('--shot', type=Path)
    args = p.parse_args()
    config = dict(l.split('=', 1) for l in (x.strip() for x in args.scenario.read_text().splitlines())
                  if l and not l.startswith('#'))
    disc = int(config.get('disc', '1'), 0)
    out = ROOT / 'build/psxperf' / args.scenario.stem
    out.mkdir(parents=True, exist_ok=True)
    subprocess.run(['tools/docker.sh', 'env', f'FF9_SCENARIO_EXPORT=build/psxperf/{args.scenario.stem}', 'build/pc/ff9ds',
                    '--scenario', str(args.scenario), '--frames', '300'], cwd=ROOT, check=True, capture_output=True)
    subprocess.run([sys.executable, 'tools/psx_battle_fixture.py', str(args.scenario), '--characters',
                    str(out / 'characters.bin'), '--events', str(out / 'events.bin'), '--base-state',
                    args.base_state or BASE_STATES[disc], '--decomp', str(args.decomp)], cwd=ROOT, check=True)
    spec = importlib.util.spec_from_file_location('ff9_emu', args.decomp / 'tools/emu.py')
    emu = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(emu)
    gs = emu.bridge('r32', GAME_STATE)
    gd = emu.bridge('r32', gs + 0x1C)
    emu.lua(PERF_LUA)
    emu.lua(f'Perf.gd = {gd}; Perf.on = false; return 1')
    emu.run_frames(args.settle)
    emu.lua('Perf.v = 0; Perf.f = 0; Perf.last = -1; Perf.on = true; return 1')
    emu.run_frames(args.measure)
    v = int(emu.lua('return Perf.v'))
    f = int(emu.lua('return Perf.f'))
    emu.lua('Perf.on = false; return 1')
    if args.shot:
        args.shot.write_bytes(emu.http('/screen/still', timeout=10))
    per_frame = v / f if f else float('inf')
    print(json.dumps({'scenario': args.scenario.stem, 'disc': disc, 'battle': int(config['battle'], 0),
                      'mode': emu.bridge('read', gs + 8, 1), 'vblanks': v, 'game_frames': f,
                      'vblanks_per_frame': round(per_frame, 3),
                      'ps1_ms_per_nominal_vblank': round(1000 / 59.94 * per_frame / args.nominal, 2)}))


if __name__ == '__main__':
    main()
