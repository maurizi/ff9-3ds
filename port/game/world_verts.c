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
