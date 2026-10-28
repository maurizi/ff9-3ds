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
    gte_SetGeomScreen(g_gameState->gameData->unkA);
    s32 k = 0;
    u32 cnt = arg0->model->boneCount;
    MATRIX *bone = (MATRIX *)arg0->boneMatrices;
    SVECTOR *last = NULL;
    s32 last_k = 0;
    for (u32 i = 0; i < cnt; bone++) {
        s32 n = ((u16 *)g->boneVertexCounts)[i++];
        if (n == 0) continue;
        /* the bone's matrix: m = RT x bone (columns through rtir), t = RT x bone.t + TR (rtv0tr) */
        gte_SetRotMatrix(&g_gameState->gameData->unk18);
        gte_SetTransMatrix(&g_gameState->gameData->unk18);
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
PORT_OVERLAY_CODE(0a) static void link_translucent(u8 *grp, u8 *pkt, u32 *ot, int clip_y, int mirror) {
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
    if (gd->unk8 != 0) pkt += *(u16 *)grp;
    NativeFieldMesh n;
    n.model = model;
    n.group = grp;
    n.packets = pkt;
    n.ot = (const unsigned int *)(gd->unk10[gd->unk8] - (s8)grp[0x13]);
    n.sxy = g_vtxScreenXY;
    n.sz = g_vtxScreenZ;
    n.nverts = 0;
    for (u32 i = 0; i < model->model->boneCount; ++i) n.nverts += ((u16 *)((WmModelGroup *)grp)->boneVertexCounts)[i];
    n.draw_x = g_gameState->drawEnv[gd->unk8].clip.x;
    n.draw_y = g_gameState->drawEnv[gd->unk8].clip.y;
    n.clip_y = 0;
    n.mirror = 0;
    int taken = port_native_field_mesh(&n);
    if (taken == 2) link_translucent(grp, pkt, (u32 *)n.ot, 0, 0);
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
    if (gd->unk8 != 0) pkt += *(u16 *)grp;
    NativeFieldMesh n;
    n.model = model;
    n.group = grp;
    n.packets = pkt;
    n.ot = (const unsigned int *)gd->unk10[gd->unk8];
    n.sxy = g_vtxScreenXY;
    n.sz = g_vtxScreenZ;
    n.nverts = 0;
    for (u32 i = 0; i < model->model->boneCount; ++i) n.nverts += ((u16 *)((WmModelGroup *)grp)->boneVertexCounts)[i];
    n.draw_x = g_gameState->drawEnv[gd->unk8].clip.x;
    n.draw_y = g_gameState->drawEnv[gd->unk8].clip.y;
    n.clip_y = 1;
    n.mirror = 0;
    int taken = port_native_field_mesh(&n);
    if (taken == 2) link_translucent(grp, pkt, (u32 *)n.ot, 1, 0);
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
    if (gd->unk8 != 0) pkt += *(u16 *)grp;
    NativeFieldMesh n;
    n.model = model;
    n.group = grp;
    n.packets = pkt;
    n.ot = (const unsigned int *)(gd->unk10[gd->unk8] - (s8)grp[0x13]);
    n.sxy = g_vtxScreenXY;
    n.sz = g_vtxScreenZ;
    n.nverts = 0;
    for (u32 i = 0; i < model->model->boneCount; ++i) n.nverts += ((u16 *)((WmModelGroup *)grp)->boneVertexCounts)[i];
    n.draw_x = g_gameState->drawEnv[gd->unk8].clip.x;
    n.draw_y = g_gameState->drawEnv[gd->unk8].clip.y;
    n.clip_y = 0;
    n.mirror = 1;
    int taken = port_native_field_mesh(&n);
    if (taken == 2) link_translucent(grp, pkt, (u32 *)n.ot, 0, 1);
    if (taken) return;
    o0a_fldmdl_drawMeshMirrored_game(model, group);
}
