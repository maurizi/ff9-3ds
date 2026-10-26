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
    n.bones = model->boneMatrices;
    n.camera = &gd->unk18;
    n.bone_count = model->model->boneCount;
    n.geom_screen = gd->unkA;
    n.draw_x = g_gameState->drawEnv[gd->unk8].clip.x;
    n.draw_y = g_gameState->drawEnv[gd->unk8].clip.y;
    if (port_native_field_mesh(&n)) return;
    o0a_fldmdl_drawMesh_link(model, group);
}
