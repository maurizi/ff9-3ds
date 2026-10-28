#!/usr/bin/env python3
"""Enter a field fixture on the original PSX (the decomp's PCSX-Redux) and measure its frame rate there.

The DS fixture's party and story come from the host's scenario bootstrap (FF9_SCENARIO_EXPORT, run here through
tools/docker.sh). From a paused field state on the same disc, the field is left through its own cleanup (main's
dispatcher, as tools/psx_battle_fixture.py), the blobs and the target field's CD extents (FF9.IMG directories 4 and
2, as ovl_14 opening_findSceneSectors finds them) are written, and the normal field loader runs. Then, with no
breakpoint set (Redux's debugger changes its timing), a per-VBlank Lua listener counts VBlanks and the game's draw
buffer flips (GameData.unk8): VBlanks per game frame, which a field paces at 2 (VSync(2)) when it keeps up.

Redux's CPU timing is an approximation (a fixed cost per instruction, the GPU's drawing free): read the result as
the PS1's frame rate within that model, not cycle-exact.
"""
import argparse
import importlib.util
import json
from pathlib import Path
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[1]
SECTOR = 2048
DISPATCH = 0x80012834  # main's return from a mode overlay (tools/psx_battle_fixture.py)
GAME_STATE = 0x8006794C
EVENT_VARS = 0x8007AEF0
BASE_STATES = {1: 'field_dali_inn', 2: 'field_iifa_tree', 3: 'disc3_pandemonium_lv99_field', 4: 'ds_perf_disc4_treno'}


def directory(image, index):
    image.seek(0)
    header = image.read(SECTOR)
    if header[:4] != b'FF9 ':
        raise ValueError('not FF9.IMG')
    kind, count, table, _ = struct.unpack_from('<4I', header, 16 + index * 16)
    if kind != 2:
        raise ValueError(f'unsupported directory {index}')
    image.seek(table * SECTOR)
    data = image.read((count + 1) * 8)
    return [struct.unpack_from('<HHI', data, i * 8) for i in range(count + 1)]


def field_extents(path, field):
    """(area id, [area sector, field sector], [area sectors, field sectors]) as opening_findSceneSectors finds them"""
    with path.open('rb') as image:
        fields = directory(image, 4)
        for i, (fid, link, sector) in enumerate(fields[:-1]):
            if fid == field:
                fsec, flen = sector, fields[i + 1][2] - sector
                break
        else:
            raise ValueError(f'field {field} not on this disc')
        areas = directory(image, 2)
        for i, (aid, _, sector) in enumerate(areas[:-1]):
            if aid == link:
                return link, [sector, fsec], [areas[i + 1][2] - sector, flen]
    raise ValueError(f'area {link} of field {field} not on this disc')


