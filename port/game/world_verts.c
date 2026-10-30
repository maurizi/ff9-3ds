/* The world map's vertex pass (ovl_0c's hand-written routines; decomp src_equiv/ovl_0c/world_verts.c) in port code
 * (they are weak there): the same steps, except that the first RTPT of each group, read only for its depths (the
 * second overwrites the screen positions, MAC/IR and MAC0/IR0 before anything reads them), pushes the depths alone
 * in DS mode (psx_gte_rtpt_depths): 1.3-1.4 ms per VBlank of a world map's frame. */
#include "psyq_port.h"
#include "ovl_0c.h"
#include "port_hw.h"
#include "native_mesh.h"

extern SVECTOR D_800CD794[]; /* normals, indexed by the low byte of a vertex's pad */
extern u32 D_800D1FF0[];     /* per-vertex fog colours */
extern u32 D_800D23F0[];     /* per-vertex lit colours */
extern u16 D_800D29F8[];     /* per-vertex depth: u32 SZ or u16 SZ / 8, depending on the routine */

#ifdef PORT_GTE_APPROX
#define WV_RTPT_DEPTHS() psx_gte_rtpt_depths()
#else
#define WV_RTPT_DEPTHS() gte_rtpt()
#endif

void o0c_wm_blockVertsFog(void *o);
void o0c_wm_modelVerts(s32 o);
void o0c_wm_modelVertsFlat(s32 o);
void o0c_wm_blockVertsFar(void *o);
void o0c_wm_blockVertsNear(void *o);

#define WV_FOG 1        /* DPCS lit + fog colours (else: NCT colour to the lit colours only) */
#define WV_DEPTH_HALF 2 /* depth factor (sz - 0x8000 + ofs) / 2 instead of / 4 */
#define WV_CURVE_MUL 4  /* bend y with a multiply */
#define WV_CURVE_GTE 8  /* bend y with INTPL */
#define WV_SZ16 16      /* store SZ / 8 as u16 instead of SZ as u32 */

static inline s32 wv_depth(u32 sz, u32 ofs, s32 flags) {
    s32 p = (s32)(sz - 0x8000 + ofs) >> ((flags & WV_DEPTH_HALF) ? 1 : 2);

    if (p <= 0) {
        p = 0;
    }
    if (p > 0x1000) {
        p = 0x1000;
    }
    return p;
}

/* attr: the vertex's vz/pad word; bit 25 (pad bit 9) picks g_worldFogParams[0] or [1]. */
static inline void wv_fog(u32 rgb, u32 sz, u32 attr, s32 flags, u32 *lit, u32 *fog) {
    u32 *e = (u32 *)g_worldFogParams + ((attr >> 25) & 1) * 2;
    u32 col = e[0];
    u32 ofs = e[1];
    u32 c;

    gte_ldrgbc(rgb);
    gte_lddp(wv_depth(sz, ofs >> 16, flags));
    gte_dpcs();
    gte_strgb2(c);
    *lit = c;
    gte_ldrgbc(col);
    gte_lddp(0x1000 - wv_depth(sz, ofs & 0xFFFF, flags));
    gte_dpcs();
    gte_strgb2(c);
    *fog = c;
}

#ifdef PORT_GTE_APPROX
/* DS mode: a group's GTE steps as arithmetic (each the GTE's: NCT's two products and DPCS's and INTPL's
 * interpolation in 64-bit sums, exact as gte.c's; the RTPTs as DS mode's 32-bit ones, psx_gte_project and
 * psx_gte_rtpt_depths), for every group but the last, which wv_transform runs through the GTE so the registers end
 * as the game leaves them. The one register a group reads from the one before is RGBC (the colour code NCT pushes
 * with: the last fog colour loaded). */
