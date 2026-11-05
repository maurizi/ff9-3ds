/* The field's area map on the bottom screen (port/game/field_hud.c draws it in its box): the walkmesh seen from above
 * (each walkable triangle filled, its outer edges outlined), and the player as an arrow pointing where he faces. The
 * field's x runs right and its z away from the camera, so the map's up is the camera's forward: the map looks the
 * way the room does. The map is built once per field (bake, a quarter second after the field has loaded: its scripts
 * have made the exit regions by then) and kept: only the player and the markers move on it (a map rebuilt each frame
 * from live state jumped about as the player's triangle or the active triangles changed). A walkmesh may hold areas
 * the camera doesn't show (Dali's field: the yard and, apart, the path beyond it): the walkmesh is split into areas,
 * each one connected up to the exits (past an exit is reached only by leaving), and the map is the player's area
 * (the last one known while the player's triangle isn't), scaled to fill the box. The exits are marked: region objects (kind 3) whose tag-2
 * function (run while the player is inside) holds GOTO_FIELD (2B) or GOTO_WORLD (B6) with a literal destination, their
 * polygon (op 29: +0x14 count, +0x18 points) drawn in green (decomp-requests R30). Shops and save moogles are marked
 * where they stand: the field's characters whose script opens a shop (MENU 75 with type 2) or the save screen (type 4). */
#include <math.h>
#include "psyq_port.h"
#include "ovl_0a.h"

#define MAP_FILL 0x5888A8   /* walkable floor (BGR as the GPU takes it): sand */
#define MAP_EDGE 0xC8E0F0   /* its outline */
#define MAP_PLAYER 0x30E8F8 /* the player */
#define MAP_EXIT 0x48D860   /* an exit (a region whose "player inside" function changes field or goes to the world) */
#define MAP_SCALE 4588     /* the map's scale, 16.16 pixels per field unit (0.07) */
#define MAP_SHOP 0x20C0F8   /* a shopkeeper (amber) */
#define MAP_SAVE 0xE070F0   /* a save moogle (pink) */

/* whether p points into PS1 RAM (where all the game's data is): the bottom screen reads game state the PS1 never
 * showed this early, and a field that is still starting up can hold stale or garbage pointers (a real 3DS doesn't zero
 * its memory as an emulator does) */
extern unsigned char psx_ram[];
static int ram_ok(const void *p, unsigned size) {
    const unsigned char *b = (const unsigned char *)p;
    return b >= psx_ram + 0x10000 && b + size <= psx_ram + 0x200000;
}

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
#define MAX_AREAS 32
static u8 near_player[MAX_TRIS]; /* (mark_connected's: connected to its start: 1, or 2 in an exit) */

/* the baked map */
static struct {
    unsigned serial;            /* the field load it was baked for (port_field_serial) */
    u32 sig;                    /* what it was baked from (signature: the walkable triangles, the exits) */
    int n, areas, cur;          /* triangles, areas, the area shown */
    s16 x[MAX_TRIS][3], z[MAX_TRIS][3];
    u32 in[MAX_TRIS];           /* per triangle: the areas it shows in (bit a) */
    u8 exit[MAX_TRIS];          /* shaded as an exit */
    int x0[MAX_AREAS], z0[MAX_AREAS], x1[MAX_AREAS], z1[MAX_AREAS];
    int from_file;              /* read from maps.bin (tools/bake_maps.py): kept as it is, its markers below */
    int nmarks;
    s16 mx[16], mz[16];
    u8 mk[16];                  /* 2 shop, 4 save */
} bk;

/* ---- the maps baked from the disc at build time (tools/bake_maps.py: build/maps.bin, beside the disc pack) ----
 * A field's record has its walkmesh triangles (by index, the walkmesh's own order) with the areas each shows in and
 * whether it is an exit's, the areas' bounds, and the save moogles' and shops' places. With a record the map is the
 * field's data as baked; without one (no maps.bin, or a field it doesn't have) it is built here as before. */
