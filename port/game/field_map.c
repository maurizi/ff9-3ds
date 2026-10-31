/* The field's area map on the bottom screen (port/game/field_hud.c draws it in its box): the walkmesh seen from above
 * (each walkable triangle filled, its outer edges outlined), and the player as an arrow pointing where he faces. The
 * field's x runs right and its z away from the camera, so the map's up is the camera's forward: the map looks the
 * way the room does. Floors that move (lifts, bridges) move on the map too: it is rebuilt each frame. A walkmesh may
 * hold areas the camera doesn't show (Dali's field: the yard and, apart, the path beyond it): the area connected to
 * the player's triangle is drawn bright, the rest dimmed. */
#include "psyq_port.h"
#include "ovl_0a.h"

#define MAP_FILL 0x5888A8   /* walkable floor (BGR as the GPU takes it): sand */
#define MAP_FILL_AWAY 0x3A4A58 /* walkable floor not connected to where the player stands (another screen's) */
#define MAP_EDGE 0xC8E0F0   /* its outline */
#define MAP_PLAYER 0x30E8F8 /* the player */

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
            if (nb < 0 || nb >= n || near_player[nb] || !bound_tri(&g_fieldBgi.triangles[nb])) continue;
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

static inline u32 xy(int x, int y) { return (u32)(x & 0xFFFF) | (u32)y << 16; }

/* The map in the box (bx, by, bw, bh): packets from pkt linked into ot[0] (drawn above the box), returns the next
 * free byte; pkt_end: where the buffer ends. Nothing without a walkmesh. */
u8 *port_field_map_draw(u32 *ot, u8 *pkt, u8 *pkt_end, int bx, int by, int bw, int bh) {
    if (!g_fieldBgi.loaded || !g_fieldBgi.header || !g_fieldBgi.triangles) return pkt;
    const int n = g_fieldBgi.header->triangleCount;
    int x0 = 0x7FFFFFFF, z0 = 0x7FFFFFFF, x1 = -0x7FFFFFFF, z1 = -0x7FFFFFFF;
    for (int i = 0; i < n; i++) {
        const BgiTriangle *t = &g_fieldBgi.triangles[i];
        if (!bound_tri(t)) continue;
        for (int k = 0; k < 3; k++) {
            int x, z;
            world_xz(t, k, &x, &z);
            if (x < x0) x0 = x;
            if (x > x1) x1 = x;
            if (z < z0) z0 = z;
            if (z > z1) z1 = z;
        }
    }
    if (x1 <= x0 || z1 <= z0 || n > MAX_TRIS) return pkt;
    GameData *g = g_gameState->gameData;
    EvtObject *pl = g->ctx.field ? (EvtObject *)g->npcModels[g->ctx.field->playerObj].actor : NULL;
    mark_connected(n, player_triangle(pl));
    /* scale (16.16) to fit with a margin, centred */
    const int m = 8;
    int sx = ((bw - 2 * m) << 16) / (x1 - x0), sz = ((bh - 2 * m) << 16) / (z1 - z0), s = sx < sz ? sx : sz;
    const int ox = bx + bw / 2, oy = by + bh / 2, cx = (x0 + x1) / 2, cz = (z0 + z1) / 2;
#define MX(wx) (ox + (int)(((long long)((wx) - cx) * s) >> 16))
#define MY(wz) (oy - (int)(((long long)((wz) - cz) * s) >> 16))
    u32 *p = (u32 *)pkt;
    for (int i = 0; i < n; i++) {
        const BgiTriangle *t = &g_fieldBgi.triangles[i];
        if (!bound_tri(t)) continue;
        if ((u8 *)(p + 5 + 3 * 4) > pkt_end) break;
        int x[3], y[3];
        for (int k = 0; k < 3; k++) {
            int wx, wz;
            world_xz(t, k, &wx, &wz);
            x[k] = MX(wx);
            y[k] = MY(wz);
        }
        /* the outer edges (no walkable neighbour) */
        for (int k = 0; k < 3; k++) {
            int nb = t->neighbors[k];
            if (nb >= 0 && nb < n && bound_tri(&g_fieldBgi.triangles[nb])) continue;
            int a = k, b = (k + 1) % 3;
            p[0] = 3u << 24;
            p[1] = 0x40000000u | MAP_EDGE;
            p[2] = xy(x[a], y[a]);
            p[3] = xy(x[b], y[b]);
            addPrim(ot, p);
            p += 4;
        }
        p[0] = 4u << 24;
        p[1] = 0x20000000u | (near_player[i] ? MAP_FILL : MAP_FILL_AWAY);
        p[2] = xy(x[0], y[0]);
        p[3] = xy(x[1], y[1]);
        p[4] = xy(x[2], y[2]);
        addPrim(ot, p); /* (after its edges in the list: added later, drawn first) */
        p += 5;
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
