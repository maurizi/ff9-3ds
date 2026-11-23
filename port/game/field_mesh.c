/* Field model vertex projection with the per-vertex loop in port code (config/overrides.txt), as
 * btl_projectMesh's (mesh_native.c): fldmdl_projectMesh is the same routine over the field's model groups. The bone
 * matrices are set up through the GTE as the game wrote it; the vertices go through gte_rtpt_batch (gte.c), and the
 * last group runs through the GTE again, so the registers end as the game leaves them.
 */
#include "psyq_port.h"
#include "ovl_0a.h"
#include "port_hw.h"
#include "native_mesh.h"
#include <stdint.h>

void o0a_fldmdl_projectMesh(ModelObj *arg0, u8 arg1);

/* the game's bone matrix: m = RT x bone, each column as rtir (MVMVA sf=1 lm=0 of the column through IR1-3: the sum
 * shifted, saturated to 16 bits), t = RT x bone.t + TR as rtv0tr (V0 the translation's low halves, MAC1-3 the sum
 * shifted to 32 bits), as gte.c's MVMVA works them out. What those commands leave in the GTE (V0, IR1-3, MAC1-3,
 * FLAG) the routine's last RTPT overwrites, and the view matrix they ran on is replaced by m before anything reads
 * it: in port code, without the two matrix loads, three column loads and four commands a bone */
static inline void fm_bone(const MATRIX *rt, const MATRIX *bone, MATRIX *m) {
    for (int j = 0; j < 3; j++) {
        const int64_t x = bone->m[0][j], y = bone->m[1][j], z = bone->m[2][j];
        for (int i = 0; i < 3; i++) {
            s32 v = (s32)((rt->m[i][0] * x + rt->m[i][1] * y + rt->m[i][2] * z) >> 12);
            m->m[i][j] = (s16)(v > 0x7FFF ? 0x7FFF : v < -0x8000 ? -0x8000 : v);
        }
    }
    const int64_t x = (s16)bone->t[0], y = (s16)bone->t[1], z = (s16)bone->t[2];
    for (int i = 0; i < 3; i++)
        m->t[i] = (s32)((((int64_t)rt->t[i] << 12) + rt->m[i][0] * x + rt->m[i][1] * y + rt->m[i][2] * z) >> 12);
}

PORT_OVERLAY_CODE(0a) void o0a_fldmdl_projectMesh(ModelObj *arg0, u8 arg1) {
    MATRIX m;
    WmModelGroup *g = &arg0->model->groups[arg1];
    gte_SetGeomScreen(g_gameState->gameData->projDist);
    s32 k = 0;
    u32 cnt = arg0->model->boneCount;
    MATRIX *bone = (MATRIX *)arg0->boneMatrices;
    SVECTOR *last = NULL;
    s32 last_k = 0;
    for (u32 i = 0; i < cnt; bone++) {
        s32 n = ((u16 *)g->boneVertexCounts)[i++];
        if (n == 0) continue;
        fm_bone(&g_gameState->gameData->viewMatrix, bone, &m);
        gte_SetRotMatrix(&m);
        gte_SetTransMatrix(&m);
        /* whole groups of three, as the game's loop (it reads and writes up to two past the bone's vertices) */
        SVECTOR *vtx = &((SVECTOR *)g->vertices)[k];
        s32 groups = (n + 2) / 3;
        gte_rtpt_batch((const gte_u32 *)vtx, groups * 3, (gte_u32 *)&g_vtxScreenXY[k], (gte_u32 *)&g_vtxScreenZ[k]);
        last = vtx + (groups - 1) * 3;
        last_k = k + (groups - 1) * 3;
        k += n;
    }
    if (last) { /* the registers as the game's last group leaves them */
        gte_ldv3c(last);
        gte_rtpt_b();
        gte_ldv3c(last + 3);
        gte_stsxy3c(&g_vtxScreenXY[last_k]);
        gte_stsz3c(&g_vtxScreenZ[last_k]);
    }
}

