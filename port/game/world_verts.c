/* The world map's vertex pass (ovl_0c's hand-written routines; decomp src_equiv/ovl_0c/world_verts.c) in port code
 * (they are weak there): the same steps, except that the first RTPT of each group, read only for its depths (the
 * second overwrites the screen positions, MAC/IR and MAC0/IR0 before anything reads them), pushes the depths alone
 * in DS mode (psx_gte_rtpt_depths): 1.3-1.4 ms per VBlank of a world map's frame. */
#include "psyq_port.h"
#include "ovl_0c.h"
#include "port_hw.h"
#include "native_mesh.h"

extern SVECTOR g_worldNormalTable[]; /* normals, indexed by the low byte of a vertex's pad */
extern u32 g_worldVtxFogColors[];     /* per-vertex fog colours */
extern u32 g_worldVtxLitColors[];     /* per-vertex lit colours */
extern u16 g_worldVtxDepth[];     /* per-vertex depth: u32 SZ or u16 SZ / 8, depending on the routine */

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
/* DS mode: a group's GTE steps as arithmetic (each the GTE's own: NCT's two products and DPCS's and INTPL's
 * interpolation in 32 bits where gte.c's mat_mul and interpolate take their 32-bit paths, which give what the 64-bit
 * ones would, else in 64; the RTPTs as DS mode's 32-bit ones: rtp_sf1's and psx_gte_rtpt_depths'), for every group
 * but the last, which wv_transform runs through the GTE so the registers end as the game leaves them. The one
 * register a group reads from the one before is RGBC (the colour code NCT pushes with: the last fog colour loaded).
 * The GTE's constants are read once a call (WvCtx). */
typedef struct {
    u32 rot[5], llm[5], lcm[5];
    s32 tr[3], bk[3], fc[3], ofx, ofy;
    u32 h;
    s32 fcc[3];        /* the far colour, clamped to where DPCS's IR clamps anyway (wv_ir) */
    s32 fog_ir[2][3];  /* DPCS's IR for each fog entry's colour (wv_ir) */
    const u16 *unr;
    u32 nct_have[8], nct[256]; /* NCT's colours by normal (without the code byte), those worked out */
} WvCtx;
static inline s32 wv_clamp32(s32 v, s32 lo, s32 hi) { return v < lo ? lo : v > hi ? hi : v; }
/* M x V >> 12 plus T, as mat_mul's results (rotation layout: 5 words) */
static inline void wv_mat(const u32 *c, const s32 *t, s32 x, s32 y, s32 z, s32 *o) {
    if ((u32)((x < 0 ? -x : x) + (y < 0 ? -y : y) + (z < 0 ? -z : z)) < 0x10000 &&
        (u32)t[0] + 0x40000000u < 0x80000000u && (u32)t[1] + 0x40000000u < 0x80000000u &&
        (u32)t[2] + 0x40000000u < 0x80000000u) {
        o[0] = t[0] + (((s16)c[0] * x + ((s32)c[0] >> 16) * y + (s16)c[1] * z) >> 12);
        o[1] = t[1] + ((((s32)c[1] >> 16) * x + (s16)c[2] * y + ((s32)c[2] >> 16) * z) >> 12);
        o[2] = t[2] + (((s16)c[3] * x + ((s32)c[3] >> 16) * y + (s16)c[4] * z) >> 12);
        return;
    }
    o[0] = (s32)((((long long)t[0] << 12) + (long long)(s16)c[0] * x + (long long)((s32)c[0] >> 16) * y + (long long)(s16)c[1] * z) >> 12);
    o[1] = (s32)((((long long)t[1] << 12) + (long long)((s32)c[1] >> 16) * x + (long long)(s16)c[2] * y + (long long)((s32)c[2] >> 16) * z) >> 12);
    o[2] = (s32)((((long long)t[2] << 12) + (long long)(s16)c[3] * x + (long long)((s32)c[3] >> 16) * y + (long long)(s16)c[4] * z) >> 12);
}
/* NCT's colour for one normal (sf 1, lm 1), pushed with the code byte (once per normal a call: out of ITCM) */
#if defined(__arm__)
__attribute__((target("arm")))
#endif
PORT_OVERLAY_CODE(0c) static u32 wv_nct(const WvCtx *cx, const SVECTOR *nv, u32 code) {
    static const s32 zero[3] = {0, 0, 0};
    s32 m[3];
    wv_mat(cx->llm, zero, nv->vx, nv->vy, nv->vz, m); /* LLM x V */
    const s32 i1 = wv_clamp32(m[0], 0, 0x7FFF), i2 = wv_clamp32(m[1], 0, 0x7FFF), i3 = wv_clamp32(m[2], 0, 0x7FFF);
    wv_mat(cx->lcm, cx->bk, i1, i2, i3, m); /* BK + LCM x IR */
    return code << 24 | (u32)wv_clamp32(m[2] >> 4, 0, 0xFF) << 16 | (u32)wv_clamp32(m[1] >> 4, 0, 0xFF) << 8 |
           (u32)wv_clamp32(m[0] >> 4, 0, 0xFF);
}
/* DPCS (sf 1, lm 0) of colour c toward the far colour fc by dp, in gte.c's terms: IR = clamp((FC << 12 - C << 16) >> 12)
 * = clamp(FC - 16 C) (both terms multiples of 4096), then the colour clamp((IR dp + C << 16) >> 12 >> 4)
 * = clamp(C + (IR dp >> 16)), whose sum stays in 32 bits (|IR dp| < 2^27). wv_ir is the first step (a far colour
 * clamped to +-40000 gives the same IR), wv_dpcs_ir the second. */