static inline s32 wv_clamp(long long v, s32 lo, s32 hi) { return v < lo ? lo : v > hi ? hi : (s32)v; }
/* M x V for one of the GTE's matrices (rotation layout: 5 words from c) */
static inline void wv_mat(const u32 *c, s32 x, s32 y, s32 z, long long *a, long long *b, long long *d) {
    *a = (long long)(s16)c[0] * x + (long long)((s32)c[0] >> 16) * y + (long long)(s16)c[1] * z;
    *b = (long long)((s32)c[1] >> 16) * x + (long long)(s16)c[2] * y + (long long)((s32)c[2] >> 16) * z;
    *d = (long long)(s16)c[3] * x + (long long)((s32)c[3] >> 16) * y + (long long)(s16)c[4] * z;
}
/* NCT's colour for one normal (sf 1, lm 1), pushed with the code byte */
static inline u32 wv_nct(const SVECTOR *nv, u32 code) {
    const u32 *c = psx_gte.c;
    long long a, b, d;
    wv_mat(c + 8, nv->vx, nv->vy, nv->vz, &a, &b, &d); /* LLM x V */
    const s32 i1 = wv_clamp(a >> 12, 0, 0x7FFF), i2 = wv_clamp(b >> 12, 0, 0x7FFF), i3 = wv_clamp(d >> 12, 0, 0x7FFF);
    wv_mat(c + 16, i1, i2, i3, &a, &b, &d); /* BK + LCM x IR */
    const s32 m1 = (s32)((((long long)(s32)c[13] << 12) + a) >> 12), m2 = (s32)((((long long)(s32)c[14] << 12) + b) >> 12);
    const s32 m3 = (s32)((((long long)(s32)c[15] << 12) + d) >> 12);
    return code << 24 | (u32)wv_clamp(m3 >> 4, 0, 0xFF) << 16 | (u32)wv_clamp(m2 >> 4, 0, 0xFF) << 8 |
           (u32)wv_clamp(m1 >> 4, 0, 0xFF);
}
/* DPCS (sf 1, lm 0) of colour rgbc toward the far colour by dp */
static inline u32 wv_dpcs(u32 rgbc, s32 dp) {
    const u32 *c = psx_gte.c;
    u32 out = rgbc & 0xFF000000u;
    for (int k = 0; k < 3; ++k) {
        const long long m = (long long)(rgbc >> (8 * k) & 0xFF) << 16;
        const s32 ir = wv_clamp((((long long)(s32)c[21 + k] << 12) - m) >> 12, -0x8000, 0x7FFF);
        const s32 mac = (s32)(((long long)ir * dp + m) >> 12);
        out |= (u32)wv_clamp(mac >> 4, 0, 0xFF) << (8 * k);
    }
    return out;
}
/* one group's results; *rgbc: RGBC as the GTE would hold it, before and after */
static inline void wv_group(const u32 *v, s32 flags, u32 *rgbc, u32 *lit, u32 *fog, u32 *sxy, u32 *zs32, u16 *zs16) {
    const u32 *c = psx_gte.c;
    u32 w[6], sz[3], rgb[3];
    for (int i = 0; i < 6; i++) w[i] = v[i];
    const s32 trz = (s32)c[7], c3 = (s32)c[3], c4 = (s16)c[4];
    for (int i = 0; i < 3; ++i) { /* (the first RTPT's depths) */
        const u32 vxy = w[2 * i];
        const s32 mac3 = trz + (((s16)c3 * (s16)vxy + (c3 >> 16) * ((s32)vxy >> 16) + c4 * (s16)w[2 * i + 1]) >> 12);
        sz[i] = mac3 > 0xFFFF ? 0xFFFF : mac3 < 0 ? 0 : (u32)mac3;
    }
    for (int i = 0; i < 3; ++i) rgb[i] = wv_nct(&D_800CD794[(w[2 * i + 1] >> 16) & 0xFF], *rgbc >> 24);
    if (flags & WV_FOG) {
        for (int i = 0; i < 3; ++i) {
            const u32 *e = (const u32 *)g_worldFogParams + ((w[2 * i + 1] >> 25) & 1) * 2;
            lit[i] = wv_dpcs(rgb[i], wv_depth(sz[i], e[1] >> 16, flags));
            fog[i] = wv_dpcs(e[0], 0x1000 - wv_depth(sz[i], e[1] & 0xFFFF, flags));
            *rgbc = e[0];
        }
    } else {
        lit[0] = rgb[0]; lit[1] = rgb[1]; lit[2] = rgb[2];
    }
    if (flags & WV_CURVE_MUL) {
        for (int i = 0; i < 3; i++) {
            const u32 f = 0x1000 - ((sz[i] >> 4) + (sz[i] >> 6));
            s32 y = (s32)(w[2 * i] & 0xFFFF0000) >> 16;
            y = (s32)((u32)y * f) >> 12;
            w[2 * i] = (w[2 * i] & 0xFFFF) | ((u32)y << 16);
        }
    }
    if (flags & WV_CURVE_GTE) { /* (INTPL with the far colour's red 0: MAC1 = (clamp(-y) x IR0 + y << 12) >> 12) */
        for (int i = 0; i < 3; i++) {
            const s32 y = (s32)w[2 * i] >> 16, dp = (s16)((sz[i] >> 4) + (sz[i] >> 6));
            const s32 ir = wv_clamp(-(long long)y, -0x8000, 0x7FFF);
            const u32 m = (u32)(s32)(((long long)ir * dp + ((long long)y << 12)) >> 12);
            w[2 * i] = (w[2 * i] & 0xFFFF) | m << 16;
        }
    }
    for (int i = 0; i < 3; ++i) {
        u32 mac0;
        psx_gte_project(w[2 * i], w[2 * i + 1], &sxy[i], &sz[i], &mac0);
    }
    if (flags & WV_SZ16) { zs16[0] = (u16)(sz[0] >> 3); zs16[1] = (u16)(sz[1] >> 3); zs16[2] = (u16)(sz[2] >> 3); }
    else { zs32[0] = sz[0]; zs32[1] = sz[1]; zs32[2] = sz[2]; }
}
#endif

