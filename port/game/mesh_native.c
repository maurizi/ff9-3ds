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
#include "native_mesh.h"

void o0b_btl_projectMesh(BtlUnit9C *arg0, u8 arg1);

void o0b_btl_projectMesh(BtlUnit9C *arg0, u8 arg1) {
    MATRIX m;
    BtlMesh *g = &arg0->header->meshes[arg1];
    gte_SetGeomScreen(g_gameState->gameData->unkA);
    s32 k = 0;
    u32 cnt = arg0->header->boneCount;
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

/* Without the hybrid renderer (PC, the DS's software paths) every mesh draws its packets. */
__attribute__((weak)) int port_native_mesh(const NativeMesh *m) { (void)m; return 0; }
__attribute__((weak)) int port_native_bg(const NativeBg *r) { (void)r; return 0; }

/* btl_drawMesh: the game's packets (mesh_packets.c, renamed by config/renames.txt) unless the renderer takes the
 * mesh, in which case only the bookkeeping the packets' function does stays: the model's first packet, the terminator
 * byte, out + 8. */
u8 *o0b_btl_drawMesh_packets(BtlUnit9C *m, u8 meshIdx, u8 *out);
u8 *o0b_btl_drawMesh(BtlUnit9C *m, u8 meshIdx, u8 *out);

u8 *o0b_btl_drawMesh(BtlUnit9C *m, u8 meshIdx, u8 *out) {
    if (m->flags & 0x10) return o0b_btl_drawMesh_packets(m, meshIdx, out);
    BtlMesh *g = &m->header->meshes[meshIdx];
    NativeMesh n;
    n.src = (const unsigned char *)g->prims;
    n.count[0] = g->nFT4; n.count[1] = g->nFT3; n.count[2] = g->nGT4; n.count[3] = g->nGT3;
    n.count[4] = g->nG4; n.count[5] = g->nG3; n.count[6] = g->nF4; n.count[7] = g->nF3;
    n.uvs = (const unsigned short *)g->uvs;
    n.colors = (const unsigned int *)g->colors;
    n.tex = (const unsigned int *)m->texPages;
    n.ot_bias = g->otBias;
    n.sxy = (const unsigned int *)g_vtxScreenXY;
    n.sz = (const unsigned int *)g_vtxScreenZ;
    n.nverts = 0;
    for (u32 i = 0; i < m->header->boneCount; ++i) n.nverts += g->vertexCounts[i];
    /* the buffer this frame's OT is drawn into (btl_endFrame: shown until then) */
    n.draw_x = g_gameState->drawEnv[g_gameState->gameData->unk8].clip.x;
    n.draw_y = g_gameState->drawEnv[g_gameState->gameData->unk8].clip.y;
    if (!port_native_mesh(&n)) return o0b_btl_drawMesh_packets(m, meshIdx, out);
    if (m->prims == 0) m->prims = (s32)out;
    out[7] = 0xFF;
    return out + 8;
}

/* btlbg_drawRecord: the record's packets (background.c, renamed) unless the renderer takes it; then nothing is
 * linked and the packet pointer stays where it was. */
u8 *o0b_btlbg_drawRecord_packets(BtlBgRec *rec, u8 *out, u32 *ot, BtlBgGroup *grp);
u8 *o0b_btlbg_drawRecord(BtlBgRec *rec, u8 *out, u32 *ot, BtlBgGroup *grp);

u8 *o0b_btlbg_drawRecord(BtlBgRec *rec, u8 *out, u32 *ot, BtlBgGroup *grp) {
    NativeBg n;
    n.rec = (const unsigned char *)rec;
    n.verts = (const unsigned char *)grp + grp->unkE + rec->unk4 * 6;
    n.textab = (const unsigned int *)((u8 *)grp + grp->unkA);
    n.ot = (const unsigned int *)ot;
    n.bright = *(u16 *)&g_btlbgBrightness;
    n.shift = *(u16 *)&g_btlbgOtShift & 31;
    n.draw_x = g_gameState->drawEnv[g_gameState->gameData->unk8].clip.x;
    n.draw_y = g_gameState->drawEnv[g_gameState->gameData->unk8].clip.y;
    if (port_native_bg(&n)) return out;
    return o0b_btlbg_drawRecord_packets(rec, out, ot, grp);
}
