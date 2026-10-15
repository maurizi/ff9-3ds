#!/usr/bin/env python3
"""Probe one battle command through the host engine's ordinary command queue.

Run inside the build container. Replaces the first player Attack's command,
ability and target words at btl_setCmd; preserves its actor, ATB and queue slot.
This tests dispatch/effect execution, not ability learning or menu selection.
Use native DS and original PSX captures separately to check presentation.
"""
import argparse
import json
from pathlib import Path
import re
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('scenario', type=Path)
    parser.add_argument('--command', type=int, required=True)
    parser.add_argument('--ability', type=int, required=True)
    parser.add_argument('--actor', type=int, choices=range(9), help='only replace an Attack from this character ID')
    parser.add_argument('--require-trance', action='store_true', help='wait for the actor to enter trance before replacing an Attack')
    parser.add_argument('--targets', type=lambda s: int(s, 0), default=0x10)
    parser.add_argument('--frames', type=int, default=12000)
    parser.add_argument('--timeout', type=float, default=60)
    parser.add_argument('--exe', type=Path, default=Path('build/pc/ff9ds.elf'))
    parser.add_argument('--data', type=Path, help='runtime data root, including optional BIOS RAM image')
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    if not 1 <= args.command <= 0x30 or not 0 <= args.ability < 192:
        parser.error('command must be 1..48 and ability 0..191')
    if not 1 <= args.targets <= 255 or args.frames <= 1800 or args.timeout <= 0:
        parser.error('targets must be 1..255, frames >1800, timeout positive')
    root = Path(__file__).resolve().parents[1]
    if not args.out.resolve().is_relative_to(root / 'build') or args.out.exists():
        parser.error('output must be a new directory under build/')
    # GDB command files have their own quoting rules. Keep file arguments quoted
    # and reject control characters before passing paths to its command parser.
    def quote(path):
        value = str(path.resolve())
        if any(ord(c) < 32 for c in value):
            parser.error('control character in path')
        return '"' + value.replace('\\', '\\\\').replace('"', '\\"') + '"'
    scenario = quote(args.scenario)
    data = '--data ' + quote(args.data) if args.data else ''
    actor = f' && ((arg0->unk4->unk68 >> 1) & 15)=={args.actor}' if args.actor is not None else ''
    if args.require_trance:
        actor += ' && (arg0->unk4->status & 0x4000)'
    args.out.mkdir(parents=True)
    script = args.out / 'probe.gdb'
    presses = ' '.join(f'--press CROSS@{frame}+4'
                       for frame in range(1000, min(6000, args.frames), 40))
    script.write_text(f'''set pagination off
set confirm off
set $cast=0
break o0b_btl_setCmd
commands
silent
if (arg0->unk4->unk68 & 1) && arg1==1 && $cast==0{actor}
set arg1={args.command}
set arg2={args.ability}
set arg3={args.targets}
set $cast=1
printf "PROBE_QUEUE cmd=%u ability=%u targets=%x actor=%u status=%x\\n",arg1,arg2,arg3,(arg0->unk4->unk68 >> 1) & 15,arg0->unk4->status
printf "PROBE_CHARACTER model=%u preset=%u flags=%x\\n",g_gameState->unk1C->characters[(arg0->unk4->unk68 >> 1) & 15].unk34.b.unk4,g_gameState->unk1C->characters[(arg0->unk4->unk68 >> 1) & 15].unk34.b.row,g_gameState->unk1C->characters[(arg0->unk4->unk68 >> 1) & 15].unkA
end
continue
end
break o0b_func_800C2598
commands
silent
if (arg0->unk4->unk68 & 1) && arg0->cmdNo==59 && (arg0->unk4->status & 0x4000){actor}
printf "PROBE_TRANCE_ENTER actor=%u effect=%u\\n",(arg0->unk4->unk68 >> 1) & 15,arg1
end
if (arg0->unk4->unk68 & 1) && arg0->cmdNo=={args.command} && arg0->subNo=={args.ability}{actor}
printf "PROBE_EFFECT cmd=%u ability=%u effect=%u\\n",arg0->cmdNo,arg0->subNo,arg1
end
continue
end
run {data} --scenario {scenario} --frames {args.frames} --status 1000 {presses}
if $_isvoid($_exitcode)
backtrace 20
quit 3
else
quit $_exitcode
end
''')
    log = args.out / 'process.log'
    with log.open('wb') as output:
        try:
            code = subprocess.run(['gdb', '-q', '-batch', '-x', str(script), str(args.exe)],
                                  stdout=output, stderr=subprocess.STDOUT,
                                  timeout=args.timeout).returncode
        except subprocess.TimeoutExpired:
            code = None
    text = log.read_text(errors='replace')
    character = re.search(r'PROBE_CHARACTER model=(\d+) preset=(\d+) flags=([0-9a-f]+)', text)
    queue = re.search(r'PROBE_QUEUE [^\n]*actor=(\d+)', text)
    queued_actor = int(queue[1]) if queue else None
    trance_actors = {int(value) for value in re.findall(r'PROBE_TRANCE_ENTER actor=(\d+)', text)}
    result = dict(scenario=str(args.scenario), command=args.command, ability=args.ability,
                  actor=args.actor,
                  queued_actor=queued_actor,
                  require_trance=args.require_trance,
                  character=({'model': int(character[1]), 'preset': int(character[2]),
                              'flags': int(character[3], 16)} if character else None),
                  data=str(args.data) if args.data else None,
                  targets=args.targets, returncode=code, queued='PROBE_QUEUE ' in text,
                  effect_started='PROBE_EFFECT ' in text,
                  trance_started=queued_actor in trance_actors,
                  frame_limit=f'frame limit {args.frames} reached' in text,
                  failure=next((line for line in text.splitlines()
                                if 'FATAL:' in line or 'received signal' in line), None))
    result['pass_execution'] = (code == 0 and result['queued'] and
                                result['effect_started'] and result['frame_limit'] and
                                (not args.require_trance or result['trance_started']))
    (args.out / 'report.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))
    return 0 if result['pass_execution'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