#if defined(__arm__)
__attribute__((target("arm"))) /* (its 64-bit products: Thumb makes each a call) */
#endif
PORT_OVERLAY_CODE(0c) static void wv_transform(u8 *o, s32 flags) {
    s32 n = *(u32 *)o & 0xFF;
    u32 *v = *(u32 **)(o + 4);
    u32 *fog = D_800D1FF0;
    u32 *lit = D_800D23F0;
    u32 *sxy = (u32 *)SCRATCHPAD;
    u32 *zs32 = (u32 *)D_800D29F8;
    u16 *zs16 = D_800D29F8;
    u32 w[6]; /* three vertices as raw words: w[2i] = x | y << 16, w[2i + 1] = z | pad << 16 */
    u32 sz[3];
    u32 rgb[3];
    u32 f, m, rfc;
    s32 i, y;

#ifdef PORT_GTE_APPROX
    { /* all groups but the last as arithmetic (wv_group) */
        u32 rgbc = psx_gte.d[6];
        while (n > 3) {
            wv_group(v, flags, &rgbc, lit, fog, sxy, zs32, zs16);
            v += 6; n -= 3; lit += 3; sxy += 3;
            if (flags & WV_FOG) fog += 3;
            if (flags & WV_SZ16) zs16 += 3; else zs32 += 3;
        }
        gte_ldrgbc(rgbc);
    }
#endif
    do {
        for (i = 0; i < 6; i++) {
            w[i] = v[i];
        }
        gte_ldv3c(w);
        WV_RTPT_DEPTHS();
        gte_ldv3(&D_800CD794[(w[1] >> 16) & 0xFF], &D_800CD794[(w[3] >> 16) & 0xFF],
                 &D_800CD794[(w[5] >> 16) & 0xFF]);
        v += 6;
        n -= 3;
        gte_nct();
        gte_stsz3c(sz);
        gte_strgb3(rgb);
        if (flags & WV_FOG) {
            for (i = 0; i < 3; i++) {
                wv_fog(rgb[i], sz[i], w[2 * i + 1], flags, &lit[i], &fog[i]);
            }
            fog += 3;
        } else {
            lit[0] = rgb[0];
            lit[1] = rgb[1];
            lit[2] = rgb[2];
        }
        lit += 3;

        if (flags & WV_CURVE_MUL) {
            for (i = 0; i < 3; i++) {
                f = 0x1000 - ((sz[i] >> 4) + (sz[i] >> 6));
                y = (s32)(w[2 * i] & 0xFFFF0000) >> 16;
                y = (s32)((u32)y * f) >> 12;
                w[2 * i] = (w[2 * i] & 0xFFFF) | ((u32)y << 16);
            }
        }
        if (flags & WV_CURVE_GTE) {
            /* IR1 = y, IR0 = SZ/16 + SZ/64, far colour red = 0: MAC1 = y - y * IR0 / 4096. The original loads
             * V0 before the third INTPL and V1 after it; kept in that order. */
            gte_strfc(rfc);
            gte_ldrfc(0);
            for (i = 0; i < 3; i++) {
                gte_lddp((sz[i] >> 4) + (sz[i] >> 6));
                gte_ldir1((s32)w[2 * i] >> 16);
                w[2 * i] &= 0xFFFF;
                if (i == 2) {
                    gte_ldv0(&w[0]);
                }
                gte_intpl();
                if (i == 2) {
                    gte_ldv1(&w[2]);
                }
                gte_stmac1(m);
                w[2 * i] |= m << 16;
            }
            gte_ldv2(&w[4]);
            gte_ldrfc(rfc);
        } else {
            gte_ldv3c(w);
        }
        gte_rtpt();
        gte_stsxy3c(sxy);
        sxy += 3;
        gte_stsz3c(sz);
        if (flags & WV_SZ16) {
            zs16[0] = sz[0] >> 3;
            zs16[1] = sz[1] >> 3;
            zs16[2] = sz[2] >> 3;
            zs16 += 3;
        } else {
            zs32[0] = sz[0];
            zs32[1] = sz[1];
            zs32[2] = sz[2];
            zs32 += 3;
        }
    } while (n > 0);
}

PORT_OVERLAY_CODE(0c) void o0c_wm_blockVertsFog(void *o) { wv_transform(o, WV_FOG | WV_CURVE_MUL); }
PORT_OVERLAY_CODE(0c) void o0c_wm_modelVerts(s32 o) { wv_transform((u8 *)o, WV_FOG | WV_CURVE_MUL | WV_SZ16); }
PORT_OVERLAY_CODE(0c) void o0c_wm_modelVertsFlat(s32 o) { wv_transform((u8 *)o, WV_FOG | WV_SZ16); }
PORT_OVERLAY_CODE(0c) void o0c_wm_blockVertsFar(void *o) { wv_transform(o, WV_FOG | WV_DEPTH_HALF | WV_CURVE_GTE); }
PORT_OVERLAY_CODE(0c) void o0c_wm_blockVertsNear(void *o) { wv_transform(o, WV_CURVE_MUL); }
