/* The world map's block face routines (ovl_0c's hand-written wm_blockFaces{Fog,Near,Far}; decomp
 * src_equiv/ovl_0c/world_faces.c, weak there) in port code: the same packets in the same order, with each face's NCLIP
 * worked out in place instead of through the GTE (three register loads, the command and a read a face: some 5000
 * faces a frame). The screen positions are the vertex pass's, clamped to -1024..1023 by its projection, so the
 * products and their sum stay in 32 bits as MAC0 has them. The GTE is left as the routines leave it: the last face's
 * positions loaded and NCLIP run on them. */
#include "psyq_port.h"
#include "ovl_0c.h"
#include "port_hw.h"
#include "native_mesh.h"
#include <stdint.h>

extern u32 g_worldVtxFogColors[]; /* per-vertex fog colours */
extern u32 g_worldVtxLitColors[]; /* per-vertex lit colours */
extern u16 g_worldVtxDepth[];     /* per-vertex depth (u32 SZ for the world blocks) */

s32 o0c_wm_blockFacesFog(void *o, s32 pkt, s32 ot);
s32 o0c_wm_blockFacesNear(void *o, s32 pkt, s32 ot);
s32 o0c_wm_blockFacesFar(void *o, s32 pkt, s32 ot);

#define WF_Z32_GRID 4   /* the deepest chosen on ((depth >> 3) & ~0xF) and range-tested on that */
#define WF_FOG 0x10     /* POLY_G3 of the fog colours when one is non-zero */
#define WF_NEED_LIT 0x20 /* POLY_GT3 only when a lit colour is non-zero */
#define WF_CULL 0x40    /* drop faces whose three vertices are all right/below of or left of the screen */

static inline void wf_link(u32 *slot, u32 *p, u32 len) {
    p[0] = (*slot & 0xFFFFFF) | (len << 24);
    *slot = (u32)(uintptr_t)p & 0xFFFFFF;
}

/* index of the largest of three depths, by signed difference as the original compares */
static inline s32 wf_max3(u32 a, u32 b, u32 c) {
    if ((s32)(a - b) > 0) return (s32)(a - c) > 0 ? 0 : 2;
    return (s32)(b - c) > 0 ? 1 : 2;
}

