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

/* A battle unit's model (party, enemy, weapon) has its triangle lists put in strip order once (strip_order): the
 * hybrid renderer draws a model's opaque triangles as the 3D engine's triangle strips (gx_hybrid.c native_strips),
 * where a triangle sharing an edge with the one before costs one vertex instead of three. The order only decides
 * which of two polygons at the same OT entry the PS1 draws first. Not effect models (modules index their split
 * vertices in list order) nor the Zombie's (model 14: effect 0x17F numbers its polygons, docs R26). */
#define STRIP_ORDERED 0x8000 /* BtlUnit9C8.flags: done (the game reads bit 0 only; a reloaded file starts clear) */
#ifdef PORT_NDS
#define STRIP_COLD __attribute__((target("thumb")))
#else
#define STRIP_COLD
#endif
#define STRIP_WINDOW 16      /* records searched each side of the last one (the lists are mostly in surface order) */

/* the FT3 at r shares the edge a-b (those uvs) and has a third vertex: it (the vertices compared first, the uvs only
 * then) */
PORT_OVERLAY_CODE(0b) STRIP_COLD static int strip_next(const u8 *r, const u16 *uvs, u32 a, u32 b, u16 ua, u16 ub, u32 *c, u16 *uc) {
    const u32 v0 = *(const u16 *)r, v1 = *(const u16 *)(r + 2), v2 = *(const u16 *)(r + 4);
    const int ia = v0 == a ? 0 : v1 == a ? 1 : v2 == a ? 2 : -1;
    if (ia < 0) return 0;
    const int ib = v0 == b ? 0 : v1 == b ? 1 : v2 == b ? 2 : -1;
    if (ib < 0 || ib == ia) return 0;
    const int ic = 3 - ia - ib;
    const u32 vc = ic == 0 ? v0 : ic == 1 ? v1 : v2;
    if (vc == a || vc == b) return 0;
    const u16 *ui = (const u16 *)(r + 0xC);
    if (uvs[ui[ia]] != ua || uvs[ui[ib]] != ub) return 0;
    *c = vc; *uc = uvs[ui[ic]];
    return 1;
}

/* the same texture, flags and colour */
PORT_OVERLAY_CODE(0b) STRIP_COLD static int strip_same(const u8 *r, const u8 *q) {
    return r[6] == q[6] && r[0x12] == q[0x12] && r[8] == q[8] && r[9] == q[9] && r[10] == q[10];
}

/* from record s (rotation rot), the strip greedily: the records it takes marked `mark` in used[], their count; with
 * order, appended there */
PORT_OVERLAY_CODE(0b) STRIP_COLD static int strip_follow(const u8 *list, int n, const u16 *uvs, u8 *used, int s, int rot, u8 mark, u16 *order) {
    const u8 *r = list + s * 0x14;
    const u32 w[3] = {*(const u16 *)r, *(const u16 *)(r + 2), *(const u16 *)(r + 4)};
    const u16 wu[3] = {uvs[*(const u16 *)(r + 0xC)], uvs[*(const u16 *)(r + 0xE)], uvs[*(const u16 *)(r + 0x10)]};
    u32 a = w[(rot + 1) % 3], b = w[(rot + 2) % 3];
    u16 ua = wu[(rot + 1) % 3], ub = wu[(rot + 2) % 3];
    int cur = s, len = 0;
    for (;;) {
        int lo = cur - STRIP_WINDOW < 0 ? 0 : cur - STRIP_WINDOW, hi = cur + STRIP_WINDOW > n ? n : cur + STRIP_WINDOW;
        int found = -1;
        u32 c = 0;
        u16 uc = 0;
        for (int j = lo; j < hi; ++j)
            if (!used[j] && strip_same(list + j * 0x14, r) && strip_next(list + j * 0x14, uvs, a, b, ua, ub, &c, &uc)) {
                found = j;
                break;
            }
        if (found < 0) break;
        used[found] = mark;
        if (order) *order++ = (u16)found;
        ++len;
        a = b; ua = ub; b = c; ub = uc; cur = found;
    }
    return len;
}