void *port_asset_open(const char *name); /* (asset_file.c) */
int port_asset_read(void *f, void *buf, unsigned long n);
int port_asset_seek(void *f, unsigned long at);
void port_asset_close(void *f);
static void *maps_fp;
static int maps_state; /* 0 not opened, 1 open, -1 none found */
static u16 maps_field[1024];
static u32 maps_at[1024];
static int maps_count;

static void maps_open(void) {
    maps_state = -1;
    maps_fp = port_asset_open("maps.bin");
    if (!maps_fp) return;
    u8 h[16];
    if (!port_asset_read(maps_fp, h, 16) || __builtin_memcmp(h, "FF9M", 4) || h[4] != 1) {
        port_asset_close(maps_fp);
        maps_fp = NULL;
        return;
    }
    maps_count = h[8] | h[9] << 8;
    if (maps_count > 1024) maps_count = 1024;
    for (int i = 0; i < maps_count; i++) {
        u8 e[8];
        if (!port_asset_read(maps_fp, e, 8)) { maps_count = i; break; }
        maps_field[i] = (u16)(e[0] | e[1] << 8);
        maps_at[i] = (u32)(e[4] | e[5] << 8 | e[6] << 16 | (u32)e[7] << 24);
    }
    maps_state = 1;
}

/* field's baked map into bk if maps.bin has it, for a walkmesh of n triangles: 1 if read */
static int maps_load(int field, int n) {
    if (!maps_state) maps_open();
    if (maps_state < 0) return 0;
    int i = 0;
    while (i < maps_count && maps_field[i] != field) i++;
    if (i == maps_count || !port_asset_seek(maps_fp, maps_at[i])) return 0;
    u8 h[8];
    if (!port_asset_read(maps_fp, h, 8) || (h[0] | h[1] << 8) != n || n > MAX_TRIS || h[2] > MAX_AREAS) return 0;
    int na = h[2], nm = h[3] > 16 ? 16 : h[3];
    s16 a[4];
    for (int k = 0; k < na; k++) {
        if (!port_asset_read(maps_fp, a, 8)) return 0;
        bk.x0[k] = a[0], bk.z0[k] = a[1], bk.x1[k] = a[2], bk.z1[k] = a[3];
    }
    for (int t = 0; t < n; t++) {
        u8 r[20];
        if (!port_asset_read(maps_fp, r, 20)) return 0;
        for (int k = 0; k < 3; k++) {
            bk.x[t][k] = (s16)(r[k * 2] | r[k * 2 + 1] << 8);
            bk.z[t][k] = (s16)(r[6 + k * 2] | r[7 + k * 2] << 8);
        }
        bk.in[t] = (u32)(r[12] | r[13] << 8 | r[14] << 16 | (u32)r[15] << 24);
        bk.exit[t] = r[16];
    }
    bk.nmarks = 0;
    for (int k = 0; k < nm; k++) {
        u8 r[8];
        if (!port_asset_read(maps_fp, r, 8)) break;
        bk.mx[k] = (s16)(r[0] | r[1] << 8), bk.mz[k] = (s16)(r[2] | r[3] << 8), bk.mk[k] = r[4];
        bk.nmarks++;
    }
    bk.n = n;
    bk.areas = na;
    bk.cur = 0;
    for (int k = 1; k < na; k++) /* (until the player's triangle is known: the largest) */
        if ((bk.x1[k] - bk.x0[k]) * (bk.z1[k] - bk.z0[k]) > (bk.x1[bk.cur] - bk.x0[bk.cur]) * (bk.z1[bk.cur] - bk.z0[bk.cur]))
            bk.cur = k;
    bk.from_file = 1;
    return 1;
}

/* the field's exits (their polygons), collected before the map is built */
static struct { int n; s16 x[8], z[8]; } exits[16];
static int n_exits;
static int region_is_exit(int entry);