static inline void wv_ir(const s32 *fcc, u32 rgb, s32 *ir) {
    for (int k = 0; k < 3; ++k) ir[k] = wv_clamp32(fcc[k] - 16 * (s32)(rgb >> (8 * k) & 0xFF), -0x8000, 0x7FFF);
}
static inline u32 wv_dpcs_ir(u32 rgbc, const s32 *ir, s32 dp) {
    u32 out = rgbc & 0xFF000000u;
    for (int k = 0; k < 3; ++k)
        out |= (u32)wv_clamp32((s32)(rgbc >> (8 * k) & 0xFF) + ((ir[k] * dp) >> 16), 0, 0xFF) << (8 * k);
    return out;
}
/* RTPS's projection of one vertex in DS mode (rtp_sf1's arithmetic): its SXY and SZ */
static inline void wv_project(const WvCtx *cx, u32 vxy, u32 vz, u32 *sxy, u32 *sz) {
    const s32 x = (s16)vxy, y = (s32)vxy >> 16, z = (s16)vz;
    const u32 *c = cx->rot;
    s32 ir1 = cx->tr[0] + (((s16)c[0] * x + ((s32)c[0] >> 16) * y + (s16)c[1] * z) >> 12);
    s32 ir2 = cx->tr[1] + ((((s32)c[1] >> 16) * x + (s16)c[2] * y + ((s32)c[2] >> 16) * z) >> 12);
    const s32 mac3 = cx->tr[2] + (((s16)c[3] * x + ((s32)c[3] >> 16) * y + (s16)c[4] * z) >> 12);
    ir1 = wv_clamp32(ir1, -0x8000, 0x7FFF);
    ir2 = wv_clamp32(ir2, -0x8000, 0x7FFF);
    const u32 sz3 = mac3 > 0xFFFF ? 0xFFFF : mac3 < 0 ? 0 : (u32)mac3;
    u32 div;
    if (cx->h >= sz3 * 2) div = 0x1FFFF;
    else {
        const int sh = __builtin_clz(sz3) - 16;
        u32 n = cx->h << sh, d = sz3 << sh;
        const u32 u = cx->unr[(d - 0x7FC0) >> 7] + 0x101;
        d = (0x2000080 - d * u) >> 8;
        d = (0x0000080 + d * u) >> 8;
        const unsigned long long q = ((unsigned long long)n * d + 0x8000) >> 16;
        div = q > 0x1FFFF ? 0x1FFFF : (u32)q;
    }
    const long long mx = (long long)ir1 * div + cx->ofx, my = (long long)ir2 * div + cx->ofy;
    s32 sx = (s32)(mx >> 16), sy = (s32)(my >> 16);
    const int clamped = sx != wv_clamp32(sx, -0x400, 0x3FF) || sy != wv_clamp32(sy, -0x400, 0x3FF);
    sx = wv_clamp32(sx, -0x400, 0x3FF);
    sy = wv_clamp32(sy, -0x400, 0x3FF);
    *sxy = (u32)sy << 16 | ((u32)sx & 0xFFFF);
    *sz = sz3;
#ifdef PORT_GTE_DEPTH
    /* (stereo depth, as gte.c records an RTPS's: SZ and the sub-pixel fractions) */
    psx_depth_record(sxy, *sxy, !sz3 || clamped ? 0 : sz3 << 16 | (u32)((my >> 8) & 0xFF) << 8 | (u32)((mx >> 8) & 0xFF));
#else
    (void)clamped;
#endif
}
/* one group's results; *rgbc: RGBC as the GTE would hold it, before and after */
static inline void wv_group(WvCtx *cx, const u32 *v, s32 flags, u32 *rgbc, u32 *lit, u32 *fog, u32 *sxy,
                            u32 *zs32, u16 *zs16) {
    u32 w[6], sz[3], rgb[3];
    for (int i = 0; i < 6; i++) w[i] = v[i];
    const u32 *c = cx->rot;
    for (int i = 0; i < 3; ++i) { /* (the first RTPT's depths) */
        const u32 vxy = w[2 * i];
        const s32 mac3 = cx->tr[2] + (((s16)c[3] * (s16)vxy + ((s32)c[3] >> 16) * ((s32)vxy >> 16) + (s16)c[4] * (s16)w[2 * i + 1]) >> 12);
        sz[i] = mac3 > 0xFFFF ? 0xFFFF : mac3 < 0 ? 0 : (u32)mac3;
    }
    for (int i = 0; i < 3; ++i) { /* (NCT's colour depends on the normal alone in a call: worked out once each) */
        const u32 ni = (w[2 * i + 1] >> 16) & 0xFF;
        if (!(cx->nct_have[ni >> 5] >> (ni & 31) & 1)) {
            cx->nct[ni] = wv_nct(cx, &g_worldNormalTable[ni], 0);
            cx->nct_have[ni >> 5] |= 1u << (ni & 31);
        }
        rgb[i] = cx->nct[ni] | *rgbc >> 24 << 24;
    }
    if (flags & WV_FOG) {
        for (int i = 0; i < 3; ++i) {
            const u32 *e = (const u32 *)g_worldFogParams + ((w[2 * i + 1] >> 25) & 1) * 2;
            const int which = (w[2 * i + 1] >> 25) & 1;
            s32 ir[3];
            wv_ir(cx->fcc, rgb[i], ir);
            lit[i] = wv_dpcs_ir(rgb[i], ir, wv_depth(sz[i], e[1] >> 16, flags));
            fog[i] = wv_dpcs_ir(e[0], cx->fog_ir[which], 0x1000 - wv_depth(sz[i], e[1] & 0xFFFF, flags));
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
            const s32 ir = wv_clamp32(-y, -0x8000, 0x7FFF);
            const u32 m = (u32)((ir * dp + (y << 12)) >> 12);
            w[2 * i] = (w[2 * i] & 0xFFFF) | m << 16;
        }
    }
    for (int i = 0; i < 3; ++i) wv_project(cx, w[2 * i], w[2 * i + 1], &sxy[i], &sz[i]);
    if (flags & WV_SZ16) { zs16[0] = (u16)(sz[0] >> 3); zs16[1] = (u16)(sz[1] >> 3); zs16[2] = (u16)(sz[2] >> 3); }
    else { zs32[0] = sz[0]; zs32[1] = sz[1]; zs32[2] = sz[2]; }
}
#endif

