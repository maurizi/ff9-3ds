#!/usr/bin/env python3
"""List decomp functions whose definition takes an 8- or 16-bit parameter that some declaration in another
header (or an implicit declaration) declares as a wider type.

On MIPS and x86 the callee narrows such an argument itself, so a caller compiled against the wide prototype
still works. ARM AAPCS makes the caller narrow it and the callee trusts it: a caller passing -3 for a u8 leaves
0xfffffffd in the callee's "u8" (func_8005593C, AKAO arguments). Each hit needs the definition's narrowing
applied at the boundary; see porting-notes.md.

  python3 tools/narrow_args.py [decomp]
"""
import re
import sys
from pathlib import Path

NARROW = {'u8', 's8', 'u16', 's16', 'char', 'unsigned char', 'signed char', 'short', 'unsigned short',
          'u_char', 'u_short', 'uchar', 'ushort'}
FUNC = re.compile(r'^(?:static\s+)?(?:inline\s+)?[A-Za-z_][\w\s\*]*?\b(\w+)\s*\(([^;{}()]*)\)\s*(\{|;)', re.M)


def params(text):
    text = text.strip()
    if text in ('', 'void'):
        return []
    out = []
    for p in text.split(','):
        p = re.sub(r'\b(const|volatile|register)\b', '', p).strip()
        p = re.sub(r'\[.*?\]', '*', p)
        if '*' in p:
            out.append('ptr')
            continue
        words = p.split()
        # drop the parameter name when there is one (more than one word, last not a type keyword)
        if len(words) > 1 and words[-1] not in ('int', 'char', 'short', 'long', 'u8', 's8', 'u16', 's16', 's32', 'u32'):
            words = words[:-1]
        out.append(' '.join(words))
    return out


def main():
    root = Path(sys.argv[1] if len(sys.argv) > 1 else 'decomp')
    defs, decls = {}, {}
    for path in list((root / 'src').rglob('*.c')) + list((root / 'include').rglob('*.h')):
        text = path.read_text(errors='replace')
        for m in FUNC.finditer(text):
            name, args, end = m.group(1), m.group(2), m.group(3)
            if name in ('if', 'while', 'for', 'switch', 'return', 'sizeof'):
                continue
            entry = (str(path.relative_to(root)), params(args))
            (defs if end == '{' else decls).setdefault(name, []).append(entry)
    hits = 0
    for name, where in sorted(defs.items()):
        for dfile, dparams in where:
            narrow = [i for i, t in enumerate(dparams) if t in NARROW]
            if not narrow:
                continue
            for hfile, hparams in decls.get(name, []):
                if len(hparams) != len(dparams):
                    continue
                wide = [i for i in narrow if hparams[i] not in NARROW and hparams[i] != 'ptr']
                if wide:
                    hits += 1
                    print(f'{name}: {dfile} ({", ".join(dparams)}) vs {hfile} ({", ".join(hparams)}) '
                          f'params {wide}')
    print(f'{hits} mismatched declarations', file=sys.stderr)


if __name__ == '__main__':
    main()