static void collect_exits(void) {
    n_exits = 0;
    if (!ram_ok(g_evtScripts, 8)) return;
    int guard = 0;
    for (EvtListNode *nd = evt_getObjectList(); ram_ok(nd, sizeof *nd) && n_exits < 16 && guard++ < 64; nd = nd->next) {
        EvtObject *o = nd->obj;
        if (!ram_ok(o, sizeof(EvtRegion)) || o->kind != 3) continue;
        EvtRegion *q = (EvtRegion *)o;
        if (q->count < 3 || q->count > 8 || !region_is_exit(o->scriptIdx)) continue;
        exits[n_exits].n = q->count;
        for (int k = 0; k < q->count; k++) exits[n_exits].x[k] = q->pts[k].x, exits[n_exits].z[k] = q->pts[k].z;
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

/* a triangle within EXIT_NEAR of an exit's polygon (some regions stop short of the walkmesh): shaded as the exit */
#define EXIT_NEAR 48
static int tri_near_exit(const BgiTriangle *t) {
    int cx = 0, cz = 0;
    for (int k = 0; k < 3; k++) {
        int x, z;
        world_xz(t, k, &x, &z);
        cx += x;
        cz += z;
        if (in_exit(x, z)) return 1;
    }
    cx /= 3;
    cz /= 3;
    for (int e = 0; e < n_exits; e++)
        for (int i = 0, n = exits[e].n; i < n; i++) {
            int j = (i + 1) % n;
            long ax = exits[e].x[i], az = exits[e].z[i], dx = exits[e].x[j] - ax, dz = exits[e].z[j] - az;
            long len2 = dx * dx + dz * dz, tq = len2 ? ((cx - ax) * dx + (cz - az) * dz) * 256 / len2 : 0;
            if (tq < 0) tq = 0;
            if (tq > 256) tq = 256;
            long px = ax + dx * tq / 256 - cx, pz = az + dz * tq / 256 - cz;
            if (px * px + pz * pz <= (long)EXIT_NEAR * EXIT_NEAR) return 1;
        }
    return 0;
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
            if (nb < 0 || nb >= n || near_player[nb] || !bound_tri(&g_fieldBgi.triangles[nb])) continue;
            if (tri_in_exit(&g_fieldBgi.triangles[nb])) { near_player[nb] = 2; continue; } /* (shown, not walked past) */
            near_player[nb] = 1;
            stack[sp++] = (s16)nb;
        }
    }
}

/* the player's walkmesh triangle, -1 if unknown */
static int player_triangle(EvtObject *pl) {
    if (!ram_ok(g_fieldBgi.actors, g_fieldBgi.actorCount * sizeof(BgiActor))) return -1;
    for (int i = 0; pl && i < g_fieldBgi.actorCount; i++)
        if ((EvtObject *)g_fieldBgi.actors[i].actor == pl) return g_fieldBgi.actors[i].triangle;
    return -1;
}

