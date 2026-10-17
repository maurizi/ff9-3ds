#!/usr/bin/env python3
"""Extract the production pitch function/call expressions for a disc-free ABI test.

Only C code is generated under build/; test tables and expected values are
synthetic. Separate translation units preserve the actual call boundary.
"""
import argparse
from pathlib import Path
import re

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('source', type=Path)
p.add_argument('out', type=Path)
a = p.parse_args()
s = a.source.read_text()
function = re.search(r'^u16 akao_calcPitch\([^\n]*\) \{.*?^}', s, re.M | re.S)
if not function:
    p.error('pitch function not found')
calls = re.findall(r'^[ \t]+(?:r|vel) = .*akao_calcPitch.*?;', s, re.M)
if len(calls) != 3:
    p.error(f'expected three production pitch call expressions, found {len(calls)}')
prefix = '#define M2CTX\n#include "ff9.h"\n'
a.out.mkdir(parents=True, exist_ok=True)
(a.out/'akao_pitch_callee.c').write_text(prefix + function.group(0) + '\n')
caller = r'''
#include <assert.h>
#include <stdio.h>
#undef NULL
#define M2CTX
#define printf ff9_game_printf
#include "ff9.h"
#undef printf
u32 g_akaoSemitonePitch[12] = {4096,4096,4096,4096,4096,4096,4096,4096,4096,4096,4096,4096};
Unk8005EBB4 D_8007F970[1];
int akao_pitch_abi_tests(void) {
    static const s16 fine[] = {-32768,-129,-128,-1,0,1,127,128,255,256,32767};
    static const s32 expected[] = {0,4064,-2048,-16,0,32,4064,-2048,-16,0,-16};
    for (unsigned i=0;i<sizeof fine/sizeof *fine;++i) {
        AkaoChannel storage={0},*c=&storage;
        AkaoDrumKey drum={0},*e=&drum;
        Unk8005EBB4 *p=&D_8007F970[0];
        s32 op=0,r=0;u32 vel=0,tmp=0;
        c->unk108=fine[i];
'''
for i, call in enumerate(calls):
    caller += call + '\n'
    if i == 0:
        caller += '        assert(r==4096 && c->unk84==(u32)expected[i]);\n'
    elif i == 1:
        caller += '        assert(vel==4096 && c->unk84==(u32)expected[i]);\n'
    else:
        caller += '        assert(vel==0x10000000 && tmp==(u32)expected[i]);\n'
caller += '''    }
    puts("AKAO pitch ABI: all 3 callers / signed fine bytes PASS");return 0;
}
#ifndef ARM9
int main(void) {return akao_pitch_abi_tests();}
#endif
'''
(a.out/'akao_pitch_caller.c').write_text(caller)