static inline __attribute__((always_inline)) s32 wf_mesh(u8 *o, s32 pkt, s32 ot, const s32 flags, const s32 limit) {
    const u32 *sxyTab = (const u32 *)SCRATCHPAD;
    u32 *alt = (u32 *)(SCRATCHPAD + 0x3D0);
    const u32 *src = (const u32 *)g_worldCurTexArea;
    const u32 *face = *(u32 **)(o + 8);
    u16 n = *(u16 *)(o + 2);
    u32 *p = (u32 *)(uintptr_t)pkt;
    u32 *const otp = (u32 *)(uintptr_t)ot;
    const u32 *zt = (const u32 *)g_worldVtxDepth;
    alt[0] = src[0];
    alt[1] = src[1];
    alt[2] = src[2];
    u32 s0, s1, s2;
    do {
        const u32 w = face[0];
        const u32 i0 = w & 0xFF, i1 = (w >> 8) & 0xFF, i2 = (w >> 16) & 0xFF;
        s0 = sxyTab[i0], s1 = sxyTab[i1], s2 = sxyTab[i2];
        const u32 *f = face;
        face += 4;
        if ((flags & WF_CULL) && (s0 & s1 & s2 & 0xFF008000)) continue;
        {
            const s32 x0 = (s16)s0, y0 = (s32)s0 >> 16, x1 = (s16)s1, y1 = (s32)s1 >> 16, x2 = (s16)s2, y2 = (s32)s2 >> 16;
            if (x0 * y1 + x1 * y2 + x2 * y0 - x0 * y2 - x1 * y0 - x2 * y1 <= 0) continue;
        }
        const u32 bias = (w & 0x1000000) >> 15;
        const u32 z0 = zt[i0] + bias, z1 = zt[i1] + bias, z2 = zt[i2] + bias;
        u32 otz;
        if (flags & WF_Z32_GRID) {
            const u32 g0 = (z0 >> 3) & ~0xFu, g1 = (z1 >> 3) & ~0xFu, g2 = (z2 >> 3) & ~0xFu;
            const s32 k = wf_max3(g0, g1, g2);
            const u32 g = k == 0 ? g0 : k == 1 ? g1 : g2;
            if ((s32)g <= 0 || (s32)g >= limit) continue;
            otz = (k == 0 ? z0 : k == 1 ? z1 : z2) >> 3;
        } else {
            const s32 k = wf_max3(z0, z1, z2);
            otz = (k == 0 ? z0 : k == 1 ? z1 : z2) >> 3;
            if ((s32)otz <= 0 || (s32)otz >= limit) continue;
        }
        const u32 fg0 = g_worldVtxFogColors[i0], fg1 = g_worldVtxFogColors[i1], fg2 = g_worldVtxFogColors[i2];
        const u32 lt0 = g_worldVtxLitColors[i0], lt1 = g_worldVtxLitColors[i1], lt2 = g_worldVtxLitColors[i2];
        const u32 litOr = (lt0 | lt1 | lt2) & 0xFFFFFF;
        u32 *slot = otp + otz;
        if ((flags & WF_FOG) && ((fg0 | fg1 | fg2) & 0xFFFFFF)) {
            p[2] = s0;
            p[4] = s1;
            p[6] = s2;
            p[1] = (fg0 & 0xFFFFFF) | (litOr ? 0x32000000 : 0x30000000);
            p[3] = fg1;
            p[5] = fg2;
            wf_link(slot, p, 6);
            p += 7;
        }
        if ((flags & WF_NEED_LIT) && !litOr) continue;
        p[2] = s0;
        p[5] = s1;
        p[8] = s2;
        p[1] = (lt0 & 0xFFFFFF) | 0x34000000;
        p[4] = lt1;
        p[7] = lt2;
        u32 uv0 = f[1], uv1 = f[2], uv2 = f[3]; /* u0 v0 clut, u1 v1 tpage, u2 v2 flags */
        if ((s32)otz < 0x9C4) {
            if (uv2 & 0x20000) {
                const u32 a = alt[2];
                uv0 += a & 0xFFFF0000;
                uv1 = (uv1 & 0xFFFF) + (a << 16);
            }
        } else if (uv2 & 0x20000) {
            u32 a = alt[0];
            const u32 b = alt[1], tp = (a & 0xFFFF) << 16;
            a &= 0xFFFF0000;
            uv0 = ((((uv0 & 0xFEFE) >> 1) | (((uv0 & 0xFFFF0000) >> 6) + a)) + b);
            uv1 = (((uv1 & 0xFEFE) >> 1) | tp) + b;
            uv2 = ((uv2 & 0xFEFE) >> 1) + b;
        } else {
            uv0 = ((uv0 & 0xFEFE) >> 1) | ((uv0 & 0xFFFF0000) - 0x10000);
            uv1 = ((uv1 & 0xFEFE) >> 1) | (uv1 & 0xFE7F0000);
            uv2 = (uv2 & 0xFEFE) >> 1;
        }
        p[3] = uv0;
        p[6] = uv1;
        p[9] = uv2;
        wf_link(slot, p, 9);
        p += 10;
    } while (--n != 0);
    gte_ldsxy3(s0, s1, s2); /* (the GTE as the last face left it) */
    gte_nclip();
    return (s32)(uintptr_t)p;
}

/* world-block faces after wm_blockVertsFog (g_worldFog set) */
PORT_OVERLAY_CODE(0c) s32 o0c_wm_blockFacesFog(void *o, s32 pkt, s32 ot) {
    return wf_mesh(o, pkt, ot, WF_FOG | WF_NEED_LIT, 0xFFE);
}
/* world-block faces after wm_blockVertsFar (no fog, a far block) */
PORT_OVERLAY_CODE(0c) s32 o0c_wm_blockFacesFar(void *o, s32 pkt, s32 ot) {
    return wf_mesh(o, pkt, ot, WF_Z32_GRID | WF_FOG | WF_NEED_LIT, 0xFFE);
}
/* world-block faces after wm_blockVertsNear (no fog, a near block): off-screen faces culled, no fog primitive */
PORT_OVERLAY_CODE(0c) s32 o0c_wm_blockFacesNear(void *o, s32 pkt, s32 ot) {
    return wf_mesh(o, pkt, ot, WF_CULL, 0xF6A);
}