/* fld_emitSprite3D (decomp src_equiv/ovl_0a/field_sprites.c: a field's 3D sprites, quads of half size (halfW,
 * halfH) around each (x, y, 0)): each corner projected by psx_gte_project as the game's RTPT and RTPS would (their
 * SXY, the third corner's SZ for the OT entry and its MAC0 for the stop), the colour scaled as gte_gpf12 does, the
 * packet written in the game's order. The last quad's GTE work runs through the GTE again, so the registers end as
 * the game leaves them. */
u8 *o0a_fld_emitSprite3D(u8 *src, FieldSprArgs *a, u8 *pkt, u32 *ot);
PORT_OVERLAY_CODE(0a) u8 *o0a_fld_emitSprite3D(u8 *src, FieldSprArgs *a, u8 *pkt, u32 *ot) {
    const u16 *uvTab = a->uvs;
    const u8 *colTab = a->colors;
    const s32 hw = a->halfW, hh = a->halfH, mod = a->fade;
    const u32 tpage = (u32)a->tpage << 16, clut = (u32)a->clut << 16, code = (u32)a->primCode << 24;
    s32 n = a->quadCount;
    if (n == 0) return pkt;
    SVECTOR v[4];
    u32 last_k = 0;
    int stopped = 0;
    do {
        u32 *p = (u32 *)pkt;
        const s32 x = (s8)src[0] * 4, y = (s8)src[1] * 4;
        v[0].vx = (s16)(x - hw); v[0].vy = (s16)(y - hh); v[0].vz = 0; v[0].pad = 0;
        v[1].vx = (s16)(x + hw); v[1].vy = (s16)(y - hh); v[1].vz = 0; v[1].pad = 0;
        v[2].vx = (s16)(x - hw); v[2].vy = (s16)(y + hh); v[2].vz = 0; v[2].pad = 0;
        v[3].vx = (s16)(x + hw); v[3].vy = (s16)(y + hh); v[3].vz = 0; v[3].pad = 0;
        u32 sxy[4], sz[4], mac0[4];
        for (int i = 0; i < 3; ++i)
            psx_gte_project((u16)v[i].vx | (u32)(u16)v[i].vy << 16, 0, &sxy[i], &sz[i], &mac0[i]);
        const u32 k = src[2];
        src += 3;
        last_k = k;
        const u32 uv = uvTab[k & 0xF], u = uv & 0xFF, vv = uv >> 8, ur = u + ((u32)hw >> 1), vb = vv + ((u32)hh >> 1);
        p[3] = u | (vv << 8) | clut;
        p[5] = ur | (vv << 8) | tpage;
        p[7] = u | (vb << 8);
        p[2] = sxy[0]; p[4] = sxy[1]; p[6] = sxy[2];
        p[9] = ur | (vb << 8);
        if (mac0[2] == 0) { stopped = 1; break; }
        const u32 sz3 = sz[2];
        psx_gte_project((u16)v[3].vx | (u32)(u16)v[3].vy << 16, 0, &sxy[3], &sz[3], &mac0[3]);
        const u8 *ce = colTab + (k >> 4) * 4;
        u32 c = *(const u16 *)ce | ((u32)ce[2] << 16);
        p[8] = sxy[3];
        if (mod >= 0) { /* (gte_gpf12: IR0 x each channel >> 12, saturated as IR, the low byte kept) */
            u32 o = 0;
            for (int ch = 0; ch < 3; ++ch) {
                s32 m = (s16)mod * (s32)(c >> (8 * ch) & 0xFF) >> 12; /* (IR0 holds 16 bits) */
                if (m > 0x7FFF) m = 0x7FFF; else if (m < -0x8000) m = -0x8000;
                o |= ((u32)m & 0xFF) << (8 * ch);
            }
            c = o;
        }
        p[1] = c | code;
        u32 *slot = &ot[(s32)sz3 >> 2];
        const u32 e = *slot;
        p[0] = (e & 0xFFFFFF) | 0x09000000;
        *slot = (e & 0xFF000000) | ((u32)p & 0xFFFFFF);
        pkt += 0x28;
    } while (--n != 0);
    /* the registers as the game's last quad leaves them */
    gte_ldv3(&v[0], &v[1], &v[2]);
    gte_rtpt();
    if (!stopped) {
        gte_ldv0(&v[3]);
        gte_rtps();
        if (mod >= 0) {
            const u8 *ce = colTab + (last_k >> 4) * 4;
            const u32 c = *(const u16 *)ce | ((u32)ce[2] << 16);
            gte_ldir0123(mod, c & 0xFF, (c >> 8) & 0xFF, (c >> 16) & 0xFF);
            gte_gpf12();
        }
    }
    return pkt;
}

