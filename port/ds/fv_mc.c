/* FastVideoDS motion compensation on the CPU (fv_mc.h): the prediction the 3D engine draws from a P-frame's display
 * list, made without the 3D engine or texture VRAM, for movies behind a field (docs/movie-validation.md).
 *
 * The list draws each 8x8 block twice from the reference: list A at the block's vector rounded down, list B rounded
 * up (in half pixels: a whole-pixel vector gives the same point twice), B blended over A at alpha 15/31. Per
 * channel that is (a6 + b6) >> 2 in the 3D engine's 6-bit colours, a6 = 2a + (a != 0) from 5-bit a, which the
 * capture keeps as 5 bits; samples outside the frame clamp to its edge, as the encoder predicts them. The texcoords
 * the vector decoder wrote into the lists give the two points: 1/16 texels, the bottom half's t offset by the rows
 * its texture starts above the frame. */
#include <nds.h>
#include "fv_mc.h"

#define FIELDS 0x1F07C1Fu /* r, g, b at bits 0, 10, 20 */

static inline u32 spread(u32 p) {
    return (p & 0x1F) | (p & 0x3E0) << 5 | (p & 0x7C00) << 10;
}

/* 2c + (c != 0), each field */
static inline u32 six(u32 s) {
    return (s << 1) + (((s + FIELDS) >> 5) & 0x100401u);
}

static inline u16 blend(u32 a, u32 b) {
    u32 f = ((six(spread(a)) + six(spread(b))) >> 2) & FIELDS;
    return (u16)(0x8000 | (f & 0x1F) | (f >> 5 & 0x3E0) | (f >> 10 & 0x7C00));
}

/* Pixels 2i and 2i+1 of a row as a word: `q` the word-aligned address at or below the row's first pixel, `odd`
 * whether that pixel is the upper half of q[0]. */
static inline __attribute__((always_inline)) u32 pair(const u32 *q, int odd, int i) {
    return odd ? q[i] >> 16 | q[i + 1] << 16 : q[i];
}

/* An inside block's blended rows, for one alignment of each source (constants: no tests in the loop). */
static inline __attribute__((always_inline)) void blend_rows(const u32 *qa, int odda, const u32 *qb, int oddb,
                                                             u16 *out) {
    for (int r = 0; r < 8; ++r, qa += FV_MC_STRIDE / 2, qb += FV_MC_STRIDE / 2, out += FV_MC_STRIDE) {
        u32 *o = (u32 *)out;
        for (int i = 0; i < 4; ++i) {
            u32 x = pair(qa, odda, i) | 0x80008000u;
            u32 y = pair(qb, oddb, i) | 0x80008000u;
            /* a zero 5-bit field in either (fields are adjacent: a borrow only follows a zero field) */
            u32 zero = ((x - 0x04210421u) & ~x) | ((y - 0x04210421u) & ~y);
            if (__builtin_expect(zero & 0x42104210u, 0))
                o[i] = blend(x & 0xFFFF, y & 0xFFFF) | (u32)blend(x >> 16, y >> 16) << 16;
            else
                o[i] = (x | y) - (((x ^ y) & 0x7BDE7BDEu) >> 1);
        }
    }
}