/* whether the script entry's tag-2 function goes to another field or the world map (a literal 2B / B6 in it) */
static int region_is_exit(int entry) {
    if (!ram_ok(g_evtScripts + entry * 8, 8)) return 0;
    u8 *h = g_evtScripts + *(u16 *)(g_evtScripts + entry * 8);
    if (!ram_ok(h, *(u16 *)(g_evtScripts + entry * 8 + 2))) return 0;
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

/* a character near a baked marker's place now (a moogle's model for a save marker: 220, 129; anyone for a shop),
 * within MARK_NEAR field units (they walk about a little, shopkeepers not at all) */
#define MARK_NEAR 640
static int mark_present(EvtObject *pl, int kind, int x, int z) {
    int guard = 0;
    for (EvtListNode *nd = evt_getObjectList(); ram_ok(nd, sizeof *nd) && guard++ < 64; nd = nd->next) {
        EvtObject *o = nd->obj;
        if (o == pl || !ram_ok(o, sizeof *o) || o->kind != 4) continue;
        if (kind == 4 && o->modelId != 220 && o->modelId != 129) continue;
        if (abs(o->pos.vx - x) <= MARK_NEAR && abs(o->pos.vz - z) <= MARK_NEAR) return 1;
    }
    return 0;
}

/* ---- who opens a shop or the save screen (decomp R32, ff9-decomp docs/r32/3ds-requests-3.md) ----
 * An object's trigger functions (an actor's tags 3 talk and 8, a region's 3, 8 and 2) are walked an instruction at a
 * time (the argument layouts: build/gen/evt_ops.h, from the decomp's op table); a literal MENU (75 00 type arg) of type
 * 2 (shop) or 4 (save) marks the object running it, and a call (10/12/14 flags level uid tag, literal) is followed into
 * the called object's function, a MENU found there marking the called object (a counter region calls its
 * shopkeeper). */
#include "../../build/gen/evt_ops.h" /* (tools/gen_evt_ops.py) */
static u8 menu_of[256]; /* per object uid: 2 shop, 4 save, 0 none */

/* past an expression (to its 0x7F) */
static int skip_expr(const u8 *c, int i, int end) {
    while (i < end) {
        int b = c[i++];
        if (b == 0x7F) return i;
        if (b == 0x7D || b == 0x78) i += 2;
        else if (b == 0x7E) i += 4;
        else if (b == 0x79 || b == 0x7A || b == 0x29 || b == 0x5F) i += 1;
        else if (b >= 0x80) i += b & 0x20 ? 2 : 1;
    }
    return -1;
}

/* the instruction after the one at i, -1 if it can't be decoded */
static int next_ins(const u8 *c, int i, int end) {
    int op = c[i++];
    if (op >= 1 && op <= 3) return i + 2;
    if (op == 4 || op == 0x30 || op == 0x31 || op == 0x6C || op == 0x6D) return i;
    if (op == 5) return skip_expr(c, i, end);
    if (op == 6) return i + 3 + 4 * c[i];
    if (op == 0x0B) return i + 5 + 2 * c[i];
    if (op == 0x0D) return i + 6 + 2 * (c[i] | c[i + 1] << 8);
    if (op >= 7 && op <= 9) return i + 2;
    if (op == 0x4E) { /* (a debug string, then a variable reference and a byte) */
        while (i < end && c[i]) i++;
        i++;
        if (i >= end) return -1;
        return i + 1 + (c[i] & 0x20 ? 2 : 1) + 1;
    }
    const char *spec;
    if (op == 0xFF) {
        if (i >= end) return -1;
        spec = evt_op2_args[c[i++]];
    } else spec = evt_op_args[op];
    if (!spec) return -1;
    if (!*spec) return i;
    int f = c[i++];
    for (; *spec && i >= 0 && i < end; spec++) {
        int w = *spec == 'b' ? 1 : *spec == 'w' ? 2 : *spec == 't' ? 3 : 0;
        if (*spec == 'r') { i++; continue; }
        if (*spec == 'N') { /* a count, then that many pairs of s16 */
            int n = f & 1 ? -1 : c[i];
            if (f & 1) i = skip_expr(c, i, end); else i++;
            f >>= 1;
            for (int k = 0; k < n && i >= 0 && i < end; k++)
                for (int h = 0; h < 2; h++, f >>= 1) i = f & 1 ? skip_expr(c, i, end) : i + 2;
            continue;
        }
        i = f & 1 ? skip_expr(c, i, end) : i + w;
        f >>= 1;
    }
    return i;
}

/* the code of entry's function tag: [*start, *end) from g_evtScripts; 0 if none */
static int function_of(int entry, int tag, const u8 **code, int *start, int *end) {
    if (entry < 0 || entry > 0xFF || !ram_ok(g_evtScripts + entry * 8, 8)) return 0;
    u8 *h = g_evtScripts + *(u16 *)(g_evtScripts + entry * 8);
    int size = *(u16 *)(g_evtScripts + entry * 8 + 2), n = *(u16 *)h >> 8;
    if (!ram_ok(h, size) || 2 + n * 4 > size) return 0;
    u16 *p = (u16 *)(h + 2);
    for (int i = 0; i < n; i++) {
        if (p[i * 2] != tag) continue;
        int s = p[i * 2 + 1] + 2, e = size;
        for (int j = 0; j < n; j++)
            if (p[j * 2 + 1] + 2 > s && p[j * 2 + 1] + 2 < e) e = p[j * 2 + 1] + 2;
        *code = h;
        *start = s;
        *end = e;
        return 1;
    }
    return 0;
}

static int entry_of_uid(int uid) {
    int guard = 0;
    for (EvtListNode *nd = evt_getObjectList(); ram_ok(nd, sizeof *nd) && guard++ < 64; nd = nd->next)
        if (ram_ok(nd->obj, sizeof(EvtObject)) && nd->obj->uid == uid) return nd->obj->scriptIdx;
    return -1;
}

static void scan_function(int entry, int tag, int uid, int depth) {
    const u8 *c;
    int i, end;
    if (!function_of(entry, tag, &c, &i, &end)) return;
    for (int guard = 0; i >= 0 && i < end && guard < 4096; guard++) {
        int op = c[i];
        if (op == 0x75 && i + 3 < end && c[i + 1] == 0 && (c[i + 2] == 2 || c[i + 2] == 4)) menu_of[uid] = c[i + 2];
        if ((op == 0x10 || op == 0x12 || op == 0x14) && i + 4 < end && c[i + 1] == 0 && depth < 2) {
            int to = c[i + 3] == 0xFF ? uid : c[i + 3];
            if (to < 0xFA || to == uid) scan_function(entry_of_uid(to), c[i + 4], to, depth + 1);
        }
        i = next_ins(c, i, end);
    }
}

static void collect_menus(void) {
    static int field = -1, age;
    int now = g_gameState->gameData->fieldId;
    if (now == field && ++age < 30) return;
    field = now;
    age = 0;
    __builtin_memset(menu_of, 0, sizeof menu_of);
    int guard = 0;
    for (EvtListNode *nd = evt_getObjectList(); ram_ok(nd, sizeof *nd) && guard++ < 64; nd = nd->next) {
        EvtObject *o = nd->obj;
        if (!ram_ok(o, sizeof *o) || (o->kind != 3 && o->kind != 4)) continue;
        scan_function(o->scriptIdx, 3, o->uid, 0);
        scan_function(o->scriptIdx, 8, o->uid, 0);
        if (o->kind == 3) scan_function(o->scriptIdx, 2, o->uid, 0);
    }
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

/* the map of the field as loaded: each walkable triangle's position, its areas (flood fills from each triangle not
 * yet in one, stopping at the exits), whether it is shaded as an exit, each area's bounds */
/* what the map is made of, in a word: the walkable triangles and the exits (the field's scripts make regions and
 * switch triangles on after it loads: a cutscene's end opens the way out) */
static u32 signature(int n) {
    u32 h = 2166136261u;
    for (int i = 0; i < n; i++) h = (h ^ (u32)bound_tri(&g_fieldBgi.triangles[i])) * 16777619u;
    h = (h ^ (u32)n_exits) * 16777619u;
    for (int e = 0; e < n_exits; e++) h = (h ^ (u32)(u16)exits[e].x[0] ^ (u32)(u16)exits[e].z[0] << 16) * 16777619u;
    return h;
}

static void bake(int n) {
    collect_exits();
    bk.sig = signature(n);
    bk.n = n;
    bk.areas = bk.cur = 0;
    __builtin_memset(bk.in, 0, sizeof bk.in);
    for (int i = 0; i < n; i++) {
        const BgiTriangle *t = &g_fieldBgi.triangles[i];
        int x, z;
        for (int k = 0; k < 3; k++) world_xz(t, k, &x, &z), bk.x[i][k] = (s16)x, bk.z[i][k] = (s16)z;
        bk.exit[i] = bound_tri(t) && tri_near_exit(t);
    }
    int best = 0, best_n = 0;
    for (int i = 0; i < n && bk.areas < MAX_AREAS; i++) {
        const BgiTriangle *t = &g_fieldBgi.triangles[i];
        if (bk.in[i] || !bound_tri(t) || tri_in_exit(t)) continue;
        mark_connected(n, i);
        int a = bk.areas++, count = 0;
        bk.x0[a] = bk.z0[a] = 0x7FFF;
        bk.x1[a] = bk.z1[a] = -0x7FFF;
        for (int j = 0; j < n; j++) {
            if (!near_player[j]) continue;
            bk.in[j] |= 1u << a;
            count++;
            for (int k = 0; k < 3; k++) {
                if (bk.x[j][k] < bk.x0[a]) bk.x0[a] = bk.x[j][k];
                if (bk.x[j][k] > bk.x1[a]) bk.x1[a] = bk.x[j][k];
                if (bk.z[j][k] < bk.z0[a]) bk.z0[a] = bk.z[j][k];
                if (bk.z[j][k] > bk.z1[a]) bk.z1[a] = bk.z[j][k];
            }
        }
        if (bk.x1[a] <= bk.x0[a] || bk.z1[a] <= bk.z0[a]) { /* (degenerate: not an area) */
            for (int j = 0; j < n; j++) bk.in[j] &= ~(1u << a);
            bk.areas--;
            continue;
        }
        if (count > best_n) best_n = count, best = a;
    }
    bk.cur = best; /* (until the player's triangle is known: the largest) */
}

/* The map in the box (bx, by, bw, bh): packets from pkt linked into ot[0] (drawn above the box), returns the next
 * free byte; pkt_end: where the buffer ends. Nothing without a walkmesh. */
u8 *port_field_map_draw(u32 *ot, u8 *pkt, u8 *pkt_end, int bx, int by, int bw, int bh) {
    if (!g_fieldBgi.loaded || !ram_ok(g_fieldBgi.header, sizeof(BgiHeader))) return pkt;
    const int n = g_fieldBgi.header->triangleCount;
    if (n <= 0 || n > MAX_TRIS || !ram_ok(g_fieldBgi.triangles, n * sizeof(BgiTriangle)) ||
        !ram_ok(g_fieldBgi.vertices, g_fieldBgi.header->vertexCount * sizeof(BgiVertex)) ||
        !ram_ok(g_fieldBgi.floors, g_fieldBgi.header->floorCount * sizeof(BgiFloor)))
        return pkt;
    for (int i = 0; i < n; i++) { /* (indices in range: else a field still loading) */
        const BgiTriangle *t = &g_fieldBgi.triangles[i];
        if ((u16)t->floor >= g_fieldBgi.header->floorCount) return pkt;
        for (int k = 0; k < 3; k++)
            if ((u16)t->vertices[k] >= g_fieldBgi.header->vertexCount) return pkt;
    }
    /* the player: the object the pad moves (the event system's playerUid; the field's playerObj model slot can be
     * another character's, as in the opening, where it was the Masked Baku's) */
    EvtObject *pl = NULL;
    if (ram_ok(g_evtState, sizeof *g_evtState)) {
        int guard = 0;
        for (EvtListNode *nd = evt_getObjectList(); ram_ok(nd, sizeof *nd) && guard++ < 64; nd = nd->next)
            if (ram_ok(nd->obj, sizeof(EvtObject)) && nd->obj->kind == 4 && nd->obj->uid == g_evtState->playerUid) {
                pl = nd->obj;
                break;
            }
    }
    unsigned port_field_serial(int *frames);
    int since;
    unsigned serial = port_field_serial(&since);
    if (bk.serial != serial) {
        bk.from_file = 0;
        if (maps_load(g_gameState->gameData->fieldId, n)) bk.serial = serial; /* (baked at build time) */
        else {
            if (since < 15) return pkt; /* (the field's scripts still making its regions) */
            bake(n);
            bk.serial = serial;
        }
    } else if (!bk.from_file && since % 30 == 0) { /* (a region made or a triangle switched since: baked again, still the same map) */
        collect_exits();
        if (signature(n) != bk.sig) {
            int cur = bk.cur;
            bake(n);
            if (cur < bk.areas) bk.cur = cur;
        }
    }
    if (!bk.areas) return pkt;
    int pt = player_triangle(pl);
    if (pt >= 0 && pt < bk.n && bk.in[pt] && !(bk.in[pt] >> bk.cur & 1)) bk.cur = __builtin_ctz(bk.in[pt]);
    const int a = bk.cur;
    const u32 bit = 1u << a;
    int x0 = bk.x0[a], z0 = bk.z0[a], x1 = bk.x1[a], z1 = bk.z1[a];
    /* scale (16.16): one for all areas (0.07 px per field unit: a room as large as Dali's weapon shop fills the box),
     * less to fit a larger one, centred */
    /* turned as the camera looks: the map's right is the camera's right on the ground, its up the camera's forward
     * (the field's own x and z aren't the camera's in every field: the Ice Cavern looks along -z, its map was upside
     * down); bounds and centre in those turned coordinates */
    int rc = 4096, rs = 0;
    if (ram_ok(g_fieldBgData, sizeof *g_fieldBgData)) {
        BgCamera *cam = fldbg_cameraPtr(g_fieldBgState.cameraId);
        if (ram_ok(cam, sizeof *cam)) {
            int ax = cam->rot[0][0], az = cam->rot[0][2], len = (int)sqrtf((float)(ax * ax + az * az));
            if (len > 256) rc = ax * 4096 / len, rs = az * 4096 / len;
        }
    }
#define RU(wx, wz) ((int)(((long long)rc * (wx) + (long long)rs * (wz)) >> 12))
#define RV(wx, wz) ((int)(((long long)rc * (wz) - (long long)rs * (wx)) >> 12))
    int u0 = 1 << 30, u1 = -(1 << 30), v0 = 1 << 30, v1 = -(1 << 30);
    for (int i = 0; i < bk.n; i++) {
        if (!(bk.in[i] & bit)) continue;
        for (int k = 0; k < 3; k++) {
            int u = RU(bk.x[i][k], bk.z[i][k]), v = RV(bk.x[i][k], bk.z[i][k]);
            if (u < u0) u0 = u;
            if (u > u1) u1 = u;
            if (v < v0) v0 = v;
            if (v > v1) v1 = v;
        }
    }
    if (u1 <= u0 || v1 <= v0) return pkt;
    const int m = 8;
    int sx = ((bw - 2 * m) << 16) / (u1 - u0), sz = ((bh - 2 * m) << 16) / (v1 - v0), s = sx < sz ? sx : sz;
    if (s > MAP_SCALE) s = MAP_SCALE; /* (the same scale everywhere: smaller only for an area too big for the box) */
    const int ox = bx + bw / 2, oy = by + bh / 2, cu = (u0 + u1) / 2, cv = (v0 + v1) / 2;
    __builtin_memset(edges, 0, sizeof edges);
    for (int i = 0; i < bk.n; i++) {
        if (!(bk.in[i] & bit)) continue;
        for (int k = 0; k < 3; k++) {
            unsigned e = edge_slot(bk.x[i][k], bk.z[i][k], bk.x[i][(k + 1) % 3], bk.z[i][(k + 1) % 3]);
            if (edges[e].count < 255) edges[e].count++;
        }
    }
#define MX(wx, wz) (ox + (int)(((long long)(RU(wx, wz) - cu) * s) >> 16))
#define MY(wx, wz) (oy - (int)(((long long)(RV(wx, wz) - cv) * s) >> 16))
    u32 *p = (u32 *)pkt;
    for (int i = 0; i < bk.n; i++) {
        if (!(bk.in[i] & bit)) continue; /* (only the player's area: the others are other screens) */
        if ((u8 *)(p + 5 + 3 * 4) > pkt_end) break;
        int x[3], y[3];
        for (int k = 0; k < 3; k++) x[k] = MX(bk.x[i][k], bk.z[i][k]), y[k] = MY(bk.x[i][k], bk.z[i][k]);
        /* the outline: edges no other drawn triangle shares */
        for (int k = 0; k < 3; k++) {
            int b = (k + 1) % 3;
            if (edges[edge_slot(bk.x[i][k], bk.z[i][k], bk.x[i][b], bk.z[i][b])].count > 1) continue;
            p[0] = 3u << 24;
            p[1] = 0x40000000u | MAP_EDGE;
            p[2] = xy(x[k], y[k]);
            p[3] = xy(x[b], y[b]);
            addPrim(ot, p);
            p += 4;
        }
        p[0] = 4u << 24;
        p[1] = 0x20000000u | (bk.exit[i] ? MAP_EXIT : MAP_FILL);
        p[2] = xy(x[0], y[0]);
        p[3] = xy(x[1], y[1]);
        p[4] = xy(x[2], y[2]);
        addPrim(ot, p); /* (after its edges in the list: added later, drawn first) */
        p += 5;
    }
    /* shopkeepers and save moogles: a diamond where each stands (baked: where the script first places them), while
     * one is there: the field's init doesn't create them in every story state (611 L. Castle/Guest Room with Zidane
     * in a soldier's armour: no moogle, the map still marked it) */
    for (int k = 0; bk.from_file && k < bk.nmarks && (u8 *)(p + 10) <= pkt_end; k++) {
        int mx = (x1 - x0) / 4, mz = (z1 - z0) / 4;
        if (mx < 512) mx = 512;
        if (mz < 512) mz = 512;
        if (bk.mx[k] < x0 - mx || bk.mx[k] > x1 + mx || bk.mz[k] < z0 - mz || bk.mz[k] > z1 + mz) continue;
        if (!mark_present(pl, bk.mk[k], bk.mx[k], bk.mz[k])) continue;
        int qx = MX(bk.mx[k], bk.mz[k]), qy = MY(bk.mx[k], bk.mz[k]);
        if (qx < bx + 5) qx = bx + 5;
        if (qx > bx + bw - 5) qx = bx + bw - 5;
        if (qy < by + 5) qy = by + 5;
        if (qy > by + bh - 5) qy = by + bh - 5;
        for (int half = 0; half < 2; half++) {
            p[0] = 4u << 24;
            p[1] = 0x20000000u | (bk.mk[k] == 2 ? MAP_SHOP : MAP_SAVE);
            p[2] = xy(qx - 4, qy);
            p[3] = xy(qx + 4, qy);
            p[4] = xy(qx, half ? qy + 4 : qy - 4);
            addPrim(ot - 1, p);
            p += 5;
        }
    }
    if (!bk.from_file) collect_menus();
    int guard = 0;
    for (EvtListNode *nd = evt_getObjectList(); !bk.from_file && ram_ok(nd, sizeof *nd) && guard++ < 64; nd = nd->next) {
        EvtObject *o = nd->obj;
        if (o == pl || !ram_ok(o, sizeof *o) || o->kind != 4 || (u8 *)(p + 10) > pkt_end) continue;
        /* (in this area: shopkeepers stand behind their counters, off the walkmesh, so with a margin) */
        int mx = (x1 - x0) / 4, mz = (z1 - z0) / 4;
        if (mx < 512) mx = 512;
        if (mz < 512) mz = 512;
        if (o->pos.vx < x0 - mx || o->pos.vx > x1 + mx || o->pos.vz < z0 - mz || o->pos.vz > z1 + mz) continue;
        int kind = menu_of[o->uid];
        if (!kind) continue;
        int qx = MX(o->pos.vx, o->pos.vz), qy = MY(o->pos.vx, o->pos.vz);
        if (qx < bx + 5) qx = bx + 5;
        if (qx > bx + bw - 5) qx = bx + bw - 5;
        if (qy < by + 5) qy = by + 5;
        if (qy > by + bh - 5) qy = by + bh - 5;
        for (int half = 0; half < 2; half++) {
            p[0] = 4u << 24;
            p[1] = 0x20000000u | (kind == 2 ? MAP_SHOP : MAP_SAVE);
            p[2] = xy(qx - 4, qy);
            p[3] = xy(qx + 4, qy);
            p[4] = xy(qx, half ? qy + 4 : qy - 4);
            addPrim(ot - 1, p);
            p += 5;
        }
    }
    /* the player: an arrow from where he stands, the way he faces (rotY: 0 toward the camera, 1024 a quarter turn) */
    if (pl && (u8 *)(p + 5) <= pkt_end) {
        int px = MX(pl->pos.vx, pl->pos.vz), py = MY(pl->pos.vx, pl->pos.vz);
        int a = pl->pos.rotY & 0xFFF;
        int fx = -math_sin(a), fz = -math_cos(a); /* (the way he faces in the field, 4096 = 1; turned as the map) */
        int dx = RU(fx, fz), dy = -RV(fx, fz);
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
#undef RU
#undef RV
    return (u8 *)p;
}