/* fldmdl_projectMeshClipY (decomp tu_800B61A4.c), the same way: each bone's vertices through its matrix (MVMVA RT
 * V0 TR, sf 1, as rtv0tr_b), IR2 raised to the model's clipY where it isn't below it (the depth word then flagged,
 * gte_stsvclamp), then the camera's RTPT over the bone's vertices in whole groups of three (into the next bone's,
 * which that bone rewrites first; gte_rtpt_batch), the depths as halfwords under the flags. The last group runs
 * through the GTE again, so the registers end as the game leaves them. */
void o0a_fldmdl_projectMeshClipY(ModelObj *arg0, u8 arg1);
static inline s32 ir_of(long long mac) { return mac > 0x7FFF ? 0x7FFF : mac < -0x8000 ? -0x8000 : (s32)mac; }
#ifdef PORT_GTE_APPROX /* (DS mode's MVMVA: 32-bit sums, as psx_gte_mvmva_rt12) */
typedef s32 ClipMac;
#else
typedef long long ClipMac;
#endif
#if defined(__arm__)
__attribute__((target("arm"))) /* (Thumb has no long multiply) */
#endif
PORT_ITCM_CODE(0a) void o0a_fldmdl_projectMeshClipY(ModelObj *arg0, u8 arg1) {
    WmModelGroup *g = &arg0->model->groups[arg1];
    gte_SetGeomScreen(g_gameState->gameData->projDist);
    const MATRIX *bone = (const MATRIX *)arg0->boneMatrices;
    const u32 cnt = arg0->model->boneCount;
    const s32 ymax = arg0->clipY;
    const SVECTOR *vtx = (const SVECTOR *)g->vertices;
    s32 k = 0, last_k = -1;
    u32 last_xy[3], last_z[3]; /* (the last group's vertices, for the GTE) */
    for (u32 i = 0; i < cnt; bone++) {
        const s32 n = ((u16 *)g->boneVertexCounts)[i++];
        if (n == 0) continue;
        u32 *sxy = (u32 *)&g_vtxScreenXY[k], *sz = (u32 *)&g_vtxScreenZ[k];
        const s32 r11 = bone->m[0][0], r12 = bone->m[0][1], r13 = bone->m[0][2];
        const s32 r21 = bone->m[1][0], r22 = bone->m[1][1], r23 = bone->m[1][2];
        const s32 r31 = bone->m[2][0], r32 = bone->m[2][1], r33 = bone->m[2][2];
        const s32 tx = bone->t[0], ty = bone->t[1], tz = bone->t[2];
        for (s32 j = 0; j < n; ++j, ++vtx) {
            const s32 vx = vtx->vx, vy = vtx->vy, vz = vtx->vz;
            /* (TR << 12 + M V) >> 12 is TR + (M V >> 12) exactly) */
            const s32 x = ir_of((long long)tx + (((ClipMac)r11 * vx + (ClipMac)r12 * vy + (ClipMac)r13 * vz) >> 12));
            s32 y = ir_of((long long)ty + (((ClipMac)r21 * vx + (ClipMac)r22 * vy + (ClipMac)r23 * vz) >> 12));
            u32 z = (u32)ir_of((long long)tz + (((ClipMac)r31 * vx + (ClipMac)r32 * vy + (ClipMac)r33 * vz) >> 12)) & 0xFFFF;
            if (!(y < ymax)) { y = ymax; z |= 0x10000000; }
            sxy[j] = (u32)(u16)x | (u32)y << 16;
            sz[j] = z;
        }
        /* the camera's projection, three at a time, in place: (x, y) and the depth's low half as the vertex */
        gte_SetRotMatrix(&g_gameState->gameData->viewMatrix);
        gte_SetTransMatrix(&g_gameState->gameData->viewMatrix);
        const s32 groups = (n + 2) / 3;
        for (s32 q = 0; q < 3; ++q) { last_xy[q] = sxy[(groups - 1) * 3 + q]; last_z[q] = sz[(groups - 1) * 3 + q]; }
        enum { CHUNK = 48 };
        u32 v[CHUNK * 2], oxy[CHUNK], oz[CHUNK];
        for (s32 at = 0; at < groups * 3; at += CHUNK) {
            const s32 m = groups * 3 - at < CHUNK ? groups * 3 - at : CHUNK;
            for (s32 q = 0; q < m; ++q) { v[q * 2] = sxy[at + q]; v[q * 2 + 1] = (u32)(s32)(s16)sz[at + q]; }
            gte_rtpt_batch((const gte_u32 *)v, m, (gte_u32 *)oxy, (gte_u32 *)oz);
            for (s32 q = 0; q < m; ++q) {
                sxy[at + q] = oxy[q];
                sz[at + q] = (sz[at + q] & 0xFFFF0000u) | (oz[q] & 0xFFFF);
            }
        }
        last_k = k + (groups - 1) * 3;
        k += n;
    }
    if (last_k >= 0) { /* the registers as the game's last group leaves them */
        gte_ldv3xyz(last_xy, last_z);
        gte_rtpt_b();
        gte_stsxy3c(&g_vtxScreenXY[last_k]);
        gte_stsz3h(&g_vtxScreenZ[last_k]);
    }
}