PERF_LUA = '''
if not PerfL then
  Perf = {v = 0, f = 0, last = -1, gd = 0, on = false}
  PerfL = PCSX.Events.createEventListener('GPU::Vsync', function()
    pcall(function()
      if Perf.on and Perf.gd ~= 0 then
        local b = PCSX.getMemPtr()[bit.band(Perf.gd + 8, 0x1fffff)]
        Perf.v = Perf.v + 1
        if b ~= Perf.last then Perf.f = Perf.f + 1; Perf.last = b end
      end
    end)
    return true
  end)
end
return 1
'''


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('scenario', type=Path)
    p.add_argument('--base-state', help='a paused field state on the fixture\'s disc (default: one per disc)')
    p.add_argument('--settle', type=int, default=600, help='VBlanks after the field loads before measuring')
    p.add_argument('--measure', type=int, default=600, help='VBlanks measured')
    p.add_argument('--nominal', type=float, default=2.0, help='VBlanks per game frame at full speed (fields: 2)')
    p.add_argument('--decomp', type=Path, default=ROOT.parent / 'ff9-decomp')
    p.add_argument('--shot', type=Path, help='a screenshot at the end of the window')
    args = p.parse_args()
    config = {}
    for line in args.scenario.read_text().splitlines():
        line = line.strip()
        if line and not line.startswith('#'):
            key, value = line.split('=', 1)
            config[key.strip()] = value.strip()
    if config.get('kind') != 'field':
        p.error('requires a field scenario')
    disc = int(config.get('disc', '1'), 0)
    field = int(config['field'], 0)
    party = [int(v) for v in config.get('party', '0,1,2,3').split(',')]
    area, sectors, lengths = field_extents(ROOT / f'disks/us/disc{disc}/FF9.IMG', field)
    out = ROOT / 'build/psxperf' / args.scenario.stem
    out.mkdir(parents=True, exist_ok=True)
    subprocess.run(['tools/docker.sh', 'env', f'FF9_SCENARIO_EXPORT=build/psxperf/{args.scenario.stem}', 'build/pc/ff9ds',
                    '--scenario', str(args.scenario), '--frames', '300'], cwd=ROOT, check=True, capture_output=True)
    characters, events = (out / 'characters.bin').read_bytes(), (out / 'events.bin').read_bytes()
    if len(characters) != 9 * 0x90 or len(events) != 0x800:
        p.error('the host did not export the bootstrapped state')

    spec = importlib.util.spec_from_file_location('ff9_emu', args.decomp / 'tools/emu.py')
    emu = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(emu)
    if emu.is_running():
        p.error('pause the reference emulator first')
    if emu.bridge('status')['pc'] >= 0xBFC00000:
        emu.run_frames(1800)  # (Redux's cold-boot BIOS must run before a state restores)
    emu.http('/state/load?name=' + (args.base_state or BASE_STATES[disc]))
    emu.http('/execution-flow?function=pause', 'POST')
    emu.lua('jit.off(); jit.flush(); return 1')
    gs = emu.bridge('r32', GAME_STATE)
    gd = emu.bridge('r32', gs + 0x1C)
    if emu.bridge('read', gs + 8, 1) != '01':
        p.error('the base state must be a loaded field')

    def word(address, value):
        emu.bridge('write', address, struct.pack('<I', value & 0xFFFFFFFF).hex())

    bp = emu.bridge('addBp', DISPATCH, 'Exec', 4, 'break', 1, 'psx_field_fixture_dispatch')
    try:
        emu.bridge('write', gs + 10, '01')
        word(gs, emu.bridge('r32', gs) | 2)  # (GS_FLD_LEAVE: the field's own exit)
        emu.run_frames(600)
        if emu.bridge('status')['pc'] != DISPATCH:
            p.error('the field exit did not reach main\'s dispatcher')
    finally:
        emu.bridge('removeBp', bp)
    emu.bridge('write', gd + 0x3C, characters.hex())
    emu.bridge('write', EVENT_VARS, events.hex())
    for i, ch in enumerate(party):
        word(gd + 0x54C + i * 4, 0 if ch < 0 else gd + 0x3C + ch * 0x90)
    for i in range(2):
        word(gd + 0x7FC + i * 4, sectors[i])
        word(gd + 0x804 + i * 4, lengths[i])
    emu.bridge('write', gd + 12, struct.pack('<H', field).hex())
    emu.bridge('write', gd + 0x3A, struct.pack('<H', area).hex())
    emu.bridge('write', gs + 8, '01')
    emu.bridge('write', gs + 10, '01')

    emu.lua(PERF_LUA)
    emu.lua(f'Perf.gd = {gd}; Perf.on = false; return 1')
    emu.run_frames(args.settle)
    emu.lua('Perf.v = 0; Perf.f = 0; Perf.last = -1; Perf.on = true; return 1')
    emu.run_frames(args.measure)
    v = int(emu.lua('return Perf.v'))
    f = int(emu.lua('return Perf.f'))
    emu.lua('Perf.on = false; return 1')
    loaded = struct.unpack('<H', bytes.fromhex(emu.bridge('read', gd + 12, 2)))[0]
    if args.shot:
        args.shot.write_bytes(emu.http('/screen/still', timeout=10))
    per_frame = v / f if f else float('inf')
    print(json.dumps({'scenario': args.scenario.stem, 'disc': disc, 'field': field, 'field_now': loaded,
                      'mode': emu.bridge('read', gs + 8, 1), 'vblanks': v, 'game_frames': f,
                      'vblanks_per_frame': round(per_frame, 3),
                      'ps1_ms_per_nominal_vblank': round(1000 / 59.94 * per_frame / args.nominal, 2)}))


if __name__ == '__main__':
    main()
