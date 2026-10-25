/* Model vertex projection with the per-vertex loop in port code (config/overrides.txt).
 *
 * btl_projectMesh (ovl_0b) sets each bone's matrix up through the GTE, then projects its vertices three at a time
 * (gte_ldv3c, gte_rtpt, gte_stsxy3c, gte_stsz3c). The matrix setup stays as the game wrote it; the vertices go
 * through gte_rtpt_batch (gte.c), which on the DS skips the register file and flags for a loop over the matrix in
 * registers (the user's choice of 2026-10-25: approximate geometry for speed). The last group runs through the GTE
 * as before, so the registers end as the game left them.
 */
#include "psyq_port.h"
#include "ovl_0b.h"
#include "port_hw.h"

void o0b_btl_projectMesh(BtlUnit9C *arg0, u8 arg1);

void o0b_btl_projectMesh(BtlUnit9C *arg0, u8 arg1) {
    MATRIX m;
    BtlMesh *g = &arg0->header->meshes[arg1];
    gte_SetGeomScreen(g_gameState->gameData->unkA);
    s32 k = 0;
    u32 cnt = arg0->header->unk2;
    MATRIX *bone = (MATRIX *)arg0->bones;
    SVECTOR *last = NULL;
    s32 last_k = 0;
    for (u32 i = 0; i < cnt; bone++) {
        s32 n = g->vertexCounts[i++];
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
        s32 groups = (n + 2) / 3;
        gte_rtpt_batch((const gte_u32 *)&g->vertices[k], groups * 3, (gte_u32 *)&g_vtxScreenXY[k],
                       (gte_u32 *)&g_vtxScreenZ[k]);
        last = &g->vertices[k + (groups - 1) * 3];
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
