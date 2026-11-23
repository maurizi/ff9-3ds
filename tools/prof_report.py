#!/usr/bin/env python3
"""tools/prof_report.py SAMPLES [ELF]: the PC build's --prof samples by function (addr2line): where the program was,
and for the commonest places, who called them (the first address on the stack's top that is in the program's code)."""
import collections, subprocess, sys
elf = sys.argv[2] if len(sys.argv) > 2 else "build/pc/ff9ds"
rows = [[int(x, 16) for x in l.split()] for l in open(sys.argv[1]) if l.strip()]
addrs = sorted({a for r in rows for a in r})
out = subprocess.run(["addr2line", "-f", "-e", elf] + [hex(a) for a in addrs], capture_output=True, text=True).stdout.split("\n")
name = {a: out[2 * i] for i, a in enumerate(addrs)}
leaf = collections.Counter(name[r[0]] for r in rows)
pair = collections.Counter()
for r in rows:
    caller = next((name[a] for a in r[1:] if name[a] != "??" and name[a] != name[r[0]]), "?")
    pair[(name[r[0]], caller)] += 1
n = len(rows)
print(n, "samples")
for f, c in leaf.most_common(30):
    callers = sorted(((k[1], v) for k, v in pair.items() if k[0] == f), key=lambda x: -x[1])[:3]
    print(f"{100 * c / n:5.1f}%  {f}   <- " + ", ".join(f"{a} {100 * b / c:.0f}%" for a, b in callers))
