/* The field's area map on the bottom screen (port/game/field_hud.c draws it in its box): the walkmesh seen from above
 * (each walkable triangle filled, its outer edges outlined), and the player as an arrow pointing where he faces. The
 * field's x runs right and its z away from the camera, so the map's up is the camera's forward: the map looks the
 * way the room does. Floors that move (lifts, bridges) move on the map too: it is rebuilt each frame. A walkmesh may
 * hold areas the camera doesn't show (Dali's field: the yard and, apart, the path beyond it): the map is the area
 * connected to the player's triangle, up to the exits (past an exit is reached only by leaving), scaled to fill the
 * box. The exits are marked: region objects (kind 3) whose tag-2
 * function (run while the player is inside) holds GOTO_FIELD (2B) or GOTO_WORLD (B6) with a literal destination, their
 * polygon (op 29: +0x14 count, +0x18 points) drawn in green (decomp-requests R30). */
#include "psyq_port.h"
#include "ovl_0a.h"

#define MAP_FILL 0x5888A8   /* walkable floor (BGR as the GPU takes it): sand */
#define MAP_EDGE 0xC8E0F0   /* its outline */
#define MAP_PLAYER 0x30E8F8 /* the player */
#define MAP_EXIT 0x48D860   /* an exit (a region whose "player inside" function changes field or goes to the world) */

static int bound_tri(const BgiTriangle *t) {
    return (t->flags & BGI_FLAG_ACTIVE) && !((t->flags >> 8) & BGI_MASK_NO_PLAYER);
}

static void world_xz(const BgiTriangle *t, int k, int *x, int *z) {
    const BgiHeader *h = g_fieldBgi.header;
    const BgiFloor *f = &g_fieldBgi.floors[t->floor];
    const BgiVertex *v = &g_fieldBgi.vertices[t->vertices[k]];
    *x = h->curPos[0] + (s16)f->curPos[0] + v->vx;
    *z = h->curPos[2] + (s16)f->curPos[2] + v->vz;
}

#define MAX_TRIS 1024
static u8 near_player[MAX_TRIS]; /* triangles connected to the player's (walkable neighbours) */

/* the field's exits (their polygons), collected before the map is built */
static struct { int n; s16 x[8], z[8]; } exits[16];
static int n_exits;
static int region_is_exit(int entry);

static void collect_exits(void) {
    n_exits = 0;
    for (EvtListNode *nd = evt_getObjectList(); nd && n_exits < 16; nd = nd->next) {
        EvtObject *o = nd->obj;
        if (!o || o->kind != 3) continue;
        Unk80039C38Quad *q = (Unk80039C38Quad *)o;
        if (q->unk14 < 3 || q->unk14 > 8 || !region_is_exit(o->scriptIdx)) continue;
        exits[n_exits].n = q->unk14;
        for (int k = 0; k < q->unk14; k++) exits[n_exits].x[k] = q->unk18[k].unk0, exits[n_exits].z[k] = q->unk18[k].unk2;
        n_exits++;
    }
}

/* whether (x, z) is in an exit (convex polygons, either winding, edges included: as evt_isInRegion) */
static int in_exit(int x, int z) {
    for (int e = 0; e < n_exits; e++) {
        int pos = 0, neg = 0, n = exits[e].n;
        for (int i = 0; i < n; i++) {
            int j = (i + 1) % n;
            long c = (long)(exits[e].z[j] - exits[e].z[i]) * (x - exits[e].x[i]) - (long)(exits[e].x[j] - exits[e].x[i]) * (z - exits[e].z[i]);
            pos |= c > 0;
            neg |= c < 0;
        }
        if (!(pos && neg)) return 1;
    }
    return 0;
}

static void world_xz(const BgiTriangle *t, int k, int *x, int *z);
/* a triangle whose centre is in an exit: the walk ends there (what lies past it is reached by leaving) */
static int tri_in_exit(const BgiTriangle *t) {
    int cx = 0, cz = 0;
    for (int k = 0; k < 3; k++) {
        int x, z;
        world_xz(t, k, &x, &z);
        cx += x;
        cz += z;
    }
    return in_exit(cx / 3, cz / 3);
}

static void mark_connected(int n, int start) {
    static s16 stack[MAX_TRIS];
    __builtin_memset(near_player, 0, sizeof near_player);
    if (start < 0 || start >= n) { __builtin_memset(near_player, 1, sizeof near_player); return; }
    int sp = 0;
    stack[sp++] = (s16)start;
    near_player[start] = 1;
    while (sp) {
        const BgiTriangle *t = &g_fieldBgi.triangles[stack[--sp]];
        for (int k = 0; k < 3; k++) {
            int nb = t->neighbors[k];
            if (nb < 0 || nb >= n || near_player[nb] || !bound_tri(&g_fieldBgi.triangles[nb]) ||
                tri_in_exit(&g_fieldBgi.triangles[nb]))
                continue;
            near_player[nb] = 1;
            stack[sp++] = (s16)nb;
        }
    }
}