/* The groups through the emulated GTE (in DS mode only the last: the registers end as the game leaves them), out of
 * ITCM (once a block) */
PORT_OVERLAY_CODE(0c) static void wv_gte_groups(u32 *v, s32 n, s32 flags, u32 *lit, u32 *fog, u32 *sxy, u32 *zs32,
                                                u16 *zs16) {
    u32 w[6]; /* three vertices as raw words: w[2i] = x | y << 16, w[2i + 1] = z | pad << 16 */
    u32 sz[3];
    u32 rgb[3];
    u32 f, m, rfc;
    s32 i, y;

    do {
        for (i = 0; i < 6; i++) {
            w[i] = v[i];
        }
        gte_ldv3c(w);
        WV_RTPT_DEPTHS();
        gte_ldv3(&g_worldNormalTable[(w[1] >> 16) & 0xFF], &g_worldNormalTable[(w[3] >> 16) & 0xFF],
                 &g_worldNormalTable[(w[5] >> 16) & 0xFF]);
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

#if defined(__arm__)
__attribute__((target("arm"))) /* (its 64-bit products: Thumb makes each a call) */
#endif
PORT_ITCM_CODE(0c) static void wv_transform(u8 *o, s32 flags) {
    s32 n = *(u32 *)o & 0xFF;
    u32 *v = *(u32 **)(o + 4);
    u32 *fog = g_worldVtxFogColors;
    u32 *lit = g_worldVtxLitColors;
    u32 *sxy = (u32 *)SCRATCHPAD;
    u32 *zs32 = (u32 *)g_worldVtxDepth;
    u16 *zs16 = g_worldVtxDepth;

#ifdef PORT_GTE_APPROX
    if (n > 3) { /* all groups but the last as arithmetic (wv_group) */
        const u32 *c = psx_gte.c;
        WvCtx cx;
        for (int k = 0; k < 5; ++k) { cx.rot[k] = c[k]; cx.llm[k] = c[8 + k]; cx.lcm[k] = c[16 + k]; }
        for (int k = 0; k < 3; ++k) { cx.tr[k] = (s32)c[5 + k]; cx.bk[k] = (s32)c[13 + k]; cx.fc[k] = (s32)c[21 + k]; }
        cx.ofx = (s32)c[24]; cx.ofy = (s32)c[25]; cx.h = (u16)c[26];
        for (int k = 0; k < 3; ++k) cx.fcc[k] = wv_clamp32(cx.fc[k], -40000, 40000);
        for (int e = 0; e < 2; ++e) wv_ir(cx.fcc, ((const u32 *)g_worldFogParams)[2 * e], cx.fog_ir[e]);
        cx.unr = psx_gte_unr();
        for (int k = 0; k < 8; ++k) cx.nct_have[k] = 0;
        u32 rgbc = psx_gte.d[6];
        while (n > 3) {
            wv_group(&cx, v, flags, &rgbc, lit, fog, sxy, zs32, zs16);
            v += 6; n -= 3; lit += 3; sxy += 3;
            if (flags & WV_FOG) fog += 3;
            if (flags & WV_SZ16) zs16 += 3; else zs32 += 3;
        }
        gte_ldrgbc(rgbc);
    }
#endif
    wv_gte_groups(v, n, flags, lit, fog, sxy, zs32, zs16);
}

PORT_OVERLAY_CODE(0c) void o0c_wm_blockVertsFog(void *o) { wv_transform(o, WV_FOG | WV_CURVE_MUL); }
PORT_OVERLAY_CODE(0c) void o0c_wm_modelVerts(s32 o) { wv_transform((u8 *)o, WV_FOG | WV_CURVE_MUL | WV_SZ16); }
PORT_OVERLAY_CODE(0c) void o0c_wm_modelVertsFlat(s32 o) { wv_transform((u8 *)o, WV_FOG | WV_SZ16); }
PORT_OVERLAY_CODE(0c) void o0c_wm_blockVertsFar(void *o) { wv_transform(o, WV_FOG | WV_DEPTH_HALF | WV_CURVE_GTE); }
PORT_OVERLAY_CODE(0c) void o0c_wm_blockVertsNear(void *o) { wv_transform(o, WV_CURVE_MUL); }
