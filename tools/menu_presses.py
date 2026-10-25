#!/usr/bin/env python3
"""Pseudo-random menu input for the host build's --press: open the main menu (Triangle) every 450 frames, wander its
screens for about 300 (a direction or Cross every 12 frames, Circle now and then), then back out with Circle. Opens
shops' and save points' screens too when a field has them under Cross. Deterministic for a seed.

    tools/menu_presses.py FROM_FRAME TO_FRAME [SEED]   -> --press arguments, one per line
"""
import random
import sys


def main():
    start, end = int(sys.argv[1]), int(sys.argv[2])
    rng = random.Random(int(sys.argv[3]) if len(sys.argv) > 3 else 1)
    out = []
    for f0 in range(start, end, 450):
        out.append(f"TRIANGLE@{f0}+2")
        f = f0 + 30
        while f < min(f0 + 330, end):
            out.append(f"{rng.choice(['UP', 'DOWN', 'LEFT', 'RIGHT', 'CROSS', 'CROSS', 'CIRCLE', 'R1', 'L1'])}@{f}+2")
            f += 12
        for k in range(6):
            out.append(f"CIRCLE@{min(f0 + 340 + 12 * k, end - 1)}+2")
    print("\n".join(out))


main()