/* fldmdl_drawMesh (decomp src_equiv/ovl_0a/model_link.c, renamed by config/renames.txt): the renderer may take the
 * group (gx_hybrid.c port_native_field_mesh) instead of the game linking its prebuilt primitives into the OT. */
void o0a_fldmdl_drawMesh_link(ModelObj *model, u8 group);
void o0a_fldmdl_drawMesh(ModelObj *model, u8 group);

__attribute__((weak)) int port_native_field_mesh(const NativeFieldMesh *m) { (void)m; return 0; }

/* fldmdl_drawMesh's linking (model_link.c) for the group's translucent polygons only (code bit 1 in the packet):
 * the renderer drew the opaque ones */
static const u8 kind_size[8] = {0x28, 0x20, 0x34, 0x28, 0x24, 0x1C, 0x18, 0x14};
static const u8 kind_src[8] = {0x18, 0x14, 0x20, 0x18, 0x18, 0x14, 0x10, 0x0C};
static const u8 kind_flags[8] = {0x15, 0x12, 0x1C, 0x0F, 0x14, 0x13, 0x0D, 0x0B};
static const u8 kind_len[8] = {9, 7, 12, 9, 8, 6, 5, 4};
static const u8 kind_xy[8][4] = {{0x08, 0x10, 0x18, 0x20}, {0x08, 0x10, 0x18}, {0x08, 0x14, 0x20, 0x2C},
                                 {0x08, 0x14, 0x20}, {0x08, 0x10, 0x18, 0x20}, {0x08, 0x10, 0x18},
                                 {0x08, 0x0C, 0x10, 0x14}, {0x08, 0x0C, 0x10}};
