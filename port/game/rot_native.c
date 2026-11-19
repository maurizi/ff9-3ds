/* math_buildRotMatrixZYX (decomp src/main/tu_80030AF4.c: a rotation matrix from three angles, Z then Y then X; the
 * field models' bones, one per bone a frame: some 340 a VBlank at Lindblum's field 552) in port code. Mode 0 is the
 * game's C as it is; mode 1 worked its products out with four GPFs (gte_gpf12: IR0 x IR1-3 >> 12), here done in
 * place, the same products from the same values. The GTE is left as the four GPFs leave it: IR0 the last one's, MAC1-3
 * and IR1-3 the last GPF's results, the colour FIFO the last three GPFs' colours and FLAG the last GPF's (gte.c
 * psx_gte_gpf12's arithmetic). */
#include "psyq_port.h"
#include "ff9.h"
#include "port_hw.h"

MATRIX *math_buildRotMatrixZYX(SVECTOR *rot, MATRIX *mtx, s32 mode);

/* gte_gpf12 on (ir0; a, b, c): MAC (returned in m), its colour (rgb's low 24 bits) and flags */
static inline void rz_gpf(s32 ir0, s32 a, s32 b, s32 c, s32 m[3], u32 *rgb, u32 *flag) {
    const s32 v[3] = {(s16)a, (s16)b, (s16)c};
    u32 f = 0, col = 0;
    for (int i = 0; i < 3; i++) {
        const s32 x = (s16)ir0 * v[i] >> 12;
        m[i] = x;
        if (x > 0x7FFF || x < -0x8000) f |= 1u << (24 - i);
        s32 cc = x >> 4;
        if (cc > 0xFF) { cc = 0xFF; f |= 1u << (21 - i); } else if (cc < 0) { cc = 0; f |= 1u << (21 - i); }
        col |= (u32)cc << (8 * i);
    }
    *rgb = col;
    *flag = f;
}
static inline s32 rz_ir(s32 x) { return x > 0x7FFF ? 0x7FFF : x < -0x8000 ? -0x8000 : x; }

MATRIX *math_buildRotMatrixZYX(SVECTOR *rot, MATRIX *mtx, s32 mode) {
    s16 sx, cx, sy, cy, sz, cz;
    s32 x, y, z;
    s32 tx, ty, tz;
    s16 t1, t2;

    x = rot->vx & 0xFFF;
    y = rot->vy & 0xFFF;
    z = rot->vz & 0xFFF;
    ISIN(sx, rot->vx, x);
    tx = 0x400 - x;
    x = tx & 0xFFF;
    ISIN(cx, tx, x);
    if (y & 0x400) {
        sy = g_sinTable[0x400 - (y & 0x3FF)];
    } else {
        sy = g_sinTable[y & 0x3FF];
    }
    if (y & 0x800) sy = -sy;
    ty = 0x400 - y;
    y = ty & 0xFFF;
    ISIN(cy, ty, y);
    ISIN(sz, z, z);
    tz = 0x400 - z;
    z = tz & 0xFFF;
    ISIN(cz, tz, z);
    if (mode == 0) {
        t1 = (sy * cz) >> 12;
        t2 = (sy * sz) >> 12;
        mtx->m[0][0] = (cy * cz) >> 12;
        mtx->m[0][1] = (-sz * cx + t1 * sx) >> 12;
        mtx->m[0][2] = (sz * sx + t1 * cx) >> 12;
        mtx->m[1][0] = (cy * sz) >> 12;
        mtx->m[1][1] = (cz * cx + t2 * sx) >> 12;
        mtx->m[1][2] = (-cz * sx + t2 * cx) >> 12;
        mtx->m[2][0] = -sy;
        mtx->m[2][1] = (cy * sx) >> 12;
        mtx->m[2][2] = (cy * cx) >> 12;
        return mtx;
    }
    /* the game's steps: IR0..IR3 = (cy; cz, sz, sx), GPF; (cx; cy, sz, cz), GPF; (sy; czSx, szSx, cxCz), GPF; IR1, IR2 =
     * (cxCz, cxSz), GPF (IR3 the third GPF's) */
    s32 m1[3], m2[3], m3[3], m4[3];
    u32 c1, c2, c3, c4, f;
    mtx->m[2][0] = -sy;
    rz_gpf(cy, cz, sz, sx, m1, &c1, &f);
    s32 czSx = cz * sx;
    mtx->m[0][0] = m1[0];
    mtx->m[1][0] = m1[1];
    mtx->m[2][1] = m1[2];
    rz_gpf(cx, cy, sz, cz, m2, &c2, &f);
    s32 szSx = (sz * sx) >> 12;
    czSx >>= 12;
    const s32 cxSz = m2[1], cxCz = m2[2];
    mtx->m[2][2] = m2[0];
    rz_gpf(sy, czSx, szSx, cxCz, m3, &c3, &f);
    mtx->m[0][1] = m3[0] - cxSz;
    mtx->m[1][1] = m3[1] + cxCz;
    rz_gpf(sy, cxCz, cxSz, rz_ir(m3[2]), m4, &c4, &f);
    mtx->m[0][2] = m4[0] + szSx;
    mtx->m[1][2] = m4[1] - czSx;
    /* the GTE as the game's four GPFs leave it */
    const u32 code = gte_mfc2(6) & 0xFF000000u;
    gte_mtc2(8, (u32)(s32)sy);
    gte_mtc2(9, (u32)rz_ir(m4[0]));
    gte_mtc2(10, (u32)rz_ir(m4[1]));
    gte_mtc2(11, (u32)rz_ir(m4[2]));
    gte_mtc2(20, code | c2);
    gte_mtc2(21, code | c3);
    gte_mtc2(22, code | c4);
    gte_mtc2(25, (u32)m4[0]);
    gte_mtc2(26, (u32)m4[1]);
    gte_mtc2(27, (u32)m4[2]);
    gte_ctc2(31, f);
    return mtx;
}
