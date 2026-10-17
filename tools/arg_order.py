#!/usr/bin/env python3
"""List decomp calls whose order of argument evaluation could matter: two or more arguments with side effects
(++/--, assignment, or a call that isn't a known pure helper).

C leaves the order unspecified. The PS1's MIPS GCC, the DS's ARM GCC and the host's x86 GCC don't all agree, and a
call such as func_800384D0(func_80038150(), a, *g_evtCurObj->unk0++, 0) reads the script stream in both arguments:
x86 read the key byte first and found no target object; the PS1 (and ARM) read the target first (porting-notes).
Each hit needs its PS1 order checked in the asm and, if it matters, a counted patch that sequences it.

  python3 tools/arg_order.py [decomp]
"""
import re
import sys
from pathlib import Path

PURE = {'sizeof', 'PSX_ADDR', 'PSX_PTR', 'PSX_HW', 'ABS', 'MIN', 'MAX', 'math_sin', 'math_cos', 'rsin', 'rcos',
        'ratan2', 'SquareRoot0', 'SquareRoot12'}
CALL = re.compile(r'\b([A-Za-z_]\w*)\s*\(')


def split_args(text):
    args, depth, cur = [], 0, ''
    for ch in text:
        if ch in '([{':
            depth += 1
        elif ch in ')]}':
            depth -= 1
        if ch == ',' and depth == 0:
            args.append(cur)
            cur = ''
        else:
            cur += ch
    args.append(cur)
    return args


def side_effect(arg):
    a = re.sub(r'"(\\.|[^"])*"', '', arg)
    if '++' in a or '--' in a:
        return True
    if re.search(r'(?<![=!<>])=(?!=)', a):
        return True
    for name in CALL.findall(a):
        if name not in PURE and name not in ('if', 'while', 'for', 'switch', 'return'):
            return True
    return False


def calls(code):
    """(name, args text, offset) for every call, parentheses balanced."""
    for m in CALL.finditer(code):
        name = m.group(1)
        if name in ('if', 'while', 'for', 'switch', 'return', 'sizeof'):
            continue
        i, depth = m.end(), 1
        while i < len(code) and depth:
            depth += code[i] == '('
            depth -= code[i] == ')'
            i += 1
        yield name, code[m.end():i - 1], m.start()


def main():
    root = Path(sys.argv[1] if len(sys.argv) > 1 else 'decomp')
    hits = 0
    for path in sorted(list((root / 'src').rglob('*.c')) + list((root / 'src_equiv').rglob('*.c'))):
        code = path.read_text(errors='replace')
        code = re.sub(r'/\*.*?\*/', lambda m: re.sub(r'[^\n]', ' ', m.group(0)), code, flags=re.S)
        code = re.sub(r'//[^\n]*', '', code)
        for name, args, at in calls(code):
            parts = split_args(args)
            if len(parts) >= 2 and sum(side_effect(p) for p in parts) >= 2:
                line = code.count('\n', 0, at) + 1
                hits += 1
                print(f'{path.relative_to(root)}:{line}: {name}({" ".join(args.split())[:110]})')
    print(f'{hits} calls', file=sys.stderr)


if __name__ == '__main__':
    main()