static inline int clampi(int v, int lo, int hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

static void block(const u16 *ref, u16 *dst, int bx, int by, int ax, int ay, int bx2, int by2, int h) {
    const int x0 = bx * 8, y0 = by * 8;
    u16 *out = dst + y0 * FV_MC_STRIDE + x0;
    if (ax == bx2 && ay == by2) { /* whole pixels: a copy */
        int sx = x0 + ax, sy = y0 + ay;
        if (sx >= 0 && sx + 8 <= FV_MC_WIDTH && sy >= 0 && sy + 8 <= h) {
            const u16 *src = ref + sy * FV_MC_STRIDE + sx;
            const u32 *q = (const u32 *)((u32)src & ~3u);
            if ((u32)src & 2)
                for (int r = 0; r < 8; ++r, q += FV_MC_STRIDE / 2, out += FV_MC_STRIDE) {
                    u32 *o = (u32 *)out;
                    for (int i = 0; i < 4; ++i) o[i] = pair(q, 1, i) | 0x80008000u;
                }
            else
                for (int r = 0; r < 8; ++r, q += FV_MC_STRIDE / 2, out += FV_MC_STRIDE) {
                    u32 *o = (u32 *)out;
                    for (int i = 0; i < 4; ++i) o[i] = q[i] | 0x80008000u;
                }
            return;
        }
        for (int r = 0; r < 8; ++r, out += FV_MC_STRIDE) {
            const u16 *row = ref + clampi(y0 + ay + r, 0, h - 1) * FV_MC_STRIDE;
            for (int c = 0; c < 8; ++c) out[c] = row[clampi(x0 + ax + c, 0, FV_MC_WIDTH - 1)] | 0x8000;
        }
        return;
    }
    int ax0 = x0 + ax, ay0 = y0 + ay, bx0 = x0 + bx2, by0 = y0 + by2;
    if (ax0 >= 0 && bx0 >= 0 && ax0 + 8 <= FV_MC_WIDTH && bx0 + 8 <= FV_MC_WIDTH && ay0 >= 0 && by0 >= 0 &&
        ay0 + 8 <= h && by0 + 8 <= h) {
        /* Inside the frame. With no channel 0 on either side, (a6 + b6) >> 2 is the average rounded up,
         * (x | y) - ((x ^ y) >> 1) per 5-bit field, two pixels a word (bit 15 set in both stays set); a pair with a
         * zero channel takes the exact blend. */
        const u16 *ra = ref + ay0 * FV_MC_STRIDE + ax0, *rb = ref + by0 * FV_MC_STRIDE + bx0;
        const u32 *qa = (const u32 *)((u32)ra & ~3u), *qb = (const u32 *)((u32)rb & ~3u);
        switch (((u32)ra & 2) | ((u32)rb & 2) >> 1) {
        case 0: blend_rows(qa, 0, qb, 0, out); break;
        case 1: blend_rows(qa, 0, qb, 1, out); break;
        case 2: blend_rows(qa, 1, qb, 0, out); break;
        default: blend_rows(qa, 1, qb, 1, out); break;
        }
        return;
    }
    for (int r = 0; r < 8; ++r, out += FV_MC_STRIDE) {
        const u16 *ra = ref + clampi(ay0 + r, 0, h - 1) * FV_MC_STRIDE;
        const u16 *rb = ref + clampi(by0 + r, 0, h - 1) * FV_MC_STRIDE;
        for (int c = 0; c < 8; ++c)
            out[c] = blend(ra[clampi(ax0 + c, 0, FV_MC_WIDTH - 1)], rb[clampi(bx0 + c, 0, FV_MC_WIDTH - 1)]);
    }
}

void fv_mc_predict(const u16 *ref, u16 *dst, const u32 *list_a, const u32 *list_b, int h) {
    const int rows = h / 8, top = h / 16, shift = 256 - h;
    const u32 *a = list_a, *b = list_b; /* each block's texcoord word: s (1/16 texel) low, t high */
    for (int by = 0; by < rows; ++by) {
        if (by == top) { a += 3; b += 3; } /* (the lists' texture switch between the halves) */
        const int off = by >= top ? shift : 0;
        for (int bx = 0; bx < FV_MC_WIDTH / 8; ++bx, a += 5, b += 5) {
            const int as = (s16)(*a & 0xFFFF) >> 4, at = ((s16)(*a >> 16) >> 4) - off;
            const int bs = (s16)(*b & 0xFFFF) >> 4, bt = ((s16)(*b >> 16) >> 4) - off;
            block(ref, dst, bx, by, as, at, bs, bt, h);
        }
    }
}
