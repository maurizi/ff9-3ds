#!/usr/bin/env python3
"""Generate ignored ARM9 fixtures from passing PSX MDEC DMA captures."""
import argparse
import hashlib
import json
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('reference',type=Path)
p.add_argument('output',type=Path)
a=p.parse_args()
directory=a.reference.resolve();output=a.output.resolve()
if not directory.is_relative_to(ROOT/'build') or not output.is_relative_to(ROOT/'build'):
    p.error('reference and output must stay under build/')
rows=json.loads((directory/'report.json').read_text())
if not rows or not all(r['match'] for r in rows):p.error('reference comparisons must have passed')
selected={0}
for name,command in [('impulse_b0_k4',0x30000000),('dc_-512',0x3c000000)]:
    selected.add(next(i for i,r in enumerate(rows) if r['name']==name and r['command']==command))
for command in [0x30000000,0x38000000]:
    selected.add(max((i for i,r in enumerate(rows) if r['command']==command),key=lambda i:len(rows[i]['rl'])))
selected=sorted(selected)
lines=['.section .rodata.mdec_fixture,"a",%progbits','.balign 4',
       '.global mdec_fixture_tables','mdec_fixture_tables:',
       '.incbin "decomp/disks/us/ovl/64.bin", 0x57c8, 128',
       '.incbin "decomp/disks/us/ovl/64.bin", 0x584c, 128']
for i in selected:
    path=directory/f'psx_{i:04}.bin'
    if hashlib.sha256(path.read_bytes()).hexdigest()!=rows[i]['reference_sha256']:
        p.error('reference pixels changed after comparison')
    lines+=['.balign 4',f'rl_{i}:','.hword '+','.join(str(w) for w in rows[i]['rl']),
            '.balign 4',f'pixels_{i}:',f'.incbin {json.dumps(str(path.relative_to(ROOT)))}']
lines+=['.balign 4','.global mdec_fixture_count','mdec_fixture_count:',f'.word {len(selected)}',
        '.global mdec_fixtures','mdec_fixtures:']
for i in selected:
    r=rows[i]
    lines+=[f'.word {r["command"]},{len(r["rl"])},rl_{i},pixels_{i},{r["bytes"]},{r["consumed"]}']
output.parent.mkdir(parents=True,exist_ok=True)
output.write_text('\n'.join(lines)+'\n')
output.with_suffix('.json').write_text(json.dumps([dict(index=i,**rows[i]) for i in selected],indent=2)+'\n')
print(f'{len(selected)} PSX pixel fixtures -> {output}')