/* each FT3's link in its last byte (padding the game never reads), for the renderer's strips (gx_hybrid.c
 * strip_perm): bit 0 it continues the one before (the strips as the list now runs, all of them drawn), bits 1-3 its
 * corners in strip order (the edge it shares first, the vertex it brings last; a strip's first triangle turned so that
 * the edge the next one shares is last) */
static const u8 link_perm[6][3] = {{0, 1, 2}, {1, 2, 0}, {2, 0, 1}, {0, 2, 1}, {1, 0, 2}, {2, 1, 0}};
PORT_OVERLAY_CODE(0b) STRIP_COLD static void strip_links(u8 *list, int n, const u16 *uvs) {
    u32 a = 0, b = 0;
    u16 ua = 0, ub = 0;
    for (int k = 0; k < n; ++k) {
        u8 *r = list + k * 0x14;
        const u32 w[3] = {*(const u16 *)r, *(const u16 *)(r + 2), *(const u16 *)(r + 4)};
        u32 c;
        u16 uc;
        if (k && strip_same(r, r - 0x14) && strip_next(r, uvs, a, b, ua, ub, &c, &uc)) {
            const int ia = w[0] == a ? 0 : w[1] == a ? 1 : 2, ib = w[0] == b ? 0 : w[1] == b ? 1 : 2, ic = 3 - ia - ib;
            int p = 0;
            while (link_perm[p][0] != ia || link_perm[p][1] != ib || link_perm[p][2] != ic) ++p;
            r[0x13] = (u8)(1 | p << 1);
            a = b; ua = ub; b = c; ub = uc;
            continue;
        }
        int rot = 0; /* (a strip's first: the rotation whose last edge the next one shares) */
        if (k + 1 < n && strip_same(r + 0x14, r)) {
            const u16 *ui = (const u16 *)(r + 0xC);
            for (int i = 0; i < 3; ++i)
                if (strip_next(r + 0x14, uvs, w[(i + 1) % 3], w[(i + 2) % 3], uvs[ui[(i + 1) % 3]], uvs[ui[(i + 2) % 3]], &c, &uc)) {
                    rot = i;
                    break;
                }
        }
        r[0x13] = (u8)(rot << 1); /* (rotations 0-2 are link_perm 0-2) */
        const u16 *ui = (const u16 *)(r + 0xC);
        a = w[(rot + 1) % 3]; ua = uvs[ui[(rot + 1) % 3]];
        b = w[(rot + 2) % 3]; ub = uvs[ui[(rot + 2) % 3]];
    }
}