/* (DS mode: in ITCM as ARM, every scene's translucent model polygons through it) */
#if defined(__arm__) && defined(PORT_DS_MODE) && defined(PORT_GX_HYBRID)
__attribute__((section(".itcm"), long_call, target("arm"), noinline))
#else
PORT_OVERLAY_CODE(0a)
#endif
static void link_translucent(u8 *grp, u8 *pkt, u32 *ot, int clip_y, int mirror) {
    u8 *poly = *(u8 **)(grp + 0x1C);
    for (int k = 0; k < 8; k++) {
        const int quad = !(k & 1);
        for (s32 n = *(u16 *)(grp + 2 + 2 * k); n > 0; n--, poly += kind_src[k], pkt += kind_size[k]) {
            if (!(pkt[7] & 2)) continue;
            u32 i0 = *(u16 *)(poly + 0), i1 = *(u16 *)(poly + 2), i2 = *(u16 *)(poly + 4);
            u32 i3 = quad ? *(u16 *)(poly + 6) : 0;
            u32 xy0 = g_vtxScreenXY[i0], xy1 = g_vtxScreenXY[i1], xy2 = g_vtxScreenXY[i2], z;
            if (clip_y) { /* (fldmdl_drawMeshClipY: a flagged vertex leaves it out; its entry from the low bits) */
                u32 sum = (u32)g_vtxScreenZ[i0] + (u32)g_vtxScreenZ[i1] + (u32)g_vtxScreenZ[i2] + (quad ? (u32)g_vtxScreenZ[i3] : 0);
                if (sum > (quad ? 0x3FFFFFFFu : 0x2FFFFFFFu)) continue;
                if (!(poly[kind_flags[k]] & 2)) {
                    s32 nclip;
                    gte_ldsxy3(xy0, xy1, xy2);
                    *(u32 *)pkt = (u32)kind_len[k] << 24;
                    gte_nclip();
                    gte_stopz(&nclip);
                    if (nclip <= 0) continue;
                }
                *(u32 *)(pkt + kind_xy[k][0]) = xy0;
                *(u32 *)(pkt + kind_xy[k][1]) = xy1;
                *(u32 *)(pkt + kind_xy[k][2]) = xy2;
                if (quad) *(u32 *)(pkt + kind_xy[k][3]) = g_vtxScreenXY[i3];
                z = quad ? (sum & 0xFFFFF) >> 4 : (sum & 0xFFFFF) / 3 >> 2;
                u32 *slot = &ot[z];
                if (PSX_OT_OUTSIDE(slot)) continue;
                *(u32 *)pkt = *slot | ((u32)kind_len[k] << 24);
                *slot = (u32)pkt & 0xFFFFFF;
                continue;
            }
            if (!(poly[kind_flags[k]] & 2)) {
                s32 nclip;
                gte_ldsxy3(xy0, xy1, xy2);
                *(u32 *)pkt = (u32)kind_len[k] << 24;
                gte_nclip();
                gte_stopz(&nclip);
                if (mirror ? nclip >= 0 : nclip <= 0) continue; /* (a reflection keeps the other side) */
            }
            *(u32 *)(pkt + kind_xy[k][0]) = xy0;
            *(u32 *)(pkt + kind_xy[k][1]) = xy1;
            *(u32 *)(pkt + kind_xy[k][2]) = xy2;
            if (quad) {
                *(u32 *)(pkt + kind_xy[k][3]) = g_vtxScreenXY[i3];
                z = ((u32)g_vtxScreenZ[i0] + g_vtxScreenZ[i1] + g_vtxScreenZ[i2] + g_vtxScreenZ[i3]) >> 4;
            } else {
                gte_ldsz3(g_vtxScreenZ[i0], g_vtxScreenZ[i1], g_vtxScreenZ[i2]);
                gte_avsz3();
                gte_stotz(&z);
            }
            u32 *slot = &ot[z];
            if (PSX_OT_OUTSIDE(slot)) continue; /* (past the table: the PS1 never draws it, psx_mem.h) */
            *(u32 *)pkt = *slot | ((u32)kind_len[k] << 24);
            *slot = (u32)pkt & 0xFFFFFF;
        }
    }
}

/* The 3DS's GTE depth shadow (gte.c) for a group's packets, by their own addresses: each vertex word the game copied
 * from g_vtxScreenXY this frame gets the depth recorded where it was projected (its entry by g_vtxScreenXY's address).
 * The renderer looks each vertex up by the packet word's address (gte_depth.h psx_depth_find): it missed there, and
 * found the depth by value, a second random read (and, for two vertices at one screen position, the last one's
 * depth: at Lindblum ~500 of 2400 model vertices a frame got none, which in 3D left them without parallax). The
 * packets and the OT are the game's, untouched. */