/* the player's walkmesh triangle, -1 if unknown */
static int player_triangle(EvtObject *pl) {
    for (int i = 0; pl && i < g_fieldBgi.actorCount; i++)
        if ((EvtObject *)g_fieldBgi.actors[i].actor == pl) return g_fieldBgi.actors[i].triangle;
    return -1;
}

/* whether the script entry's tag-2 function goes to another field or the world map (a literal 2B / B6 in it) */
static int region_is_exit(int entry) {
    u8 *h = g_evtScripts + *(u16 *)(g_evtScripts + entry * 8);
    int size = *(u16 *)(g_evtScripts + entry * 8 + 2), n = *(u16 *)h >> 8;
    u16 *p = (u16 *)(h + 2);
    for (int i = 0; i < n; i++) {
        if (p[i * 2] != 2) continue;
        /* the function runs to the next function's start, or the entry's end */
        int start = p[i * 2 + 1] + 2, end = size;
        for (int j = 0; j < n; j++)
            if (p[j * 2 + 1] + 2 > start && p[j * 2 + 1] + 2 < end) end = p[j * 2 + 1] + 2;
        for (int k = start; k + 3 < end; k++)
            if ((h[k] == 0x2B || h[k] == 0xB6) && h[k + 1] == 0x00) return 1;
        return 0;
    }
    return 0;
}

/* edges shared by two drawn triangles (by world position: a walkmesh's floors don't share vertex indices) are inside
 * the area; the rest are its outline */
#define EDGE_HASH 4096
static struct { s16 x0, z0, x1, z1; u8 count; } edges[EDGE_HASH];
static unsigned edge_slot(int x0, int z0, int x1, int z1) {
    if (x1 < x0 || (x1 == x0 && z1 < z0)) { int t = x0; x0 = x1; x1 = t; t = z0; z0 = z1; z1 = t; }
    unsigned h = ((unsigned)x0 * 73856093u ^ (unsigned)z0 * 19349663u ^ (unsigned)x1 * 83492791u ^ (unsigned)z1 * 2654435761u) &
                 (EDGE_HASH - 1);
    for (int probe = 0; probe < EDGE_HASH; probe++, h = (h + 1) & (EDGE_HASH - 1)) {
        if (!edges[h].count) {
            edges[h].x0 = (s16)x0; edges[h].z0 = (s16)z0; edges[h].x1 = (s16)x1; edges[h].z1 = (s16)z1;
            return h;
        }
        if (edges[h].x0 == x0 && edges[h].z0 == z0 && edges[h].x1 == x1 && edges[h].z1 == z1) return h;
    }
    return 0;
}

static inline u32 xy(int x, int y) { return (u32)(x & 0xFFFF) | (u32)y << 16; }

/* The map in the box (bx, by, bw, bh): packets from pkt linked into ot[0] (drawn above the box), returns the next
 * free byte; pkt_end: where the buffer ends. Nothing without a walkmesh. */
