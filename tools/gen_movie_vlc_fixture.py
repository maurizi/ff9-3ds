#!/usr/bin/env python3
"""Embed selected passing PSX coefficient captures in an ignored DS test ROM.

Run from the project root; report/BS/RL files must be beneath build/.
No proprietary bytes are written into tracked sources.
"""
import argparse
import hashlib
import json
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('reference',type=Path,help='directory containing report.json and input_NNN.bs/psx_NNN.rl')
p.add_argument('output',type=Path)
a=p.parse_args()
reference=a.reference.resolve();output=a.output.resolve()
if not reference.is_relative_to(ROOT/'build') or not output.is_relative_to(ROOT/'build'):
    p.error('reference and output must be under build/')
rows=json.loads((reference/'report.json').read_text())
if not rows or not all(r['match'] for r in rows):p.error('reference comparison must have passed')
selected=sorted({0,len(rows)-1,max(range(len(rows)),key=lambda i:rows[i]['bytes']),
                 max(range(len(rows)),key=lambda i:rows[i]['bits'])})
lines=['.section .rodata.movie_vlc_fixture,"a",%progbits','.balign 4',
       '.global movie_vlc_ac_table','movie_vlc_ac_table:',
       '.incbin "decomp/disks/us/ovl/64.bin", 0x55fc, 216']
for i in selected:
    for label,name in [('bs',f'input_{i:03}.bs'),('rl',f'psx_{i:03}.rl')]:
        path=reference/name
        if label=='rl' and hashlib.sha256(path.read_bytes()).hexdigest()!=rows[i]['reference_sha256']:
            p.error('reference coefficients no longer match report')
        lines+=['.balign 4',f'fixture_{label}_{i}:',f'.incbin {json.dumps(str(path.relative_to(ROOT)))}']
lines+=['.balign 4','.global movie_vlc_fixture_count','movie_vlc_fixture_count:',
        f'.word {len(selected)}','.global movie_vlc_fixtures','movie_vlc_fixtures:']
for i in selected:
    lines+=[f'.word fixture_bs_{i}, {(reference/f"input_{i:03}.bs").stat().st_size}, '
            f'fixture_rl_{i}, {rows[i]["bytes"]}, {rows[i]["blocks"]}']
output.parent.mkdir(parents=True,exist_ok=True)
output.write_text('\n'.join(lines)+'\n')
output.with_suffix('.json').write_text(json.dumps([dict(index=i,**rows[i]) for i in selected],indent=2)+'\n')
print(f'{len(selected)} PSX coefficient fixtures -> {output}')
