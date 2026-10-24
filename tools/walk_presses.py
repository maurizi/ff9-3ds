#!/usr/bin/env python3
"""Pseudo-random walking input for the host build's --press, to play a field past its entry: hold a direction for
0.5-1.5 s at a time, press Cross every 0.75 s (talk, read, confirm), and Circle three times every 10 s (back out of
whatever opened). No Start or Triangle. Deterministic for a seed.

    tools/walk_presses.py FROM_FRAME TO_FRAME [SEED]   -> --press arguments, one per line
"""
import random
import sys


def main():
    start, end = int(sys.argv[1]), int(sys.argv[2])
    rng = random.Random(int(sys.argv[3]) if len(sys.argv) > 3 else 1)
    out = []
    f = start
    while f < end:
        hold = rng.randint(30, 90)
        out.append(f"{rng.choice(['UP', 'DOWN', 'LEFT', 'RIGHT'])}@{f}+{hold}")
        f += hold + rng.randint(0, 10)
    for f in range(start + 20, end, 45):
        out.append(f"CROSS@{f}+2")
    for f in range(start + 300, end, 600):
        out += [f"CIRCLE@{f}+2", f"CIRCLE@{f + 8}+2", f"CIRCLE@{f + 16}+2"]
    print("\n".join(out))


main()
