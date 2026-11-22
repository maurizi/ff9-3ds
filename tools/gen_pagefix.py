#!/usr/bin/env python3
"""port/3ds/pagefix.h: for each PS1 polygon colour c (a channel, 0-255), the constants with which the PICA's combiners
give a texel of the VRAM mirror (5 bits a channel) times c exactly as the PS1 does: min(31, (t * c) >> 7), as 8 * that.

The PICA widens a 5-bit texel to 8 bits (t << 3 | t >> 2), multiplies with rounding ((a * b + 127) / 255, x 2), blends
at 8 bits and keeps the top 5: a texel times a colour comes out up to one step of 5 bits above the PS1's, in about
half the cases (measured on an Old 3DS and in Azahar with scenario key blendtest=, the same in all 8192 cells). Where
a frame is drawn from the one before (the field-to-battle swirl, 30 frames of it) that adds up to a picture far too
bright. With three more stages the product is brought to the PS1's 5 bits before it is blended:
    q = round(max(0, 2 * round(t8 * c' / 255) - k) * m / 255),  then  8 * q  (q + q, x 4)
and (c', k, m) searched here so that q is the PS1's value for every t. A second table gives a quarter of it (blending
mode 3, B + F/4, whose quarter the PS1 takes of the 5-bit product: added whole). No game data: arithmetic only."""
import sys
e8 = [(v << 3) | (v >> 2) for v in range(32)]
rnd = lambda x: (x + 127) // 255


def table(shift):
    rows, exact = [], 0
    for c in range(256):
        want = [min(31, (t * c) >> 7) >> shift for t in range(32)]
        best = None
        for cp in range(256):
            f = [min(255, 2 * rnd(e8[t] * cp)) for t in range(32)]
            for k in range(16):
                g = [max(0, x - k) for x in f]
                for m in range(4, 72):
                    q = [rnd(g[t] * m) for t in range(32)]
                    bad = sum(1 for t in range(32) if q[t] != want[t])
                    worst = max(abs(q[t] - want[t]) for t in range(32))
                    key = (bad, worst, abs(cp - c))
                    if best is None or key < best[0]:
                        best = (key, cp, k, m)
                if best[0][0] == 0:
                    break
            if best[0][0] == 0:
                break
        exact += best[0][0] == 0
        rows.append((best[1], best[2], best[3], best[0][0]))
    return rows, exact


def dithered():
    """{form, a, b, k}: with the PS1's dithering on, its 8-bit product min(255, (t * c) >> 4), to which the dither is
    added before the 5 bits are kept, as two products added: x = P(a) + P(b) - k (form 0) or 2 P(a) + P(b) - k (form 1),
    P(n) = round(t8 * n / 255) (the texel times the vertices' colour a, plus the texel times a constant b). One
    product, doubled, gives even values only, and a frame drawn from the one before settles on the values where that
    is one off. The pair with the fewest texel values off, then the least error."""
    import numpy as np
    t8 = np.array(e8)
    P = (t8[None, :] * np.arange(256)[:, None] + 127) // 255
    rows, exact = [], 0
    for c in range(256):
        want = np.minimum(255, (np.arange(32) * c) >> 4)
        best = None
        for form, X in ((0, P), (1, np.minimum(255, 2 * P))):
            S = np.minimum(255, X[:, None, :] + P[None, :, :])
            for k in range(3):
                E = np.abs(np.maximum(0, S - k) - want[None, None, :])
                bad = (E != 0).sum(axis=2) * 1000 + E.sum(axis=2)
                i = int(bad.argmin())
                key = (int(bad.flat[i]), form, k)
                if best is None or key < best[0]:
                    best = (key, form, i // 256, i % 256, k)
        exact += best[0][0] == 0
        rows.append((best[1], best[2], best[3], best[4], best[0][0] // 1000))
    return rows, exact


rows, exact = table(0)
rows4, exact4 = table(2)
rowsd, exactd = dithered()
with open(sys.argv[1] if len(sys.argv) > 1 else "port/3ds/pagefix.h", "w") as o:
    for name, rs, ex, what in (("pagefix", rows, exact, "8 * the PS1's product"),
                               ("pagefix4", rows4, exact4, "8 * a quarter of it (mode 3, B + F/4: added whole)")):
        o.write("/* tools/gen_pagefix.py: {c', k, m, texel values (of 32) not the PS1's} per polygon colour, for %s; %d of 256 exact */\n" % (what, ex))
        o.write("static const unsigned char %s[256][4] = {\n" % name)
        for i in range(0, 256, 8):
            o.write("    " + " ".join("{%d,%d,%d,%d}," % r for r in rs[i:i + 8]) + "\n")
        o.write("};\n")
    o.write("/* {form, a, b, k, texel values not the PS1's} with the PS1's dithering on (the 8-bit product as two products added, the\n"
            " * dither added, then the 5 bits kept: set_tev_for); %d of 256 exact */\n" % exactd)
    o.write("static const unsigned char pagefixd[256][5] = {\n")
    for i in range(0, 256, 8):
        o.write("    " + " ".join("{%d,%d,%d,%d,%d}," % r for r in rowsd[i:i + 8]) + "\n")
    o.write("};\n")
print("dithered:", exactd, "exact; worst", max(r[4] for r in rowsd), "values off; c 152:", rowsd[152])
print(exact, exact4, "exact; worst", max(r[3] for r in rows), max(r[3] for r in rows4), "values off")