#ifdef PORT_GTE_DEPTH
#include "gte.h"
#include "gte_depth.h"
static void depth_to_packets(const u8 *grp, const u8 *pkt) {
    if (!psx_depth_on) return; /* (2D: none recorded) */
    const u8 *poly = *(u8 *const *)(grp + 0x1C);
    for (int k = 0; k < 8; k++) {
        const int nv = k & 1 ? 3 : 4;
        for (s32 n = *(const u16 *)(grp + 2 + 2 * k); n > 0; n--, poly += kind_src[k], pkt += kind_size[k])
            for (int j = 0; j < nv; j++) {
                const u32 i = *(const u16 *)(poly + 2 * j), off = kind_xy[k][j];
                const u32 w = *(const u32 *)(pkt + off);
                if (w != (u32)g_vtxScreenXY[i]) continue; /* (not written this frame: culled) */
                const PsxDepthSlot *src = &psx_depth_tab[((const u8 *)&g_vtxScreenXY[i] - psx_ram) >> 2];
                if (src->word != w) continue;
                psx_depth_tab[(pkt + off - psx_ram) >> 2] = *src;
            }
    }
}
#else
static void depth_to_packets(const u8 *grp, const u8 *pkt) { (void)grp; (void)pkt; }
#endif

/* fldmdl_drawMesh's linking (the decomp's model_link.c, with config/patches.txt's F35) with each polygon's two GTE
 * commands worked out here: NCLIP's MAC0 (the three corners' cross product, 64 bits cut to 32) and AVSZ3's OTZ
 * (ZSF3 x the three depths >> 12, within 0..0xFFFF), as gte.c's. A crowd's 2600 polygons a frame (field 574) went
 * through the GTE's registers for both: three registers loaded, the command's dispatch, its flags, one read back,
 * twice a polygon. What the routine leaves in the GTE is put back at its end: the last polygon's corners through
 * NCLIP and the last triangle's depths through AVSZ3, in the order they came, as the game's own commands leave SXY0-2,
 * SZ1-3, MAC0, OTZ and FLAG. (GTE_BATCH_CHECK builds: every polygon's two values against the GTE's own.) */
