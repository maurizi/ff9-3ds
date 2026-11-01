/* Field model vertex projection with the per-vertex loop in port code (config/overrides.txt), as
 * btl_projectMesh's (mesh_native.c): fldmdl_projectMesh is the same routine over the field's model groups. The bone
 * matrices are set up through the GTE as the game wrote it; the vertices go through gte_rtpt_batch (gte.c), and the
 * last group runs through the GTE again, so the registers end as the game leaves them.
 */
#include "psyq_port.h"
#include "ovl_0a.h"
#include "port_hw.h"
#include "native_mesh.h"

void o0a_fldmdl_projectMesh(ModelObj *arg0, u8 arg1);

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
        /* the bone's matrix: m = RT x bone (columns through rtir), t = RT x bone.t + TR (rtv0tr) */
        gte_SetRotMatrix(&g_gameState->gameData->viewMatrix);
        gte_SetTransMatrix(&g_gameState->gameData->viewMatrix);
        gte_ldclmv(bone);
        gte_rtir_b();
        gte_stclmv(&m);
        gte_ldclmv(&bone->m[0][1]);
        gte_rtir();
        gte_stclmv(&m.m[0][1]);
        gte_ldclmv(&bone->m[0][2]);
        gte_rtir_n1();
        gte_stclmv(&m.m[0][2]);
        gte_ldlv0(bone->t);
        gte_rtv0tr_n1();
        gte_stlvnl(m.t);
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
    o0a_fldmdl_drawMesh_link(model, group);
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
}