PORT_OVERLAY_CODE(0b) STRIP_COLD static void strip_order(BtlUnit9C8 *h) {
    h->flags |= STRIP_ORDERED;
    for (u32 g = 0; g < h->meshCount; ++g) {
        BtlMesh *m = &h->meshes[g];
        const int n = m->nFT3;
        /* scratch: the projection's screen arrays (filled again before anything reads them) */
        u16 *order = (u16 *)g_vtxScreenZ;
        u8 *used = (u8 *)g_vtxScreenXY;
        u8 *list = (u8 *)m->prims + m->nFT4 * 0x18;
        for (int k = 0; k < n; ++k) list[k * 0x14 + 0x13] = 0; /* (no links until they are worked out) */
        if (n < 3 || n > 1600) continue; /* (both arrays hold 0x321 words) */
        const u16 *uvs = (const u16 *)m->uvs;
        for (int k = 0; k < n; ++k) used[k] = 0;
        int out = 0;
        for (int s = 0; s < n; ++s) {
            if (used[s]) continue;
            used[s] = 1;
            order[out++] = (u16)s;
            /* the rotation whose last edge a record near it shares (none: as stored) */
            int best = 0;
            {
                const u8 *r = list + s * 0x14;
                const u32 w[3] = {*(const u16 *)r, *(const u16 *)(r + 2), *(const u16 *)(r + 4)};
                const u16 *ui = (const u16 *)(r + 0xC);
                const int lo = s - STRIP_WINDOW < 0 ? 0 : s - STRIP_WINDOW, hi = s + STRIP_WINDOW > n ? n : s + STRIP_WINDOW;
                for (int j = lo; j < hi && !best; ++j) {
                    if (used[j] || !strip_same(list + j * 0x14, r)) continue;
                    for (int rot = 0; rot < 3; ++rot) {
                        u32 c;
                        u16 uc;
                        if (strip_next(list + j * 0x14, uvs, w[(rot + 1) % 3], w[(rot + 2) % 3], uvs[ui[(rot + 1) % 3]],
                                       uvs[ui[(rot + 2) % 3]], &c, &uc)) { best = rot ? rot : 3; break; }
                    }
                }
                if (best == 3) best = 0;
            }
            out += strip_follow(list, n, uvs, used, s, best, 1, order + out);
        }
        /* the records into that order, cycle by cycle (one record of room) */
        for (int k = 0; k < n; ++k) used[k] = 0;
        for (int i = 0; i < n; ++i) {
            if (used[i] || order[i] == i) { used[i] = 1; continue; }
            u32 tmp[5];
            u32 *ri = (u32 *)(list + i * 0x14);
            for (int w = 0; w < 5; ++w) tmp[w] = ri[w];
            int j = i;
            for (;;) {
                const int k = order[j];
                used[j] = 1;
                u32 *rj = (u32 *)(list + j * 0x14);
                if (k == i) { for (int w = 0; w < 5; ++w) rj[w] = tmp[w]; break; }
                const u32 *rk = (const u32 *)(list + k * 0x14);
                for (int w = 0; w < 5; ++w) rj[w] = rk[w];
                j = k;
            }
        }
        strip_links(list, n, uvs);
    }
}

/* a battle unit's own model or weapon (not an effect's, not the Zombie's) */
PORT_OVERLAY_CODE(0b) STRIP_COLD static int strip_unit_model(const BtlUnit9C *m) {
    Battle *bt = g_gameState->gameData->ctx.battle;
    if (!bt) return 0;
    for (int i = 0; i < 8; ++i) {
        const BtlUnit *u = &bt->units[i];
        if (u->weaponModel == m) return 1;
        if (u->model == m) return !(i >= 4 && u->modelId == 14);
    }
    return 0;
}

void o0b_btl_projectMesh(BtlUnit9C *arg0, u8 arg1) {
    MATRIX m;
    if (!(arg0->header->flags & STRIP_ORDERED) && strip_unit_model(arg0)) strip_order(arg0->header);
    BtlMesh *g = &arg0->header->meshes[arg1];
    gte_SetGeomScreen(g_gameState->gameData->projDist);
    s32 k = 0;
    u32 cnt = arg0->header->boneCount;
    MATRIX *bone = (MATRIX *)arg0->bones;
    SVECTOR *last = NULL;
    s32 last_k = 0;
    for (u32 i = 0; i < cnt; bone++) {
        s32 n = g->vertexCounts[i++];
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
    n.strips = (m->header->flags & STRIP_ORDERED) != 0;
    for (u32 i = 0; i < m->header->boneCount; ++i) n.nverts += g->vertexCounts[i];
    /* the buffer this frame's OT is drawn into (btl_endFrame: shown until then) */
    n.draw_x = g_gameState->drawEnv[g_gameState->gameData->drawBuf].clip.x;
    n.draw_y = g_gameState->drawEnv[g_gameState->gameData->drawBuf].clip.y;
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
    n.verts = (const unsigned char *)grp + grp->vertexOfs + rec->firstVertex * 6;
    n.textab = (const unsigned int *)((u8 *)grp + grp->texOfs);
    n.ot = (const unsigned int *)ot;
    n.bright = *(u16 *)&g_btlbgBrightness;
    n.shift = *(u16 *)&g_btlbgOtShift & 31;
    n.draw_x = g_gameState->drawEnv[g_gameState->gameData->drawBuf].clip.x;
    n.draw_y = g_gameState->drawEnv[g_gameState->gameData->drawBuf].clip.y;
    if (port_native_bg(&n)) return out;
    return o0b_btlbg_drawRecord_packets(rec, out, ot, grp);
}