#if !defined(PORT_NDS) && !defined(FM_LINK_GAME) /* (FM_LINK_GAME: the game's own, to compare a build with) */
#include "gte.h"
PORT_OVERLAY_CODE(0a) static void fm_link(ModelObj *model, u8 group) {
    u8 *m = (u8 *)model;
    u8 *grp = *(u8 **)(*(u8 **)(m + 8) + 0x10) + group * 0x28;
    u8 *pkt = *(u8 **)(*(u8 **)(m + 0x44) + group * 12 + 4);
    u8 *poly = *(u8 **)(grp + 0x1C);
    GameData *gd = g_gameState->gameData;
    u32 *ot = gd->ot[gd->drawBuf];
    if (gd->drawBuf != 0) pkt += *(u16 *)grp;
    ot -= (s8)grp[0x13];
    const s32 zsf3 = (s16)gte_cfc2(29);
    u32 last_xy[3] = {0, 0, 0}, last_z[3] = {0, 0, 0};
    unsigned seq = 0, xy_seq = 0, z_seq = 0;
    for (int k = 0; k < 8; k++) {
        const int quad = !(k & 1);
        for (s32 n = *(u16 *)(grp + 2 + 2 * k); n > 0; n--, poly += kind_src[k], pkt += kind_size[k]) {
            const u32 i0 = *(u16 *)(poly + 0), i1 = *(u16 *)(poly + 2), i2 = *(u16 *)(poly + 4);
            const u32 i3 = quad ? *(u16 *)(poly + 6) : 0;
            const u32 xy0 = (u32)g_vtxScreenXY[i0], xy1 = (u32)g_vtxScreenXY[i1], xy2 = (u32)g_vtxScreenXY[i2];
            u32 z;
            if (!(poly[kind_flags[k]] & 2)) {
                *(u32 *)pkt = (u32)kind_len[k] << 24;
                const s32 x0 = (s16)xy0, y0 = (s32)xy0 >> 16, x1 = (s16)xy1, y1 = (s32)xy1 >> 16, x2 = (s16)xy2, y2 = (s32)xy2 >> 16;
                const s32 nclip = (s32)((int64_t)x0 * y1 + (int64_t)x1 * y2 + (int64_t)x2 * y0 - (int64_t)x0 * y2 - (int64_t)x1 * y0 - (int64_t)x2 * y1);
                last_xy[0] = xy0, last_xy[1] = xy1, last_xy[2] = xy2, xy_seq = ++seq;
#ifdef GTE_BATCH_CHECK
                { s32 real; gte_ldsxy3(xy0, xy1, xy2); gte_nclip(); gte_stopz(&real);
                  if (real != nclip) port_fatal("fm_link check: NCLIP %08x here, %08x the GTE's", (unsigned)nclip, (unsigned)real); }
#endif
                if (nclip <= 0) continue;
            }
            *(u32 *)(pkt + kind_xy[k][0]) = xy0;
            *(u32 *)(pkt + kind_xy[k][1]) = xy1;
            *(u32 *)(pkt + kind_xy[k][2]) = xy2;
            if (quad) {
                *(u32 *)(pkt + kind_xy[k][3]) = (u32)g_vtxScreenXY[i3];
                z = ((u32)g_vtxScreenZ[i0] + g_vtxScreenZ[i1] + g_vtxScreenZ[i2] + g_vtxScreenZ[i3]) >> 4;
            } else {
                const u32 z0 = (u32)g_vtxScreenZ[i0], z1 = (u32)g_vtxScreenZ[i1], z2 = (u32)g_vtxScreenZ[i2];
                const int64_t m0 = (int64_t)zsf3 * (s32)((z0 & 0xFFFF) + (z1 & 0xFFFF) + (z2 & 0xFFFF));
                const int64_t q = m0 >> 12;
                z = q > 0xFFFF ? 0xFFFF : q < 0 ? 0 : (u32)q;
                last_z[0] = z0, last_z[1] = z1, last_z[2] = z2, z_seq = ++seq;
#ifdef GTE_BATCH_CHECK
                { u32 real; gte_ldsz3(z0, z1, z2); gte_avsz3(); gte_stotz(&real);
                  if (real != z) port_fatal("fm_link check: AVSZ3 %x here, %x the GTE's", (unsigned)z, (unsigned)real); }
#endif
            }
            u32 *slot = &ot[z];
            if (PSX_OT_OUTSIDE(slot)) continue; /* (past the table: the PS1 never draws it, psx_mem.h) */
            *(u32 *)pkt = *slot | ((u32)kind_len[k] << 24);
            *slot = (u32)pkt & 0xFFFFFF;
        }
    }
    /* (the GTE as the game's commands leave it) */
    for (int pass = 0; pass < 2; pass++) {
        const int nclip_now = xy_seq && (pass == 0) == (xy_seq < z_seq || !z_seq);
        const int avsz_now = z_seq && (pass == 0) == (z_seq < xy_seq || !xy_seq);
        if (nclip_now) { gte_ldsxy3(last_xy[0], last_xy[1], last_xy[2]); gte_nclip(); }
        if (avsz_now) { gte_ldsz3(last_z[0], last_z[1], last_z[2]); gte_avsz3(); }
    }
}
#else
#define fm_link o0a_fldmdl_drawMesh_link
#endif

PORT_OVERLAY_CODE(0a) void o0a_fldmdl_drawMesh(ModelObj *model, u8 group) {
    GameData *gd = g_gameState->gameData;
    u8 *grp = (u8 *)&model->model->groups[group];
    u8 *pkt = *(u8 **)((u8 *)model->groupOut + group * 12 + 4);
    if (gd->drawBuf != 0) pkt += *(u16 *)grp;
    void port_model_packets(const void *lo, const void *hi); /* (render.c: model polygons, for the 3DS's outlines) */
    port_model_packets(pkt, pkt + *(u16 *)grp);
    NativeFieldMesh n;
    n.model = model;
    n.group = grp;
    n.packets = pkt;
    n.ot = (const unsigned int *)(gd->ot[gd->drawBuf] - (s8)grp[0x13]);
    n.sxy = g_vtxScreenXY;
    n.sz = g_vtxScreenZ;
    n.nverts = 0;
    for (u32 i = 0; i < model->model->boneCount; ++i) n.nverts += ((u16 *)((WmModelGroup *)grp)->boneVertexCounts)[i];
    n.draw_x = g_gameState->drawEnv[gd->drawBuf].clip.x;
    n.draw_y = g_gameState->drawEnv[gd->drawBuf].clip.y;
    n.clip_y = 0;
    n.mirror = 0;
    int taken = port_native_field_mesh(&n);
    if (taken >= 2) link_translucent(grp, pkt, (u32 *)n.ot, 0, 0);
    if (taken) return;
    fm_link(model, group);
    depth_to_packets(grp, pkt);
}

