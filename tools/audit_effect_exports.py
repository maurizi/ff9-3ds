#!/usr/bin/env python3
"""Compare effect export pointer masks with the linked native DWARF signatures.

Read-only: does not run the game or modify masks. C scalar arguments can carry
data/code pointers, so a clean result cannot establish complete ABI coverage.
Requires a generated overlay image and a matching debug ELF, plus Python GDB.
"""
import argparse
import json
from pathlib import Path
import re
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--elf', type=Path, default=Path('build/pc/ff9ds.elf'))
    parser.add_argument('--image-asm', type=Path, default=Path('build/gen/img_ovl_0b.s'))
    parser.add_argument('--bridge', type=Path, default=Path('port/game/effect_vm.c'))
    parser.add_argument('--scalar-source', type=Path, default=Path('decomp/src/ovl_0b/ovl_0b_2.c'))
    args = parser.parse_args()
    words = []
    for line in args.image_asm.read_text().splitlines():
        if line.strip().startswith('.4byte '):
            words.extend(part.strip() for part in line.strip()[7:].split(','))
    starts = [i for i in range(len(words)-2)
              if words[i:i+3] == ['LoadImage', 'StoreImage', 'o0b_func_800E0014']]
    if len(starts) != 1:
        parser.error('cannot identify the 216-function effect export table')
    names = words[starts[0]:starts[0]+216]
    if len(names) != 216 or any(not re.fullmatch(r'[A-Za-z_]\w*', name) for name in names):
        parser.error('unexpected effect export table contents')
    source = args.bridge.read_text().split('pointer_mask[216]={', 1)[1].split('};', 1)[0]
    masks = {int(offset, 0): int(mask, 0) for offset, mask in
             re.findall(r'\[(0x[0-9a-fA-F]+|\d+)/4\]\s*=\s*(0x[0-9a-fA-F]+|\d+)', source)}
    code = '''import gdb,json
rows=[]
for name in NAMES:
 row={'symbol':name}
 try:
  value=gdb.parse_and_eval(name)
  t=value.type.strip_typedefs()
  if str(t).startswith('<'):
   block=gdb.block_for_pc(int(gdb.parse_and_eval('&'+name))&~1)
   expected=name[4:] if name.startswith('o0b_') else name
   while block:
    if block.function and str(block.function)==expected: break
    block=block.superblock
   if not block: raise ValueError('no matching debug signature')
   t=block.function.type.strip_typedefs()
   row['debug_symbol']=str(block.function)
  if t.code==gdb.TYPE_CODE_PTR: t=t.target().strip_typedefs()
  if t.code!=gdb.TYPE_CODE_FUNC: raise ValueError('not a function type')
  args=t.fields(); pointers=callbacks=0
  row['parameters']=[str(f.type) for f in args]
  for i,f in enumerate(args):
   a=f.type.strip_typedefs()
   if a.code==gdb.TYPE_CODE_PTR:
    if a.target().strip_typedefs().code==gdb.TYPE_CODE_FUNC: callbacks|=1<<i
    else: pointers|=1<<i
  row.update(data_pointer_mask=pointers,callback_mask=callbacks,
             return_type=str(t.target().strip_typedefs()))
 except Exception as exc: row['unresolved']=str(exc)
 rows.append(row)
print('EXPORT_SIGNATURES='+json.dumps(rows))
'''.replace('NAMES', repr(names))
    result = subprocess.check_output(['gdb', '-nx', '-q', '-batch',
             '-iex', 'set auto-load off', '-iex', 'set debuginfod enabled off',
             '--se', str(args.elf), '-ex', 'python exec('+repr(code)+')'], text=True)
    rows = json.loads(next(line.removeprefix('EXPORT_SIGNATURES=') for line in result.splitlines()
                           if line.startswith('EXPORT_SIGNATURES=')))
    # The libc ELF has no printf DWARF. Its fixed stdio.h parameter is known;
    # pointer-valued variadic arguments still require separate call-site review.
    # Empty/constant-return wrappers can lose their function DIE completely.
    # These source-reviewed signatures contain no data or function pointers.
    scalar_wrappers = {name: parameters for name, parameters in (
        ('o0b_func_800D8CAC', []), ('o0b_func_800E738C', ['s32']),
        ('o0b_func_800DF938', []), ('o0b_func_800DF928', []),
        ('o0b_func_800DF920', []), ('o0b_func_800DF930', []))}
    scalar_source = args.scalar_source.read_text()
    scalar_pointer_masks = {'o0b_Hi_SplitMdlVertex': 4}
    for row in rows:
        if row['symbol'] == 'printf' and 'unresolved' in row:
            del row['unresolved']
            row.update(parameters=['const char *', '...'], data_pointer_mask=1,
                       callback_mask=0, signature_source='stdio.h; variadic tail unaudited')
        elif row['symbol'] in scalar_wrappers and 'unresolved' in row:
            parameters = scalar_wrappers[row['symbol']]
            declaration = re.search(r'\b(?:void|s32)\s+'+re.escape(row['symbol'][4:])+
                                    r'\s*\(([^)]*)\)\s*\{', scalar_source)
            expected = r'\s*s32\s+\w+\s*' if parameters else r'\s*(?:void)?\s*'
            if declaration and re.fullmatch(expected, declaration[1]):
                del row['unresolved']
                row.update(parameters=parameters, data_pointer_mask=0,
                           callback_mask=0, signature_source='reviewed scalar wrapper; declaration checked')
    for i, row in enumerate(rows):
        mask = masks.get(i*4, 0)
        row.update(offset=hex(i*4), bridge_mask=mask)
        if 'data_pointer_mask' in row:
            reviewed = scalar_pointer_masks.get(row['symbol'], 0)
            row['reviewed_scalar_pointer_mask'] = reviewed
            required = row['data_pointer_mask'] | reviewed
            row['missing_data_bits'] = required & ~mask
            row['additional_bridge_bits'] = mask & ~required
        if row['symbol'] in ('o0b_Hi_GetMdlVertexPtr', 'o0b_func_800DD0B0', 'o0b_func_800DD0DC') and row.get('return_type') != 'unsigned int':
            row['return_contract_error'] = 'effect modules consume the forwarded count as a full 32-bit result'
    print(json.dumps({'elf': str(args.elf), 'scope': __doc__, 'exports': rows,
                      'missing': [r for r in rows if r.get('missing_data_bits')],
                      'unresolved': [r for r in rows if 'unresolved' in r],
                      'return_contract_errors': [r for r in rows if 'return_contract_error' in r]}, indent=2))
    return int(any(r.get('missing_data_bits') or 'unresolved' in r or 'return_contract_error' in r for r in rows))


if __name__ == '__main__':
    raise SystemExit(main())