u8 *port_field_map_draw(u32 *ot, u8 *pkt, u8 *pkt_end, int bx, int by, int bw, int bh) {
    if (!g_fieldBgi.loaded || !g_fieldBgi.header || !g_fieldBgi.triangles) return pkt;
    const int n = g_fieldBgi.header->triangleCount;
    if (n > MAX_TRIS) return pkt;
    GameData *g = g_gameState->gameData;
    EvtObject *pl = g->ctx.field ? (EvtObject *)g->npcModels[g->ctx.field->playerObj].actor : NULL;
    collect_exits();
    mark_connected(n, player_triangle(pl));
    int x0 = 0x7FFFFFFF, z0 = 0x7FFFFFFF, x1 = -0x7FFFFFFF, z1 = -0x7FFFFFFF;
    for (int i = 0; i < n; i++) {
        const BgiTriangle *t = &g_fieldBgi.triangles[i];
        if (!bound_tri(t) || !near_player[i]) continue;
        for (int k = 0; k < 3; k++) {
            int x, z;
            world_xz(t, k, &x, &z);
            if (x < x0) x0 = x;
            if (x > x1) x1 = x;
            if (z < z0) z0 = z;
            if (z > z1) z1 = z;
        }
    }
    if (x1 <= x0 || z1 <= z0) return pkt;
    /* the exits that touch the area fit in too */
    {
        int ax0 = x0, az0 = z0, ax1 = x1, az1 = z1;
        for (int e = 0; e < n_exits; e++) {
            int ex0 = 0x7FFFFFFF, ez0 = 0x7FFFFFFF, ex1 = -0x7FFFFFFF, ez1 = -0x7FFFFFFF;
            for (int k = 0; k < exits[e].n; k++) {
                if (exits[e].x[k] < ex0) ex0 = exits[e].x[k];
                if (exits[e].x[k] > ex1) ex1 = exits[e].x[k];
                if (exits[e].z[k] < ez0) ez0 = exits[e].z[k];
                if (exits[e].z[k] > ez1) ez1 = exits[e].z[k];
            }
            if (ex1 < ax0 || ex0 > ax1 || ez1 < az0 || ez0 > az1) continue; /* (another area's) */
            if (ex0 < x0) x0 = ex0;
            if (ex1 > x1) x1 = ex1;
            if (ez0 < z0) z0 = ez0;
            if (ez1 > z1) z1 = ez1;
        }
    }
    /* scale (16.16) to fit with a margin, centred */
    const int m = 8;
    int sx = ((bw - 2 * m) << 16) / (x1 - x0), sz = ((bh - 2 * m) << 16) / (z1 - z0), s = sx < sz ? sx : sz;
    const int ox = bx + bw / 2, oy = by + bh / 2, cx = (x0 + x1) / 2, cz = (z0 + z1) / 2;
    __builtin_memset(edges, 0, sizeof edges);
    for (int i = 0; i < n; i++) {
        const BgiTriangle *t = &g_fieldBgi.triangles[i];
        if (!bound_tri(t) || !near_player[i]) continue;
        int wx[3], wz[3];
        for (int k = 0; k < 3; k++) world_xz(t, k, &wx[k], &wz[k]);
        for (int k = 0; k < 3; k++) {
            unsigned e = edge_slot(wx[k], wz[k], wx[(k + 1) % 3], wz[(k + 1) % 3]);
            if (edges[e].count < 255) edges[e].count++;
        }
    }
#define MX(wx) (ox + (int)(((long long)((wx) - cx) * s) >> 16))
#define MY(wz) (oy - (int)(((long long)((wz) - cz) * s) >> 16))
    u32 *p = (u32 *)pkt;
    for (int i = 0; i < n; i++) {
        const BgiTriangle *t = &g_fieldBgi.triangles[i];
        if (!bound_tri(t) || !near_player[i]) continue; /* (only the player's area: the others are other screens) */
        if ((u8 *)(p + 5 + 3 * 4) > pkt_end) break;
        int x[3], y[3], wxs[3], wzs[3];
        for (int k = 0; k < 3; k++) {
            world_xz(t, k, &wxs[k], &wzs[k]);
            x[k] = MX(wxs[k]);
            y[k] = MY(wzs[k]);
        }
        /* the outline: edges no other drawn triangle shares */
        for (int k = 0; k < 3; k++) {
            if (edges[edge_slot(wxs[k], wzs[k], wxs[(k + 1) % 3], wzs[(k + 1) % 3])].count > 1) continue;
            int a = k, b = (k + 1) % 3;
            p[0] = 3u << 24;
            p[1] = 0x40000000u | MAP_EDGE;
            p[2] = xy(x[a], y[a]);
            p[3] = xy(x[b], y[b]);
            addPrim(ot, p);
            p += 4;
        }
        p[0] = 4u << 24;
        p[1] = 0x20000000u | MAP_FILL;
        p[2] = xy(x[0], y[0]);
        p[3] = xy(x[1], y[1]);
        p[4] = xy(x[2], y[2]);
        addPrim(ot, p); /* (after its edges in the list: added later, drawn first) */
        p += 5;
    }
    /* the exits */
    for (int e = 0; e < n_exits; e++) {
        int cnt = exits[e].n;
        if ((u8 *)(p + 5 * 6 + 4 * 8) > pkt_end) break;
        for (int k = 0; k < cnt; k++) { /* its outline */
            int j = (k + 1) % cnt;
            p[0] = 3u << 24;
            p[1] = 0x40000000u | MAP_EXIT;
            p[2] = xy(MX(exits[e].x[k]), MY(exits[e].z[k]));
            p[3] = xy(MX(exits[e].x[j]), MY(exits[e].z[j]));
            addPrim(ot - 1, p);
            p += 4;
        }
        for (int k = 1; k + 1 < cnt; k++) { /* filled (convex: a fan) */
            p[0] = 4u << 24;
            p[1] = 0x20000000u | MAP_EXIT;
            p[2] = xy(MX(exits[e].x[0]), MY(exits[e].z[0]));
            p[3] = xy(MX(exits[e].x[k]), MY(exits[e].z[k]));
            p[4] = xy(MX(exits[e].x[k + 1]), MY(exits[e].z[k + 1]));
            addPrim(ot - 1, p);
            p += 5;
        }
    }
    /* the player: an arrow from where he stands, the way he faces (rotY: 0 toward the camera, 1024 a quarter turn) */
    if (pl && (u8 *)(p + 5) <= pkt_end) {
        int px = MX(pl->pos.vx), py = MY(pl->pos.vz);
        int a = pl->pos.rotY & 0xFFF;
        int dx = -math_sin(a), dy = math_cos(a); /* (4096 = 1) */
        int tip_x = px + dx * 7 / 4096, tip_y = py + dy * 7 / 4096;
        int lx = px - dx * 4 / 4096 - dy * 4 / 4096, ly = py - dy * 4 / 4096 + dx * 4 / 4096;
        int rx = px - dx * 4 / 4096 + dy * 4 / 4096, ry = py - dy * 4 / 4096 - dx * 4 / 4096;
        p[0] = 4u << 24;
        p[1] = 0x20000000u | MAP_PLAYER;
        p[2] = xy(tip_x, tip_y);
        p[3] = xy(lx, ly);
        p[4] = xy(rx, ry);
        addPrim(ot - 1, p); /* (in front of the map) */
        p += 5;
    }
#undef MX
#undef MY
    return (u8 *)p;
}
