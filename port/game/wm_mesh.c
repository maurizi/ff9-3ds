/* World map model vertex projection with the per-vertex loop in port code (config/overrides.txt): wm_projectMesh
 * (ovl_0c) is the field's fldmdl_projectMesh and the battle's btl_projectMesh routine, overridden as they are
 * (field_mesh.c, mesh_native.c). The bone matrices are set up through the GTE as the game wrote it; the vertices go
 * through gte_rtpt_batch (gte.c), and the last group runs through the GTE again, so the registers end as the game
 * leaves them.
 */
#include "psyq_port.h"
#include "ovl_0c.h"
#include "port_hw.h"
#include "native_mesh.h"

void o0c_wm_projectMesh(ModelObj *arg0, u8 arg1);

PORT_OVERLAY_CODE(0c) void o0c_wm_projectMesh(ModelObj *arg0, u8 arg1) {
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

/* a world map model's mesh group just drawn (wm_drawMesh, wm_drawMeshClipY): its packets, the half of the group's
 * buffer for the frame buffer drawn into, registered as a model's (render.c: in stereo one depth, not grown) */
void port_model_packets(const void *lo, const void *hi);
void port_wm_mesh_packets(ModelObj *model, u8 group) {
    u8 *m = (u8 *)model;
    u8 *grp = *(u8 **)(*(u8 **)(m + 8) + 0x10) + group * 0x28;
    u8 *pkt = *(u8 **)(*(u8 **)(m + 0x44) + group * 12 + 4);
    u16 half = *(u16 *)grp;
    if (g_gameState->gameData->drawBuf != 0) pkt += half;
    port_model_packets(pkt, pkt + half);
}