/* fldmdl_drawMeshClipY (tu_800B61A4.c, renamed by config/renames.txt): a group cut at the model's clipY, as
 * fldmdl_drawMesh but for the polygons with a flagged vertex, the OT without the group's bias, and the triangles'
 * entries from their depths' sum / 3 */
void o0a_fldmdl_drawMeshClipY_game(ModelObj *model, u8 group);
PORT_OVERLAY_CODE(0a) void o0a_fldmdl_drawMeshClipY(ModelObj *model, u8 group) {
    GameData *gd = g_gameState->gameData;
    u8 *grp = (u8 *)&model->model->groups[group];
    u8 *pkt = *(u8 **)((u8 *)model->groupOut + group * 12 + 4);
    if (gd->drawBuf != 0) pkt += *(u16 *)grp;
    void port_model_packets(const void *lo, const void *hi); /* (render.c: model polygons, for the 3DS's outlines) */
    port_model_packets(pkt, pkt + *(u16 *)grp);
    NativeFieldMesh n;
    n.model = model;
    n.group = grp;
    n.packets = pkt;
    n.ot = (const unsigned int *)gd->ot[gd->drawBuf];
    n.sxy = g_vtxScreenXY;
    n.sz = g_vtxScreenZ;
    n.nverts = 0;
    for (u32 i = 0; i < model->model->boneCount; ++i) n.nverts += ((u16 *)((WmModelGroup *)grp)->boneVertexCounts)[i];
    n.draw_x = g_gameState->drawEnv[gd->drawBuf].clip.x;
    n.draw_y = g_gameState->drawEnv[gd->drawBuf].clip.y;
    n.clip_y = 1;
    n.mirror = 0;
    int taken = port_native_field_mesh(&n);
    if (taken >= 2) link_translucent(grp, pkt, (u32 *)n.ot, 1, 0);
    if (taken) return;
    o0a_fldmdl_drawMeshClipY_game(model, group);
    depth_to_packets(grp, pkt);
}

/* fldmdl_drawMeshMirrored (tu_800B61A4.c, renamed by config/renames.txt): a reflection's group, the PS1's culling
 * reversed (NCLIP >= 0 culled), otherwise as fldmdl_drawMesh */
void o0a_fldmdl_drawMeshMirrored_game(ModelObj *model, u8 group);
PORT_OVERLAY_CODE(0a) void o0a_fldmdl_drawMeshMirrored(ModelObj *model, u8 group) {
    GameData *gd = g_gameState->gameData;
    u8 *grp = (u8 *)&model->model->groups[group];
    u8 *pkt = *(u8 **)((u8 *)model->groupOut + group * 12 + 4);
    if (gd->drawBuf != 0) pkt += *(u16 *)grp;
    void port_model_packets(const void *lo, const void *hi); /* (render.c: model polygons, for the 3DS's outlines) */
    port_model_packets(pkt, pkt + *(u16 *)grp);
    NativeFieldMesh n;
    n.model = model;
    n.group = grp;
    n.packets = pkt;
    n.ot = (const unsigned int *)(gd->ot[gd->drawBuf] - (s8)grp[0x13]);
    n.sxy = g_vtxScreenXY;
    n.sz = g_vtxScreenZ;
    n.nverts = 0;
    for (u32 i = 0; i < model->model->boneCount; ++i) n.nverts += ((u16 *)((WmModelGroup *)grp)->boneVertexCounts)[i];
    n.draw_x = g_gameState->drawEnv[gd->drawBuf].clip.x;
    n.draw_y = g_gameState->drawEnv[gd->drawBuf].clip.y;
    n.clip_y = 0;
    n.mirror = 1;
    int taken = port_native_field_mesh(&n);
    if (taken >= 2) link_translucent(grp, pkt, (u32 *)n.ot, 0, 1);
    if (taken) return;
    o0a_fldmdl_drawMeshMirrored_game(model, group);
    depth_to_packets(grp, pkt);
}
