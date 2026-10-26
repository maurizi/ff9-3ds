/* Hybrid renderer (docs/design.md §5.0): polygons the game draws into a back buffer go to the DS 3D engine; every
 * other primitive, and anything drawn into the displayed buffer, stays on the exact software rasteriser.
 *
 * - Buffers: the display areas the game shows (two for double buffering). A polygon drawn into a buffer other than
 *   the displayed one belongs to that buffer's scene. A buffer's first draw after it was shown starts a new scene.
 * - A scene is recorded as primitives (clipped to the drawing area, depth = submission order). When the game shows
 *   the buffer, its textures are planned for the whole scene, copied to texture VRAM, and its GX commands go to
 *   the geometry FIFO; the 3D engine keeps rendering them until the next swap.
 * - Textures are copies of PS1 VRAM in its own byte layout: a page's 4-, 8- or 15-bit texels are a DS 16-colour,
 *   256-colour or direct texture as they are (15-bit gets the DS alpha bit). One region per page and depth (its
 *   texels' bounding box) serves every CLUT; CLUTs are copied to palette VRAM. A region that keeps its rectangle
 *   keeps its place, so a steady scene copies nothing; regions under changed PS1 VRAM are re-copied
 *   (psx_vram_touch).
 * - 2D layer: the displayed buffer's pixels other than its scene's clear value (polygons never reach PS1 VRAM),
 *   scaled like the software display and shown as bitmap OBJ blocks above the 3D layer.
 *
 * Layout while active: VRAM A-D textures (512 KB), F-G texture palettes (32 KB), E main OBJ (the 2D layer).
 */
#if defined(PORT_GX_HYBRID)
#include "blackbox.h"
#include <nds.h>
#include <string.h>
#include <stdlib.h>

#include "gp0.h"
#include "psx_raster.h"
#include "psx_display.h"
#include "profile.h"
#include "gx_hybrid.h"
#include "psx_runtime.h"
#include "psx_mem.h"
#include "ovl0b_layout.h"
#include "ovl0a_layout.h"
#include "ovl0c_layout.h"
#include "native_mesh.h"
#include "gte.h"

extern unsigned short *psx_vram;

#define MAX_PRIMS 2500   /* recorded; the hardware takes 2048 polygons and 6144 vertices a frame */
#define BATTLE_PRIMS 1200 /* battle: the rest of each buffer's range holds its native meshes (port_native_mesh) */
#ifndef MAX_MESHES
#define MAX_MESHES 160
#endif
#ifndef NATIVE_BG_OFF
#define NATIVE_BG_OFF 0
#endif
#define PRIM_TABLES 4     /* bytes per recorded primitive: prim_group, prim_palette, prim_drop, prim_layer */
#define RANK_SHIFT 0       /* log2 main-OT entries per rank bucket (note_walk) */
#define Z_STEP 3           /* battle: the depth step between primitives in drawing order (emit_scene) */
/* plan_drops drops the smallest first, by twice the area: below 16, then 64, ... */
static const int drop_bounds[6] = {16, 64, 256, 1024, 4096, 1 << 30};
#define DROP_TIERS 6

#define RANK_BUCKETS ((0x1010 >> RANK_SHIFT) + 1)
#define MAX_POLYS 2044
#define MAX_VERTICES 6140

int gxh_stats_polys, gxh_stats_dropped, gxh_stats_texture_fail, gxh_stats_palette_fail, gxh_stats_blocks;
int gxh_stats_regions, gxh_stats_units, gxh_stats_unplaced, gxh_stats_units_peak;
int gxh_stats_soft; /* field: why scenes went software (bits, go_software's callers) */
unsigned int gxh_debug[4]; /* the last plan: regions and 2 KB units in use */
static int active;

/* ---- recorded primitives ----------------------------------------------------------------------------------- */

enum { P_TEXTURED = 1, P_RAW = 2, P_SEMI = 4, P_GOURAUD = 8,
       P_RECT = 16, /* a sprite: u, v are its last texels; the right and bottom vertices sample one further */
       P_UI = 32,   /* field: a UI page's sprite (text, window parts), drawn in software for the 2D layer */
       P_DITHER = 64, /* field: drawn with dithering on (E1 bit 9), for the software replays */
       P_TWIN = 128   /* field: a UI primitive drawn through a texture window (its buffer's twins[]) */ };

typedef struct {
    u8 n, flags, abr, depth;
    u16 tx, ty, clut_x, clut_y;
    s16 x[4], y[4];
    u8 r[4], g[4], b[4], u[4], v[4];
} Prim;
/* a field's background sprite: an opaque textured sprite, not a UI page's (the composite's, bgc_use) */
static inline int bg_prim(const Prim *p) { return (p->flags & (P_RECT | P_TEXTURED | P_SEMI | P_UI | P_TWIN)) == (P_RECT | P_TEXTURED); }

typedef struct {
    int x, y, w, h, valid;
    int shown;           /* displayed since its scene started: the next draw into it starts a new scene */
    Prim *prims;
    int count;
    unsigned int clear;  /* RGB15 of the scene's clearing fill */
    int submitted;       /* sent to the 3D engine since it last changed */
    int soft;            /* field: the scene went software (replayed into PS1 VRAM, then drawn there directly) */
    int polys;           /* field: polygons recorded (before the first, sprites are the software background layer) */
    struct { u16 k; u8 mask_x, mask_y, off_x, off_y; } twins[32]; /* field: the P_TWIN primitives' windows */
    int twin_count;      /* (the dialog windows' borders, 10 or so a scene) */
    struct { s16 x0, y0, x1, y1; int count; } over[4]; /* field: rectangles written into PS1 VRAM (LoadImage,
                          * MoveImage, fills) when the scene had count primitives: while it still has, a read inside
                          * one is of what was written, not of the scene (file_copyViaVram's scratch) */
    int over_count;
    /* world map: each recorded primitive's depth (its OT entry, world_depth), and the native terrain */
    u16 *slots;
    u64 touched[64];     /* field: the 8x8 cells (from the buffer's corner) that may differ from the clear value:
                          * the 2D layer skips the others (gxh_present) */
    /* battle: native meshes (port_native_mesh) */
    u16 *ranks;          /* per 4 entries of the main OT: primitives recorded when the walk reached them */
    int last_bucket;     /* the nearest bucket the walk has reached (RANK_BUCKETS: none) */
    unsigned scene_gen;  /* psx_ot_generation of the walk that drew the scene */
    int ofs_x, ofs_y;    /* the walk's drawing offset less the buffer's corner */
    u8 *pool;            /* the meshes' snapshots (NULL: none in this place) */
    unsigned pool_used, mesh_gen;
    int meshes;
    u16 mesh_at[MAX_MESHES]; /* their places in the pool, in drawing order */
    int mesh_drop;       /* plan_drops: the meshes' polygons below this (twice the area) are dropped, */
    int mesh_quota_bound, mesh_quota; /* and the first mesh_quota below mesh_quota_bound */
} Buffer;

static Buffer buffers[2];
static struct { int valid; u32 hash; unsigned tex; } bgc; /* a field's background composite in bank A (bgc_use) */

/* field models (port_native_field_mesh) */
#define FM_GROUPS 32       /* groups a scene */
#define FIELD_PRIMS_NATIVE 1500 /* a field buffer's primitives at most (use_place) */
#define FM_USES 32         /* texture pages and CLUTs a scene */
#define FM_POLYS 4096       /* polygons a scene (its culling bits) */
/* (src, polys and verts as taken: the game may have freed or reused the group by the time the scene is sent, two
 * frames later; fm_live checks) */
typedef struct { const u8 *group, *packets, *src; u16 vert0, poly0, polys, verts; s32 depth0, depth_far; } FGroup;
typedef struct { u16 tpage, clut; u8 u0, v0, u1, v1, group, palette; } FUse;
typedef struct {
    unsigned gen; int ngroups, nverts, nuses;
    int npolys, front_polys, front_verts; /* its polygons, those facing the camera and their vertices */
    FGroup groups[FM_GROUPS];
    u32 front[FM_POLYS / 32]; /* a bit a polygon: drawn (NCLIP > 0, or double-sided) */
    FUse uses[FM_USES];
    /* the groups' vertices (fm_cap): x, y (11 bits each: the GTE saturates them so) and 10 bits of depth, in OT
     * entries from the group's nearest (FGroup.depth0, on the OT entries' scale) */
    u32 *verts;
} FStore;
static FStore *fm_ring[3];         /* by generation (use_place: after the field's depths) */
static int fm_cap;                 /* vertices a scene */
/* packet sizes and xy offsets per source type (FT4, FT3, GT4, GT3, G4, G3, F4, F3: model_link.c's kinds) */
static const u8 fm_size[8] = {0x28, 0x20, 0x34, 0x28, 0x24, 0x1C, 0x18, 0x14};
static const u8 fm_xy_at[8][4] = {{0x08, 0x10, 0x18, 0x20}, {0x08, 0x10, 0x18}, {0x08, 0x14, 0x20, 0x2C},
                                  {0x08, 0x14, 0x20}, {0x08, 0x10, 0x18, 0x20}, {0x08, 0x10, 0x18},
                                  {0x08, 0x0C, 0x10, 0x14}, {0x08, 0x0C, 0x10}};

static FStore *scene_models(const Buffer *b);
static void fm_uses(FStore *st);
/* a scene gone software: its native models' polygons through the software renderer, each at its OT entry */
static int fm_soft_next;           /* the OT entries from here (nearer) aren't drawn yet */
static void fm_soft_draw(Buffer *b, int e);
/* (the models' code is ovl_0a's: a field place can outlast it by a frame while another slot-0 overlay loads) */
static inline int ovl0a_resident(void) { return psx_ovl_resident[0] == 0x0A; }
static inline void fm_soft_upto(Buffer *b, int e) { if (e < fm_soft_next && ovl0a_resident()) fm_soft_draw(b, e); }
#define FM_SOFT_ALL (-0x8000)

/* A primitive copied word by word (a struct assignment is a memcpy call) */
typedef union { Prim p; u32 w[12]; } PrimCopy;
_Static_assert(sizeof(Prim) == 48, "PrimCopy");
/* A primitive stored in 4-word bursts: the ARM9's data cache allocates on reads only, so the buffers' lines aren't
 * cached and single stores would go out one by one */
static inline void store_prim(Prim *d, const PrimCopy *s) {
    u32 *dw = (u32 *)d;
    const u32 *sw = s->w;
    __asm__ volatile("ldmia %1!, {r4, r5, r6, r7}\n\tstmia %0!, {r4, r5, r6, r7}\n\t"
                     "ldmia %1!, {r4, r5, r6, r7}\n\tstmia %0!, {r4, r5, r6, r7}\n\t"
                     "ldmia %1!, {r4, r5, r6, r7}\n\tstmia %0!, {r4, r5, r6, r7}"
                     : "+l"(dw), "+l"(sw) : : "r4", "r5", "r6", "r7", "memory");
}
static inline void copy_prim(PrimCopy *d, const Prim *s) {
    const u32 *w = (const u32 *)s; /* buffers and Prim are 4-byte aligned */
    d->w[0] = w[0]; d->w[1] = w[1]; d->w[2] = w[2]; d->w[3] = w[3]; d->w[4] = w[4]; d->w[5] = w[5];
    d->w[6] = w[6]; d->w[7] = w[7]; d->w[8] = w[8]; d->w[9] = w[9]; d->w[10] = w[10]; d->w[11] = w[11];
}
static Buffer *clear_fill; /* the game's fill that's about to land covers this buffer (psx_vram_filled) */

/* Where the scene buffers and the GX chunks live: the resident mode overlay's MIPS code range, which the port never
 * runs. Battle: ovl_0b (ovl0b_layout.h). Field (FIELDGX=1): ovl_0a (ovl0a_layout.h), where the scene also holds the
 * background sprites, so they keep their depth order with the characters. */
typedef struct { int overlay, field; unsigned int at, prims_at[2], max_prims; int world; } Place;
static int prims_max;     /* the buffers' capacity (use_place) */
static const Place places[] = {
    {0x0B, 0, OVL0B_AT, {OVL0B_PRIMS_AT_0, OVL0B_PRIMS_AT_1}, BATTLE_PRIMS, 0},
#ifdef PORT_FIELD_GX
    {0x0A, 1, OVL0A_AT, {OVL0A_PRIMS_AT_0, OVL0A_PRIMS_AT_1}, OVL0A_FIELD_PRIMS, 0},
#endif
#ifdef PORT_WORLD_GX
    /* the world map: as a battle, with its terrain native (world_terrain) and depths by OT entry */
    {0x0C, 0, OVL0C_AT, {OVL0C_PRIMS_AT_0, OVL0C_PRIMS_AT_1}, OVL0C_PRIMS, 1},
#endif
};
static const Place *place = &places[0];

/* A depth on the world scene's scale: OT entry e (of the 0x1010 from the main OT's base) nearer for larger values */
static inline int world_entry_depth(int e) { return (0x1010 - e) * 4; }
/* a field scene starts from this in PS1 VRAM: only what software draws (text, UI) differs, for the 2D layer */
#define FIELD_KEY 0x7C1F
static void dirty_cells(int x0, int y0, int x1, int y1);
static int soft_hold;     /* field: scenes to draw in software before trying 3D again (field_frame) */
static int direct_soft;   /* field: the GPU goes straight to the software renderer (present_direct) */
/* 1024x512: MARK_ADD where a software-drawn additive glow wrote since its buffer's scene started */
static u8 *marks;
#define MARK_ADD 1

static Buffer *buffer_at(int x, int y) {
    for (int i = 0; i < 2; ++i) {
        Buffer *b = &buffers[i];
        if (b->valid && x >= b->x && y >= b->y && x < b->x + b->w && y < b->y + b->h) return b;
    }
    return NULL;
}

static Buffer *displayed(void) {
    PsxDisplayInfo d;
    psx_gpu_display_info(&d);
    return buffer_at(d.x, d.y);
}

static unsigned buffers_version; /* bumped when a buffer's place or size changes (target's cache) */

static void note_buffer(int x, int y, int w, int h) {
    if (w < 256 || h < 192) return;
    for (int i = 0; i < 2; ++i)
        if (buffers[i].valid && buffers[i].x == x && buffers[i].y == y) {
            if (buffers[i].w != w || buffers[i].h != h) ++buffers_version;
            buffers[i].w = w; buffers[i].h = h;
            return;
        }
    ++buffers_version;
    Buffer *b = !buffers[0].valid ? &buffers[0] : !buffers[1].valid ? &buffers[1] : NULL;
    if (!b) b = displayed() == &buffers[0] ? &buffers[1] : &buffers[0]; /* a third area replaces the hidden one */
    b->x = x; b->y = y; b->w = w; b->h = h; b->valid = 1; b->shown = 1; b->count = 0; b->submitted = 0;
}

/* Field: b's PS1 VRAM all the key colour */
static void fill_buffer(Buffer *b, u32 colour) {
    int w = b->x + b->w <= 1024 ? b->w : 1024 - b->x;
    for (int row = 0; row < b->h; ++row) {
        u32 *p = (u32 *)(psx_vram + ((b->y + row) & 511) * 1024 + b->x);
        for (int k = 0; k < w / 2; ++k) p[k] = colour | colour << 16;
    }
}
static void key_fill(Buffer *b) { fill_buffer(b, FIELD_KEY); }

static void start_scene(Buffer *b, int filled /* the game's fill is about to cover it */) {
    b->shown = 0;
    b->count = 0;
    b->last_bucket = RANK_BUCKETS;
    b->scene_gen = ~0u;
    b->submitted = 0;
    b->soft = 0;
    b->polys = 0;
    b->twin_count = 0;
    b->over_count = 0;
    if (place->field && soft_hold) {
        b->soft = 1; /* textures didn't fit lately: drawn straight in software, nothing recorded (field_frame) */
    } else if (place->field) {
        /* nothing clears a field's buffer (its background covers it): the scene's PS1 VRAM starts as the key */
        if (!filled) key_fill(b);
        b->clear = FIELD_KEY;
        dirty_cells(b->x, b->y, b->x + b->w - 1, b->y + b->h - 1);
        memset(b->touched, 0, sizeof b->touched); /* all the clear value */
    }
    for (int row = 0; row < b->h; ++row)
        memset(marks + (((b->y + row) & 511) * 1024 + b->x) / 8, 0,
               ((b->x + b->w <= 1024 ? b->w : 1024 - b->x) + 7) / 8); /* buffers start on 8-pixel columns */
}

/* The buffer the current drawing area belongs to (NULL: offscreen), starting its scene if it was shown; *shown is
 * the displayed buffer. */
static Buffer *target(Buffer **shown) {
    /* the lookups depend only on the drawing environment, the display position and the buffers' places */
    static struct { int x, y, w, h, ax, ay, dx, dy; unsigned version; Buffer *shown, *b; } last = {.w = -1};
    int x, y, w, h, dx, dy, dw, dh;
    psx_gpu_draw_area(&x, &y, &w, &h);
    psx_gpu_display_area(&dx, &dy, &dw, &dh);
    Buffer *b;
    if (x == last.x && y == last.y && w == last.w && h == last.h && gp0_state.area_x0 == last.ax &&
        gp0_state.area_y0 == last.ay && dx == last.dx && dy == last.dy && buffers_version == last.version) {
        *shown = last.shown;
        b = last.b;
    } else {
        note_buffer(x, y, w, h);
        *shown = displayed();
        b = buffer_at(gp0_state.area_x0, gp0_state.area_y0);
        last.x = x; last.y = y; last.w = w; last.h = h; last.ax = gp0_state.area_x0; last.ay = gp0_state.area_y0;
        last.dx = dx; last.dy = dy; last.version = buffers_version; last.shown = *shown; last.b = b;
    }
    if (b && b->shown && b != *shown) start_scene(b, 0);
    return b;
}

static int triangle_area(const Gp0Vtx *a, const Gp0Vtx *b, const Gp0Vtx *c) {
    return (b->x - a->x) * (c->y - a->y) - (b->y - a->y) * (c->x - a->x);
}

/* Native meshes are placed among the scene's primitives by their OT entry: as the walk reaches each bucket of
 * RANK_SHIFT entries of the main OT (far to near), the count of primitives recorded so far is its rank. */
static inline void note_walk(Buffer *b) {
    if (!b->ranks) return;
    int q = (int)(psx_ot_slot >> RANK_SHIFT);
    if (q < b->last_bucket) {
        for (int i = q; i < b->last_bucket; ++i) b->ranks[i] = (u16)b->count;
        b->last_bucket = q;
    }
    b->scene_gen = psx_ot_generation;
    b->ofs_x = gp0_state.ofs_x - b->x; b->ofs_y = gp0_state.ofs_y - b->y;
}

/* The world map and fields are depth tested: a primitive at the depth of the walk's OT entry (emit_scene orders
 * those of one entry), native polygons at their vertices' depths on the same scale */
static inline void note_depth(Buffer *b) {
    if (!b->slots) return;
    b->slots[b->count] = (u16)world_entry_depth((int)psx_ot_slot);
    b->scene_gen = psx_ot_generation;
    b->ofs_x = gp0_state.ofs_x - b->x; b->ofs_y = gp0_state.ofs_y - b->y;
}

static void record(Buffer *b, const Gp0Mode *m, const Gp0Vtx *v, int n) {
    if (b->count >= prims_max) { gxh_stats_dropped++; return; }
    note_walk(b);
    note_depth(b);
    PrimCopy t; /* built on the stack (DTCM), stored in bursts */
    Prim *p = &t.p; /* (a triangle's fourth vertex is left as it is: nothing reads it) */
    p->n = (u8)n;
    p->flags = (u8)((m->textured ? P_TEXTURED : 0) | (m->raw ? P_RAW : 0) | (m->semi ? P_SEMI : 0) |
                    (m->gouraud ? P_GOURAUD : 0) | (place->field && (gp0_state.texpage & 0x200) ? P_DITHER : 0));
    p->abr = (u8)m->abr; p->depth = (u8)(m->depth > 2 ? 2 : m->depth);
    p->tx = (u16)m->tx; p->ty = (u16)m->ty; p->clut_x = (u16)m->clut_x; p->clut_y = (u16)m->clut_y;
    for (int i = 0; i < n; ++i) {
        p->x[i] = (s16)(v[i].x - b->x); p->y[i] = (s16)(v[i].y - b->y);
        p->r[i] = (u8)(v[i].r < 0 ? 0 : v[i].r > 255 ? 255 : v[i].r);
        p->g[i] = (u8)(v[i].g < 0 ? 0 : v[i].g > 255 ? 255 : v[i].g);
        p->b[i] = (u8)(v[i].b < 0 ? 0 : v[i].b > 255 ? 255 : v[i].b);
        p->u[i] = (u8)v[i].u; p->v[i] = (u8)v[i].v;
    }
    store_prim(&b->prims[b->count++], &t);
}

/* ---- native battle meshes (port/game/mesh_native.c) ---------------------------------------------------------
 *
 * btl_drawMesh's models come here instead of as PS1 packets: the mesh's source lists, a copy of its projected
 * vertices and the texture words it uses, kept with the buffer the frame's OT is drawn into. The scene drawn by the
 * walk of the same frame (psx_ot_generation) emits them after its primitives, each vertex at the depth of the
 * primitives the walk had recorded when it reached the vertex's OT entry (emit_scene), so the 3D engine's depth
 * test sorts their pixels among the rest, where the PS1 sorted whole polygons into its OT. The PS1's culling is
 * kept (NCLIP <= 0, except double-sided). The packets' cost goes: writing them, walking and decoding them,
 * recording and planning each polygon. */
#define MESH_USES 16
#define POOL_BYTES ((MAX_PRIMS - BATTLE_PRIMS) * (int)sizeof(Prim))
/* a texture word and blend mode a mesh uses: its key (tex, mode), the tpage bits the game ors in, its texels' bounds,
 * and its plan */
typedef struct { u32 page; u8 tex, mode, orbits, semi, u0, v0, u1, v1, group, palette; } MeshUse;
typedef struct {
    const u8 *src;       /* a model mesh's source lists; a background record (BtlBgRec) */
    const u16 *uvs;
    const u32 *colors;
    const u32 *xy;
    const u16 *z;        /* a mesh's vertex depths; a record's OT entries per polygon (0xFFFF: not drawn) */
    u8 kind;             /* MESH_MODEL, MESH_BG */
    u16 bright;          /* a record's brightness (0x80: as is) */
    u16 count[8];        /* a record: [0] quads, [1] triangles */
    s16 bias;
    u8 nuses, pad;
    u16 polys, vertices; /* after the PS1's culling (plan_drops) */
    u8 untex_abr;        /* a model's untextured polygons' blend mode: the last textured one's (the GPU's mode) */
    const u8 *info;      /* per polygon: 0xFF not drawn, else translucent << 7 | its use << 3 (15: untextured) | size tier */
    u16 tier_polys[6], tier_vertices[6]; /* by size (drop_bounds) */
    MeshUse uses[MESH_USES];
} MeshInst;
enum { MESH_MODEL, MESH_BG };
static const u8 mesh_size[8] = {0x18, 0x14, 0x20, 0x18, 0x18, 0x14, 0x10, 0x0C};
static const u8 mesh_tex_at[4] = {0x13, 0x06, 0x1D, 0x16}, mesh_flags_at[8] = {0x15, 0x12, 0x1C, 0x0F, 0x14, 0x13, 0x0D, 0x0B};
#define MW(p, o) (*(const u32 *)((p) + (o)))
#define MH(p, o) (*(const u16 *)((p) + (o)))
#define MB(p, o) (*(const u8 *)((p) + (o)))
int gxh_stats_meshes, gxh_stats_mesh_fallback;

/* the uv words of a textured source polygon (type 0-3) into u[4]/v[4] */
static inline int mesh_uvs(const MeshInst *mi, int type, const u8 *s, u16 *uv) {
    switch (type) {
    case 0: uv[0] = mi->uvs[MW(s, 8) & 0xFFFF]; uv[1] = mi->uvs[MW(s, 8) >> 16];
            uv[2] = mi->uvs[MW(s, 0xC) & 0xFFFF]; uv[3] = mi->uvs[MW(s, 0xC) >> 16]; return 4;
    case 1: uv[0] = mi->uvs[MW(s, 0xC) & 0xFFFF]; uv[1] = mi->uvs[MW(s, 0xC) >> 16]; uv[2] = mi->uvs[MH(s, 0x10)]; return 3;
    case 2: uv[0] = mi->uvs[MW(s, 8) & 0xFFFF]; uv[1] = mi->uvs[MW(s, 8) >> 16];
            uv[2] = mi->uvs[MW(s, 0xC) & 0xFFFF]; uv[3] = mi->uvs[MW(s, 0xC) >> 16]; return 4;
    default: uv[0] = mi->uvs[MH(s, 6)]; uv[1] = mi->uvs[MW(s, 8) & 0xFFFF]; uv[2] = mi->uvs[MW(s, 8) >> 16]; return 3;
    }
}

/* Whether a polygon's screen vertices (GTE SXY words) all lie off one side of the buffer: hybrid_poly clips such a
 * packet to nothing. The drawing offset is the walk's (b->ofs_*: the previous frame's, the same in battle). */
static int offscreen(const Buffer *b, const u32 *xy, const u32 *v, int n) {
    int x0 = 1 << 30, y0 = 1 << 30, x1 = -(1 << 30), y1 = -(1 << 30);
    for (int i = 0; i < n; ++i) {
        int x = (s16)xy[v[i]], y = (s16)(xy[v[i]] >> 16);
        if (x < x0) x0 = x;
        if (x > x1) x1 = x;
        if (y < y0) y0 = y;
        if (y > y1) y1 = y;
    }
    return x1 + b->ofs_x < 0 || x0 + b->ofs_x >= b->w || y1 + b->ofs_y < 0 || y0 + b->ofs_y >= b->h;
}

/* twice a source polygon's screen area (as prim_area2), given NCLIP of its first three vertices */
static int mesh_area2(const u32 *xy, const u8 *s, int t, int nclip) {
    int a = nclip < 0 ? -nclip : nclip;
    if (!(t & 1)) {
        u32 c = xy[MW(s, 0) >> 16], d = xy[MW(s, 4) & 0xFFFF], e = xy[MW(s, 4) >> 16];
        int x1 = (s16)c, y1 = (s16)(c >> 16), x2 = (s16)d, y2 = (s16)(d >> 16), x3 = (s16)e, y3 = (s16)(e >> 16);
        int q = (x2 - x1) * (y3 - y1) - (y2 - y1) * (x3 - x1);
        a += q < 0 ? -q : q;
    }
    return a;
}

int port_native_mesh(const NativeMesh *nm) {
    if (!active || place->field) return 0;
    Buffer *b = buffer_at(nm->draw_x, nm->draw_y);
    if (!b || !b->pool) return 0;
    if (b->mesh_gen != psx_ot_generation) { b->mesh_gen = psx_ot_generation; b->pool_used = 0; b->meshes = 0; }
    unsigned npolys = 0;
    for (int t = 0; t < 8; ++t) npolys += nm->count[t];
    unsigned need = (sizeof(MeshInst) + (unsigned)nm->nverts * 6 + npolys + 3) & ~3u;
    if (b->meshes >= MAX_MESHES || b->pool_used + need > POOL_BYTES) { gxh_stats_mesh_fallback++; return 0; }
    MeshInst *mi = (MeshInst *)(b->pool + b->pool_used);
    u32 *xy = (u32 *)(mi + 1);
    u16 *z = (u16 *)(xy + nm->nverts);
    u8 *info = (u8 *)(z + nm->nverts);
    int bx0 = 1 << 30, by0 = 1 << 30, bx1 = -(1 << 30), by1 = -(1 << 30);
    for (int i = 0; i < nm->nverts; ++i) {
        u32 w = nm->sxy[i];
        xy[i] = w; z[i] = (u16)nm->sz[i];
        int x = (s16)w, y = (s16)(w >> 16);
        if (x < bx0) bx0 = x;
        if (x > bx1) bx1 = x;
        if (y < by0) by0 = y;
        if (y > by1) by1 = y;
    }
    /* (a model all inside the buffer needs no test per polygon) */
    int inside = bx0 + b->ofs_x >= 0 && bx1 + b->ofs_x < b->w && by0 + b->ofs_y >= 0 && by1 + b->ofs_y < b->h;
    mi->src = nm->src; mi->uvs = nm->uvs; mi->colors = (const u32 *)nm->colors; mi->bias = (s16)nm->ot_bias;
    mi->xy = xy; mi->z = z; mi->info = info; mi->nuses = 0; mi->kind = MESH_MODEL; mi->untex_abr = 0;
    for (int t = 0; t < 8; ++t) mi->count[t] = nm->count[t];
    memset(mi->tier_polys, 0, sizeof mi->tier_polys);
    memset(mi->tier_vertices, 0, sizeof mi->tier_vertices);
    /* each polygon once: the PS1's culling (NCLIP <= 0 unless double-sided), no area or off the buffer (as
     * hybrid_poly), its size for the budget, the texture word and blend mode it uses and their texels' bounds */
    unsigned polys = 0, vertices = 0;
    int last_use = 0;
    const u8 *s = nm->src;
    for (int t = 0, k = 0, pi = 0; t < 8; ++t)
        for (k = 0; k < nm->count[t]; ++k, s += mesh_size[t], ++pi) {
            int quad = !(t & 1), nv = quad ? 4 : 3;
            u32 vi[4] = {MW(s, 0) & 0xFFFF, MW(s, 0) >> 16, quad ? MW(s, 4) & 0xFFFF : MH(s, 4), quad ? MW(s, 4) >> 16 : 0};
            u8 f = MB(s, mesh_flags_at[t]);
            int x0 = (s16)xy[vi[0]], y0 = (s16)(xy[vi[0]] >> 16), x1 = (s16)xy[vi[1]], y1 = (s16)(xy[vi[1]] >> 16);
            int x2 = (s16)xy[vi[2]], y2 = (s16)(xy[vi[2]] >> 16);
            int nclip = x0 * y1 + x1 * y2 + x2 * y0 - x0 * y2 - x1 * y0 - x2 * y1;
            info[pi] = 0xFF;
            if (!(f & 2) && nclip <= 0) continue;
            int a2 = mesh_area2(xy, s, t, nclip);
            if (!a2 || (!inside && offscreen(b, xy, vi, nv))) continue;
            int tier = 0, use = 15, semi = 0;
            while (a2 >= drop_bounds[tier]) ++tier;
            if (t < 4) {
                u8 tex = MB(s, mesh_tex_at[t]), mode = f & 0xE1;
                u16 uv[4];
                mesh_uvs(mi, t, s, uv);
                use = last_use; /* (consecutive polygons nearly always share one) */
                if (use >= mi->nuses || mi->uses[use].tex != tex || mi->uses[use].mode != mode)
                    for (use = 0; use < mi->nuses; ++use)
                        if (mi->uses[use].tex == tex && mi->uses[use].mode == mode) break;
                last_use = use;
                if (use == mi->nuses) {
                    if (mi->nuses == MESH_USES) { gxh_stats_mesh_fallback++; return 0; }
                    mi->uses[mi->nuses++] = (MeshUse){nm->tex[tex], tex, mode, (u8)(mode & 0xE0), (u8)(mode & 1), 255, 255, 0,
                                                      0, 0xFF, 0xFF};
                }
                MeshUse *u = &mi->uses[use];
                for (int i = 0; i < nv; ++i) {
                    u8 cu = (u8)uv[i], cv = (u8)(uv[i] >> 8);
                    if (cu < u->u0) u->u0 = cu;
                    if (cu > u->u1) u->u1 = cu;
                    if (cv < u->v0) u->v0 = cv;
                    if (cv > u->v1) u->v1 = cv;
                }
                semi = f & 1;
            } else if (t >= 6) {
                semi = (MW(s, 8) & 0x02000000) != 0;
            }
            info[pi] = (u8)(semi << 7 | use << 3 | tier);
                    if (t < 4) mi->untex_abr = (u8)((((mi->uses[use].page & 0xFFFF) | mi->uses[use].orbits) >> 5) & 3);
            ++polys;
            vertices += (unsigned)nv;
            mi->tier_polys[tier]++;
            mi->tier_vertices[tier] += (u16)nv;
        }
    mi->polys = (u16)polys; mi->vertices = (u16)vertices;
    b->mesh_at[b->meshes] = (u16)b->pool_used;
    b->pool_used += need;
    b->meshes++;
    gxh_stats_meshes++;
    return 1;
}

/* A background record's polygons: the OT entry each would link into (btlbg_drawRecord's rules), 0xFFFF where it
 * culls (NCLIP < 0) or skips (depth 0) one. */
static int bg_entry(u32 flags, u32 shift, u32 z, int quad, int base) {
    if (flags & 8) return base + 0x3FC / 4;
    if (flags == 0) {
        if (quad) {
            z = (z << shift) & 0xFFFC;
            if (!z) return -1;
            if (z & 0xC000) z = 0x3FFC;
        } else {
            if (!z) return -1;
            z = (z << shift) << 2;
            if (z & 0xC000) z = 0x3FFC;
        }
    } else {
        z >>= quad ? 6 : 4;
        if (flags & 2) z = 0;
        else if (z & 0xFF00) z = 0x3FC;
        else { z <<= 2; if (!z) return -1; }
    }
    return base + (int)(z / 4);
}

int port_native_bg(const NativeBg *r) {
    if (NATIVE_BG_OFF || !active || place->field || !psx_main_ot) return 0;
    Buffer *b = buffer_at(r->draw_x, r->draw_y);
    if (!b || !b->pool) return 0;
    int base = (int)(r->ot - psx_main_ot);
    if (base < 0 || base >= (int)psx_main_ot_n) return 0; /* (another OT: keep its packets) */
    const u8 *rec = r->rec;
    u32 flags = MH(rec, 0), nv = MH(rec, 2), nquad = MH(rec, 0xC), ntri = MH(rec, 0xE);
    if (!nv || nv > 128) return 0;
    if (b->mesh_gen != psx_ot_generation) { b->mesh_gen = psx_ot_generation; b->pool_used = 0; b->meshes = 0; }
    /* its polygons index the scratchpad's projected vertices, which may reach past its own into an earlier
     * record's (btlbg_project leaves them there): the snapshot holds every slot up to the highest it names */
    const u8 *idx0 = rec + MH(rec, 8);
    u32 nsnap = nv;
    for (u32 k = 0; k < nquad * 4 + ntri * 3; ++k) {
        u32 v = MH(idx0, 2 * k) / 4;
        if (v >= nsnap) nsnap = v + 1;
    }
    if (nsnap > 128) return 0;
    unsigned need = (sizeof(MeshInst) + nsnap * 4 + (nquad + ntri) * 3 + 3) & ~3u;
    if (b->meshes >= MAX_MESHES || b->pool_used + need > POOL_BYTES) { gxh_stats_mesh_fallback++; return 0; }
    MeshInst *mi = (MeshInst *)(b->pool + b->pool_used);
    u32 *xy = (u32 *)(mi + 1);
    u16 *oti = (u16 *)(xy + nsnap);
    u8 *info = (u8 *)(oti + nquad + ntri);
    /* the vertices through RTPT's projection (btlbg_project: SZ >> 4 for the depths), the last three again
     * through the GTE so its registers end as the game's loop leaves them */
    u32 pair[2 * 6], *sp_xy = (u32 *)psx_scratchpad, *sp_z = (u32 *)(psx_scratchpad + 0x200);
    for (u32 i = 0; i < nv; i += 6) {
        for (int k = 0; k < 6; ++k) {
            const u8 *v = r->verts + (i + k) * 6;
            pair[2 * k] = (u32)(v[0] | v[1] << 8) | (u32)(v[2] | v[3] << 8) << 16;
            pair[2 * k + 1] = (u32)(v[4] | v[5] << 8);
        }
        u32 z6[6];
        gte_rtpt_batch((const gte_u32 *)pair, 6, (gte_u32 *)&sp_xy[i], (gte_u32 *)z6);
        for (int k = 0; k < 6; ++k) sp_z[i + k] = z6[k] >> 4; /* (as btlbg_project stores them) */
    }
    for (u32 i = 0; i < nsnap; ++i) xy[i] = sp_xy[i];
    for (int k = 0; k < 3; ++k) { psx_gte.d[2 * k] = pair[6 + 2 * k]; psx_gte.d[2 * k + 1] = pair[7 + 2 * k]; }
    gte_cmd(0x0280030);
    const u32 *col = (const u32 *)(rec + MH(rec, 6));
    const u8 *idx = rec + MH(rec, 8), *uv = rec + MH(rec, 0xA);
    mi->src = rec; mi->uvs = NULL; mi->colors = col; mi->xy = xy; mi->z = oti; mi->kind = MESH_BG;
    mi->bright = (u16)r->bright; mi->bias = 0; mi->nuses = 0; mi->info = info;
    memset(mi->count, 0, sizeof mi->count);
    mi->count[0] = (u16)nquad; mi->count[1] = (u16)ntri;
    memset(mi->tier_polys, 0, sizeof mi->tier_polys);
    memset(mi->tier_vertices, 0, sizeof mi->tier_vertices);
    unsigned polys = 0, vertices = 0;
    for (u32 k = 0; k < nquad + ntri; ++k) {
        int quad = k < nquad;
        const u8 *ix = quad ? idx + k * 8 : idx + nquad * 8 + (k - nquad) * 6;
        const u8 *uw = quad ? uv + k * 8 : uv + nquad * 8 + (k - nquad) * 6;
        u32 v0 = MH(ix, 0) / 4, v1 = MH(ix, 2) / 4, v2 = MH(ix, 4) / 4, v3 = quad ? MH(ix, 6) / 4 : 0;
        int x0 = (s16)xy[v0], y0 = (s16)(xy[v0] >> 16), x1 = (s16)xy[v1], y1 = (s16)(xy[v1] >> 16);
        int x2 = (s16)xy[v2], y2 = (s16)(xy[v2] >> 16);
        int nclip = x0 * y1 + x1 * y2 + x2 * y0 - x0 * y2 - x1 * y0 - x2 * y1;
        oti[k] = 0xFFFF;
        info[k] = 0xFF;
        if (nclip < 0) continue;
        u32 vv[4] = {v0, v1, v2, v3};
        if (offscreen(b, xy, vv, quad ? 4 : 3)) continue;
        /* (the quads read SZ halfwords, the triangles' third a whole word: background.c) */
        u32 z0 = (u16)sp_z[v0], z1 = (u16)sp_z[v1], z2 = (u16)sp_z[v2];
        u32 z = quad ? z0 + z1 + z2 + (flags & 8 ? 0 : (u16)sp_z[v3]) : (((z0 + z1) >> 1) + sp_z[v2]) >> 1;
        int e = bg_entry(flags, r->shift, z, quad, base);
        if (e < 0) continue;
        if (e >= (int)psx_main_ot_n) e = (int)psx_main_ot_n - 1;
        oti[k] = (u16)e;
        /* the texture word and blend mode it uses, and its texels' bounds */
        u32 c = col[k];
        u8 tex = (u8)((c >> 24) & 0x1F), mode = (u8)((c >> 24) & 0xE0);
        int use;
        for (use = 0; use < mi->nuses; ++use)
            if (mi->uses[use].tex == tex && mi->uses[use].mode == mode) break;
        MeshUse *u = &mi->uses[use];
        if (use == mi->nuses) {
            if (mi->nuses == MESH_USES) { gxh_stats_mesh_fallback++; return 0; }
            mi->nuses++;
            *u = (MeshUse){r->textab[tex], tex, mode, (u8)((c >> 25) & 0x60), (u8)((c >> 29) & 1), 255, 255, 0, 0,
                           0xFF, 0xFF};
        }
        for (int i = 0; i < (quad ? 4 : 3); ++i) {
            u16 w = MH(uw, 2 * i);
            u8 cu = (u8)w, cv = (u8)(w >> 8);
            if (cu < u->u0) u->u0 = cu;
            if (cu > u->u1) u->u1 = cu;
            if (cv < u->v0) u->v0 = cv;
            if (cv > u->v1) u->v1 = cv;
        }
        int a2 = nclip, tier = 0, nvx = quad ? 4 : 3;
        if (quad) {
            int x3 = (s16)xy[v3], y3 = (s16)(xy[v3] >> 16), q = (x2 - x1) * (y3 - y1) - (y2 - y1) * (x3 - x1);
            a2 += q < 0 ? -q : q;
        }
        while (a2 >= drop_bounds[tier]) ++tier;
        if (!a2) { oti[k] = 0xFFFF; continue; } /* (no area: hybrid_poly drops it) */
        info[k] = (u8)(((c >> 29) & 1) << 7 | use << 3 | tier);
        ++polys;
        vertices += (unsigned)nvx;
        mi->tier_polys[tier]++;
        mi->tier_vertices[tier] += (u16)nvx;
    }
    mi->polys = (u16)polys; mi->vertices = (u16)vertices;
    b->mesh_at[b->meshes] = (u16)b->pool_used;
    b->pool_used += need;
    b->meshes++;
    gxh_stats_meshes++;
    return 1;
}

static inline MeshInst *mesh_at(Buffer *b, int m) { return (MeshInst *)(b->pool + b->mesh_at[m]); }

/* ---- world map terrain ------------------------------------------------------------------------------------- */

/* wm_drawBlock's blocks (port/game/world_native.c) come here after the game's vertex pass, instead of as the
 * POLY_GT3s and fog POLY_G3s of its face routines (decomp src_equiv/ovl_0c/world_faces.c). Each face the PS1 would
 * draw is one polygon of the 3D engine: textured with its lit colours, or black where the PS1 draws only the fog
 * triangle, and fogged by the 3D engine's fog. The PS1 adds the fog triangle (blend mode 1, every face measured)
 * over the lit one, whose colours the vertex pass darkened toward a black far colour: lit + fog colour x d. The 3D
 * engine's fog gives colour x (1 - d) + fog colour x d, d from each pixel's depth by a table: so each vertex's
 * colour is its lit colour / (1 - d) (d by the first fog entry's offset; a vertex of the second entry gets the
 * first's ramp: the user's choice of 2026-10-25, native drawing with the hardware's fog). The scene is depth
 * tested: every primitive at the depth of its OT entry (world_depth), the terrain's vertices at theirs, so the
 * fog follows each pixel's depth. */
#define TB_MAX 24     /* blocks a frame (the game draws at most 17) */
#define TV_MAX 1700   /* projected vertices */
#define TF_MAX 2048   /* faces drawn (the 3D engine takes 2048 polygons) */
#define TU_MAX 32     /* texture page and CLUT pairs */
typedef struct { const u32 *faces; u16 vbase; u8 variant, pad; u32 alt[3]; } TBlock;
typedef struct { u32 xy; s16 z; u16 colour; } TVert;     /* screen position, depth (world_depth), RGB15 */
/* A vertex all fog (its lit colour weighs nothing there): the face's lit polygon takes another vertex's colour, so its
 * texture fades only by the fog across the face, linearly as the PS1's lit + fog colours do (emit_scene) */
#define TV_ALL_FOG 0xFFFF
typedef struct __attribute__((packed, aligned(2))) { u32 ref; u8 use, tier; } TFace; /* ref: block << 27 | face << 15 | otz << 2 | fog << 1 | lit;
                                                          * tier: the order it goes in when the 3D engine's RAM is
                                                          * short: fog-only faces first (the fog behind them is much
                                                          * the same), then lit ones, smallest first in each
                                                          * (drop_bounds, by twice its area) */
#define TERRAIN_TIERS (2 * DROP_TIERS)
typedef struct { u16 tpage, clut; u8 u0, v0, u1, v1, group, palette, scale, pad; } TUse; /* scale: plan_scale */
struct Terrain {
    unsigned gen;                    /* psx_ot_generation of the frame the blocks were drawn for */
    int buffer;                      /* the buffer they were drawn into */
    int nblocks, nverts, nfaces, nuses;
    int fog_mode;                    /* 0 none, 1 fog (wm_blockVertsFog), 2 far blocks' (wm_blockVertsFar) */
    u32 fog_colour, fog_offset;      /* the first fog entry's colour and fog offset */
    u32 backdrop_fog;                /* the backdrop's fog colour (g_worldBackdropFog) */
    TBlock blocks[TB_MAX];
    TUse uses[TU_MAX];
    TVert verts[TV_MAX];
    TFace faces[TF_MAX];
    u16 tier_polys[TERRAIN_TIERS];   /* the polygons the faces of each tier make (the backdrop's: up to two a face) */
};
_Static_assert(sizeof(struct Terrain) <= OVL0C_TERRAIN_BYTES, "the terrain overruns its place in ovl_0c");
_Static_assert(OVL0C_TERRAIN_AT + 3 * OVL0C_TERRAIN_BYTES <= OVL0C_CODE_END, "the world's tables overrun ovl_0c's code");
/* Three frames' terrain, by OT generation: the world map shows a buffer, and its scene is sent, only after the game
 * has drawn the next frame's blocks for that buffer (wm_endFrame: PutDispEnv before DrawOTagEnv), so a frame's
 * terrain is still needed while the next two are drawn. */
static struct Terrain *terrain_ring[3];
static struct Terrain *scene_terrain(const Buffer *b) {
    if (!b->slots) return NULL;
    struct Terrain *t = terrain_ring[b->scene_gen % 3];
    return t && t->gen == b->scene_gen && t->buffer == (int)(b - buffers) ? t : NULL;
}
int gxh_stats_terrain;

static void terrain_reset(struct Terrain *t) {
    t->gen = ~0u; t->nblocks = t->nverts = t->nfaces = t->nuses = 0;
    memset(t->tier_polys, 0, sizeof t->tier_polys);
}


/* the fog amount d (0-4096) at depth sz: the vertex pass's (world_verts.c wv_depth) */
static inline int terrain_fog(u32 sz, u32 ofs, int shift) {
    int d = (int)(sz - 0x8000 + ofs) >> shift;
    return d < 0 ? 0 : d > 0x1000 ? 0x1000 : d;
}

/* A face's three uv words as wf_face sends them (half-resolution and alternate pages by its OT entry) */
static inline void terrain_uvs(const u32 *face, const u32 *alt, int lod, int otz, u32 *o0, u32 *o1, u32 *o2) {
    u32 uv0 = face[1], uv1 = face[2], uv2 = face[3];
    if (!lod || otz < 0x9C4) { /* (WF_ALT never set on blocks) */
        if (lod && (uv2 & 0x20000)) {
            u32 a = alt[2];
            uv0 += a & 0xFFFF0000;
            uv1 = (uv1 & 0xFFFF) + (a << 16);
        }
    } else if (uv2 & 0x20000) {
        u32 a = alt[0], b = alt[1], tp = (a & 0xFFFF) << 16;
        a &= 0xFFFF0000;
        uv0 = ((((uv0 & 0xFEFE) >> 1) | (((uv0 & 0xFFFF0000) >> 6) + a)) + b);
        uv1 = (((uv1 & 0xFEFE) >> 1) | tp) + b;
        uv2 = ((uv2 & 0xFEFE) >> 1) + b;
    } else {
        uv0 = ((uv0 & 0xFEFE) >> 1) | ((uv0 & 0xFFFF0000) - 0x10000);
        uv1 = ((uv1 & 0xFEFE) >> 1) | (uv1 & 0xFE7F0000);
        uv2 = (uv2 & 0xFEFE) >> 1;
    }
    *o0 = uv0; *o1 = uv1; *o2 = uv2;
}

static inline int max3_index(u32 a, u32 b, u32 c) {
    if ((s32)(a - b) > 0) return (s32)(a - c) > 0 ? 0 : 2;
    return (s32)(b - c) > 0 ? 1 : 2;
}

PORT_OVERLAY_CODE(0c) int port_native_world_block(const NativeWorldBlock *n) {
    if (!active || !place->world) return 0;
    Buffer *b = buffer_at(n->draw_x, n->draw_y);
    if (!b || !b->slots) return 0;
    struct Terrain *t = terrain_ring[psx_ot_generation % 3];
    if (t->gen != psx_ot_generation) {
        terrain_reset(t);
        t->gen = psx_ot_generation;
        t->buffer = (int)(b - buffers);
        t->fog_mode = 0;
    }
    const u8 *blk = n->block;
    int nv = blk[0], nf = *(const u16 *)(blk + 2);
    nv = (nv + 2) / 3 * 3; /* (the vertex pass does whole groups of three) */
    if (!nf || t->nblocks == TB_MAX || t->nverts + nv > TV_MAX || t->nfaces == TF_MAX) return 0;
    int backdrop = n->variant == WORLD_BACKDROP;
    int fog = n->variant != WORLD_FACES_NEAR && !backdrop, shift = n->variant == WORLD_FACES_FAR ? 1 : 2;
    u32 fog_offset = n->fog_params[1] & 0xFFFF;
    if (fog) { t->fog_mode = n->variant == WORLD_FACES_FAR ? 2 : 1; t->fog_colour = n->fog_params[0]; t->fog_offset = fog_offset; }
    u32 bf = n->fog_params[0] & 0xFFFFFF, bf_max = 1;
    if (backdrop) { /* (its fog is added by translucent polygons: a vertex's fog colour is bf x d) */
        t->backdrop_fog = bf;
        u32 r = bf & 0xFF, g = bf >> 8 & 0xFF, bl = bf >> 16 & 0xFF;
        bf_max = r > g ? (r > bl ? r : bl) : (g > bl ? g : bl);
        if (!bf_max) bf_max = 1;
    }
    /* the vertices: depth on the scene's scale, colour lit / (1 - d) */
    int vbase = t->nverts;
    for (int i = 0; i < nv; ++i) {
        u32 sz = n->sz[i], lit = n->lit[i];
        int z = world_entry_depth((int)(sz >> 3) + 8);
        if (z < -32000) z = -32000;
        if (z > 32000) z = 32000;
        u32 r = lit & 0xFF, g = (lit >> 8) & 0xFF, bl = (lit >> 16) & 0xFF;
        int d = -1;
        if (backdrop) { /* the fog amount from the fog colour; the depth field holds it as an alpha (0-31) */
            u32 f = n->fog[i], fr = f & 0xFF, fg = f >> 8 & 0xFF, fb = f >> 16 & 0xFF;
            u32 fm = fr > fg ? (fr > fb ? fr : fb) : (fg > fb ? fg : fb);
            d = (int)(fm * 0x1000 / bf_max);
            if (d > 0x1000) d = 0x1000;
            z = (d * 31 + 0x800) >> 12;
        } else if (fog) d = terrain_fog(sz, fog_offset, shift);
        int all_fog = 0;
        if (d >= 0) {
            if (d >= 0x1000) all_fog = 1;
            else { u32 k = (0x1000u << 12) / (u32)(0x1000 - d); r = r * k >> 12; g = g * k >> 12; bl = bl * k >> 12; }
        }
        /* 128 = x1, in 32 steps; over x1 (TUse.scale) in 16 up to x2, with bit 15 set */
        u32 c;
        if (r > 124 || g > 124 || bl > 124) {
            r = r >> 3 > 31 ? 31 : r >> 3; g = g >> 3 > 31 ? 31 : g >> 3; bl = bl >> 3 > 31 ? 31 : bl >> 3;
            c = 0x8000 | r | g << 5 | bl << 10;
        } else {
            c = r >> 2 | (g >> 2) << 5 | (bl >> 2) << 10;
        }
        if (all_fog) c = TV_ALL_FOG;
        t->verts[vbase + i] = (TVert){n->sxy[i], (s16)z, (u16)c};
    }
    /* the faces wf_face would draw */
    int ti = t->nblocks;
    TBlock *tb = &t->blocks[ti];
    tb->faces = *(const u32 *const *)(blk + 8); tb->vbase = (u16)vbase; tb->variant = (u8)n->variant;
    if (n->alt) { tb->alt[0] = n->alt[0]; tb->alt[1] = n->alt[1]; tb->alt[2] = n->alt[2]; }
    int cull = n->variant == WORLD_FACES_NEAR, grid = n->variant == WORLD_FACES_FAR;
    int limit = n->variant == WORLD_FACES_NEAR ? 0xF6A : 0xFFE, need_lit = n->variant != WORLD_FACES_NEAR;
    int drawn = 0;
    for (int f = 0; f < nf && t->nfaces + drawn < TF_MAX; ++f) { /* (past TF_MAX the 3D engine has no room anyway) */
        const u32 *face = tb->faces + 4 * f;
        u32 w = face[0];
        int i0 = w & 0xFF, i1 = (w >> 8) & 0xFF, i2 = (w >> 16) & 0xFF;
        u32 s0 = n->sxy[i0], s1 = n->sxy[i1], s2 = n->sxy[i2];
        if (cull && (s0 & s1 & s2 & 0xFF008000)) continue;
        int x0 = (s16)s0, y0 = (s16)(s0 >> 16), x1 = (s16)s1, y1 = (s16)(s1 >> 16), x2 = (s16)s2, y2 = (s16)(s2 >> 16);
        int area2 = x0 * y1 + x1 * y2 + x2 * y0 - x0 * y2 - x1 * y0 - x2 * y1;
        if (area2 <= 0) continue;
        { /* the GPU draws no polygon 1024+ wide or 512+ high (the GTE clamps vertices off to the sides at +-1024) */
            int xa = x0 < x1 ? x0 : x1, xb = x0 < x1 ? x1 : x0, ya = y0 < y1 ? y0 : y1, yb = y0 < y1 ? y1 : y0;
            if (x2 < xa) xa = x2;
            if (x2 > xb) xb = x2;
            if (y2 < ya) ya = y2;
            if (y2 > yb) yb = y2;
            if (xb - xa >= 1024 || yb - ya >= 512) continue;
        }
        u32 bias = (w & 0x1000000) >> 15, z[3] = {n->sz[i0] + bias, n->sz[i1] + bias, n->sz[i2] + bias};
        int otz;
        if (backdrop) {
            otz = 0xFA0; /* (wm_backdropFaces: every face in one OT entry) */
        } else if (grid) {
            u32 zg[3] = {(z[0] >> 3) & ~0xFu, (z[1] >> 3) & ~0xFu, (z[2] >> 3) & ~0xFu};
            int k = max3_index(zg[0], zg[1], zg[2]);
            if ((s32)zg[k] <= 0 || (s32)zg[k] >= limit) continue;
            otz = (int)(z[k] >> 3);
        } else {
            otz = (int)(z[max3_index(z[0], z[1], z[2])] >> 3);
            if (otz <= 0 || otz >= limit) continue;
        }
        u32 lit_or = (n->lit[i0] | n->lit[i1] | n->lit[i2]) & 0xFFFFFF, fog_or = (n->fog[i0] | n->fog[i1] | n->fog[i2]) & 0xFFFFFF;
        int has_fog = fog && fog_or, has_lit = !(need_lit && !lit_or);
        if (backdrop) { /* a POLY_GT3 always, under its fog POLY_G3; that one opaque where the lit colours are black */
            has_fog = fog_or != 0;
            has_lit = !(has_fog && !lit_or);
        }
        if (!has_fog && !has_lit) continue;
        u8 use = 0xFF;
        if (has_lit) { /* its texture page and CLUT, and its texels' bounds */
            u32 uv0, uv1, uv2;
            terrain_uvs(face, tb->alt, !backdrop, otz, &uv0, &uv1, &uv2);
            u16 tpage = (u16)(uv1 >> 16), clut = (u16)(uv0 >> 16);
            int u;
            for (u = 0; u < t->nuses; ++u) if (t->uses[u].tpage == tpage && t->uses[u].clut == clut) break;
            if (u == t->nuses) {
                if (u == TU_MAX) { gxh_stats_mesh_fallback++; continue; }
                t->uses[u] = (TUse){tpage, clut, 255, 255, 0, 0, 0xFF, 0xFF, 16, 0};
                t->nuses++;
            }
            TUse *tu = &t->uses[u];
            for (int k = 0; k < 3; ++k) { /* its brightest channel over x1: the palette's scale */
                u32 c = t->verts[vbase + (k == 0 ? i0 : k == 1 ? i1 : i2)].colour;
                if (!(c & 0x8000) || c == TV_ALL_FOG) continue;
                u32 m = c & 31;
                if ((c >> 5 & 31) > m) m = c >> 5 & 31;
                if ((c >> 10 & 31) > m) m = c >> 10 & 31;
                if (m > tu->scale) tu->scale = (u8)(m > 32 ? 32 : m); /* (16ths: m is 8-bit / 8, 128 = 16) */
            }
            u8 us[3] = {(u8)uv0, (u8)uv1, (u8)uv2}, vs[3] = {(u8)(uv0 >> 8), (u8)(uv1 >> 8), (u8)(uv2 >> 8)};
            for (int k = 0; k < 3; ++k) {
                if (us[k] < tu->u0) tu->u0 = us[k];
                if (us[k] > tu->u1) tu->u1 = us[k];
                if (vs[k] < tu->v0) tu->v0 = vs[k];
                if (vs[k] > tu->v1) tu->v1 = vs[k];
            }
            use = (u8)u;
        }
        int tier = 0;
        while (tier < DROP_TIERS - 1 && area2 >= drop_bounds[tier]) ++tier;
        if (has_lit) tier += DROP_TIERS;
        t->tier_polys[tier] += (u16)(backdrop ? has_lit + has_fog : 1);
        t->faces[t->nfaces + drawn] =
            (TFace){(u32)ti << 27 | (u32)f << 15 | (u32)(otz & 0x1FFF) << 2 | (u32)has_fog << 1 | (u32)has_lit, use, (u8)tier};
        drawn++;
    }
    t->nfaces += drawn;
    t->nverts += nv;
    t->nblocks++;
    gxh_stats_terrain = t->nfaces;
    return 1;
}

/* A textured use's texture page as GP0 sees it: btl_drawMesh and btlbg_drawRecord or blend bits into the tpage. */
static inline void use_page(const MeshUse *u, Prim *p) {
    u32 tp = (u->page & 0xFFFF) | u->orbits, clut = u->page >> 16;
    p->tx = (u16)((tp & 15) * 64); p->ty = (u16)(((tp >> 4) & 1) * 256);
    p->abr = (u8)((tp >> 5) & 3);
    p->depth = (u8)((tp >> 7) & 3); if (p->depth == 3) p->depth = 2;
    p->clut_x = (u16)((clut & 0x3F) * 16); p->clut_y = (u16)((clut >> 6) & 0x1FF);
}

/* Clip against the drawing area (the 3D engine only clips to the viewport). */
/* Field: draw b's primitives [from, to) in software into PS1 VRAM, as the game drew them (only the UI pages' with
 * ui_only), exactly: whole-buffer drawing area, the dithering each was drawn with, no mask bits or texture window
 * (scenes with those go software when they appear). */
static u8 *prim_layer;
/* clip: VRAM x0, y0, x1, y1 inclusive to draw within (NULL: the whole buffer) */
static void replay_clip(Buffer *b, int from, int to, int layer /* -1: all, -2: the background sprites */, const int *clip) {
    Gp0State saved = gp0_state;
    gp0_state.area_x0 = b->x; gp0_state.area_y0 = b->y;
    gp0_state.area_x1 = b->x + b->w - 1; gp0_state.area_y1 = b->y + b->h - 1;
    if (clip) {
        if (gp0_state.area_x0 < clip[0]) gp0_state.area_x0 = clip[0];
        if (gp0_state.area_y0 < clip[1]) gp0_state.area_y0 = clip[1];
        if (gp0_state.area_x1 > clip[2]) gp0_state.area_x1 = clip[2];
        if (gp0_state.area_y1 > clip[3]) gp0_state.area_y1 = clip[3];
    }
    gp0_state.twin_mask_x = gp0_state.twin_mask_y = 0;
    gp0_state.set_mask = gp0_state.check_mask = 0;
    for (int k = from; k < to; ++k) {
        const Prim *p = &b->prims[k];
        if (layer >= 0 ? prim_layer[k] != layer : layer == -2 && !bg_prim(p)) continue;
        if (clip) { /* (most miss a small area: their bounds first) */
            int px0 = p->x[0], px1 = px0, py0 = p->y[0], py1 = py0;
            for (int i = 1; i < p->n; ++i) {
                if (p->x[i] < px0) px0 = p->x[i];
                if (p->x[i] > px1) px1 = p->x[i];
                if (p->y[i] < py0) py0 = p->y[i];
                if (p->y[i] > py1) py1 = p->y[i];
            }
            if (b->x + px1 < clip[0] || b->x + px0 > clip[2] + 1 || b->y + py1 < clip[1] || b->y + py0 > clip[3] + 1) continue;
        }
        gp0_state.texpage = (saved.texpage & ~0x200u) | (p->flags & P_DITHER ? 0x200u : 0);
        gp0_state.twin_mask_x = gp0_state.twin_mask_y = gp0_state.twin_off_x = gp0_state.twin_off_y = 0;
        if (p->flags & P_TWIN)
            for (int t = 0; t < b->twin_count; ++t)
                if (b->twins[t].k == k) {
                    gp0_state.twin_mask_x = b->twins[t].mask_x; gp0_state.twin_mask_y = b->twins[t].mask_y;
                    gp0_state.twin_off_x = b->twins[t].off_x; gp0_state.twin_off_y = b->twins[t].off_y;
                }
        Gp0Mode m = {!!(p->flags & P_TEXTURED), !!(p->flags & P_RAW), !!(p->flags & P_SEMI), !!(p->flags & P_GOURAUD),
                     p->abr, p->depth, p->tx, p->ty, p->clut_x, p->clut_y};
        if (p->flags & P_RECT) {
            Gp0Vtx v = {b->x + p->x[0], b->y + p->y[0], p->r[0], p->g[0], p->b[0], p->u[0], p->v[0]};
            psx_softgpu.rect(&m, &v, p->x[1] - p->x[0], p->y[2] - p->y[0]);
        } else {
            Gp0Vtx v[4];
            for (int i = 0; i < p->n; ++i)
                v[i] = (Gp0Vtx){b->x + p->x[i], b->y + p->y[i], p->r[i], p->g[i], p->b[i], p->u[i], p->v[i]};
            psx_softgpu.poly(&m, v, p->n);
        }
    }
    gp0_state = saved;
}
static void replay(Buffer *b, int from, int to, int layer /* -1: all */) { replay_clip(b, from, to, layer, NULL); }

/* Field: what the 3D engine can't draw exactly from here on (a subtractive blend, a texture window, mask bits, a
 * polygon past the drawing area, too many primitives) turns the scene into software: what it recorded is drawn
 * now, the rest as it comes; it is shown like the software renderer's frames. */
static void go_software(Buffer *b) {
    /* the next scenes will likely need it too: drawn in software from the start; but not for a buffer that filled
     * (the hold would keep the models' packets, which fill the next: the next scene tries with the models native) */
    if (place->field) soft_hold = b->count >= prims_max ? 0 : 60;
    if (place->field && b->slots) {
        /* the native models' polygons among the recorded primitives by OT entry (the walk's order, far to near) */
        fm_soft_next = 0x7FFF;
        for (int k = 0; k < b->count;) {
            int k1 = k + 1;
            while (k1 < b->count && b->slots[k1] == b->slots[k]) ++k1;
            fm_soft_upto(b, 0x1010 - b->slots[k] / 4);
            replay(b, k, k1, -1);
            k = k1;
        }
    } else replay(b, 0, b->count, -1);
    b->count = 0;
    b->soft = 1;
    dirty_cells(b->x, b->y, b->x + b->w - 1, b->y + b->h - 1);
}

static int clip_inside(const Gp0Vtx *v, int edge, int bound) {
    int value = edge < 2 ? v->x : v->y;
    return (edge & 1) ? value <= bound : value >= bound;
}

static Gp0Vtx clip_intersection(const Gp0Vtx *a, const Gp0Vtx *b, int edge, int bound) {
    int av = edge < 2 ? a->x : a->y, bv = edge < 2 ? b->x : b->y;
    Gp0Vtx p;
#define LERP(member) p.member = a->member + (int)((long long)(b->member - a->member) * (bound - av) / (bv - av))
    LERP(x); LERP(y); LERP(r); LERP(g); LERP(b); LERP(u); LERP(v);
#undef LERP
    if (edge < 2) p.x = bound; else p.y = bound;
    return p;
}

static void record_clipped_triangle(Buffer *buf, const Gp0Mode *m, const Gp0Vtx *v) {
    Gp0Vtx scratch[2][8];
    memcpy(scratch[0], v, 3 * sizeof *v);
    int count = 3, current = 0;
    int bounds[4] = {gp0_state.area_x0, gp0_state.area_x1 + 1, gp0_state.area_y0, gp0_state.area_y1 + 1};
    for (int edge = 0; edge < 4 && count; ++edge) {
        int out = 0;
        const Gp0Vtx *a = &scratch[current][count - 1];
        int a_in = clip_inside(a, edge, bounds[edge]);
        for (int i = 0; i < count; ++i) {
            const Gp0Vtx *b = &scratch[current][i];
            int b_in = clip_inside(b, edge, bounds[edge]);
            if (a_in != b_in) scratch[current ^ 1][out++] = clip_intersection(a, b, edge, bounds[edge]);
            if (b_in) scratch[current ^ 1][out++] = *b;
            a = b; a_in = b_in;
        }
        count = out; current ^= 1;
    }
    for (int i = 1; i + 1 < count; ++i) {
        Gp0Vtx t[3] = {scratch[current][0], scratch[current][i], scratch[current][i + 1]};
        if (triangle_area(&t[0], &t[1], &t[2])) record(buf, m, t, 3);
    }
}

/* ---- textures ---------------------------------------------------------------------------------------------- */

/* Texture VRAM (A-D, 512 KB) in 2 KB units, allocated buddy-style (power-of-two runs, aligned). */
#define TEX_UNIT 2048
#define TEX_UNITS 256
#define MAX_REGIONS 96

typedef struct {
    int used, depth, x, y, wh, h;      /* PS1 VRAM rectangle in halfwords */
    int tw, th;                         /* DS texture size in texels */
    u32 size_bits;                      /* TEXIMAGE's size fields for tw, th */
    int unit, units;
    int dirty, wanted;
    int conv, clut_x, clut_y;           /* conv: additive-blend copy of 4/8-bit texels through this CLUT (A5I3) */
    int cpal;                           /* conv: its 8-colour palette in ADD_SLOT */
} Region;

static Region regions[MAX_REGIONS];
static u8 unit_used[TEX_UNITS];
/* Untextured additive glows sample a fixed ramp whose alpha rises along U (A5I3, 32x8, white): each vertex's U is
 * its brightness, so the interpolated coordinate is a per-pixel alpha (the DS has none per vertex). */
#define RAMP_UNIT (TEX_UNITS - 1)
#define RAMP_PALETTE 63   /* its palette among the additive copies' 8-colour palettes (ADD_SLOT) */
static int ramp_written;

static inline int texels_per_halfword(int depth) { return depth == 0 ? 4 : depth == 1 ? 2 : 1; }
/* texels to halfwords: a shift (dividing by texels_per_halfword calls the ARM9's software division) */
static inline int texel_shift(int depth) { return 2 - depth; }

static int log2_at_least(int v) {
    int n = 0;
    while ((1 << n) < v) ++n;
    return n;
}

static int allocate_units(int units) {
    for (int start = 0; start + units <= TEX_UNITS; start += units) {
        int free = 1;
        for (int i = 0; i < units && free; ++i) if (unit_used[start + i]) free = 0;
        if (free) {
            memset(unit_used + start, 1, units);
            return start;
        }
    }
    return -1;
}

/* The additive copies' palettes (ADD_SLOT): users per palette; one made from a 4- or 8-bit CLUT serves every copy
 * through that CLUT (a 15-bit copy's is its own: the texels' colours) */
static u16 cpal_users[64];
static u16 cpal_hue[64]; /* each additive palette's brightest colour (light_under) */
static struct { u16 x, y; u8 depth; } cpal_clut[64];

static int cpal_take(int depth, int cx, int cy) {
    int free = -1;
    for (int i = 0; i < RAMP_PALETTE; ++i) {
        if (!cpal_users[i]) { if (free < 0) free = i; continue; }
        if (depth < 2 && cpal_clut[i].depth == depth && cpal_clut[i].x == cx && cpal_clut[i].y == cy) {
            cpal_users[i]++;
            return i;
        }
    }
    if (free < 0) return -1;
    cpal_users[free] = 1;
    cpal_clut[free].x = (u16)cx; cpal_clut[free].y = (u16)cy; cpal_clut[free].depth = (u8)depth;
    return free;
}

static void free_region(Region *r) {
    memset(unit_used + r->unit, 0, r->units);
    if (r->conv) cpal_users[r->cpal]--;
    r->used = 0;
}

static Region *region_containing(int depth, int conv, int cx, int cy, int x, int y, int wh, int h) {
    for (int i = 0; i < MAX_REGIONS; ++i) {
        Region *r = &regions[i];
        if (r->used && r->depth == depth && r->conv == conv && (!conv || depth == 2 || (r->clut_x == cx && r->clut_y == cy)) &&
            x >= r->x && y >= r->y && x + wh <= r->x + r->wh && y + h <= r->y + r->h)
            return r;
    }
    return NULL;
}

static Region *place_region(int depth, int conv, int cx, int cy, int x, int y, int wh, int h) {
    int sw = log2_at_least(wh * texels_per_halfword(depth)), sh = log2_at_least(h);
    if (sw < 3) sw = 3;
    if (sh < 3) sh = 3;
    if (sw > 10 || sh > 10) return NULL;
    int bytes = ((1 << sw) << sh) * (conv ? 8 : depth == 0 ? 4 : depth == 1 ? 8 : 16) / 8;
    int units = 1 << log2_at_least((bytes + TEX_UNIT - 1) / TEX_UNIT);
    int i = 0;
    while (i < MAX_REGIONS && regions[i].used) ++i;
    if (i == MAX_REGIONS) return NULL;
    int cpal = conv ? cpal_take(depth, cx, cy) : 0; /* an 8-colour palette */
    if (cpal < 0) return NULL;
    int unit = allocate_units(units);
    if (unit < 0) {
        if (conv) cpal_users[cpal]--;
        return NULL;
    }
    regions[i] = (Region){1, depth, x, y, wh, h, 1 << sw, 1 << sh, (u32)(sw - 3) << 20 | (u32)(sh - 3) << 23,
                          unit, units, 1, 1, conv, cx, cy, cpal};
    return &regions[i];
}

/* Texture palettes: 64 slots of 256 colours in F-G (32 KB). The last two hold the additive copies' 64 palettes of 8
 * colours (A5I3); the others take CLUTs. */
#define PAL_SLOTS 62
#define ADD_SLOT 62
/* The 3D engine draws additive light as dst x (1 - a) + hue x a (a the source's brightness), the PS1 as dst + source.
 * Over a uniform background d the two agree with d added to the hue (clamped): that's what turns the PS1's stacked
 * light white, where the plain hue stays coloured. The background is taken as ADD_WHITEN / 31 (the user's "whiter
 * approximation" of 2026-10-25; FF9's battle backgrounds behind effects are mostly about a quarter bright). */
#ifndef ADD_WHITEN
#define ADD_WHITEN 4
#endif
/* c + w per channel, clamped (w: 15-bit, a background per channel) */
static __attribute__((noinline)) u32 whiten_by(u32 c, u32 w) {
    u32 r = (c & 31) + (w & 31), g = (c >> 5 & 31) + (w >> 5 & 31), b = (c >> 10 & 31) + (w >> 10 & 31);
    return (r > 31 ? 31 : r) | (g > 31 ? 31 : g) << 5 | (b > 31 ? 31 : b) << 10;
}
#define ADD_BASE (ADD_WHITEN * 0x421u)
/* at most this much from the light under a primitive: where the earlier layers fell short of the PS1's brightness,
 * whitening toward its estimate shows the primitive's shape */
/* and none for primitives over this many cells: screen-sized layers (flashes, fades), not stacked glows */
#define ADD_STACK_CELLS 24
#ifndef ADD_STACK_MAX
#define ADD_STACK_MAX 8
#endif
static inline u32 whiten(u32 c) { return whiten_by(c, ADD_BASE); }
/* Stacked light: on the PS1 each additive layer's background is the light already added there, so the background
 * that whitens a primitive's vertex colours is the base plus what earlier additive primitives of the scene put under
 * it, per channel (blue light on blue saturates blue, it doesn't whiten), kept per 16-pixel cell: a primitive adds
 * its light (hue x brightness) to every cell whose centre it covers. The texture palettes (shared) take the base only. */
enum { LIGHT_W = 20, LIGHT_H = 15 };
static u8 light_cells[LIGHT_H][LIGHT_W][3];
static u32 add_white = ADD_BASE; /* this primitive's, for rgb_for */
/* whether (x, y) is inside triangle a b c (either winding) */
static inline int inside(int x, int y, int ax, int ay, int bx, int by, int cx, int cy) {
    int e0 = (bx - ax) * (y - ay) - (by - ay) * (x - ax);
    int e1 = (cx - bx) * (y - by) - (cy - by) * (x - bx);
    int e2 = (ax - cx) * (y - cy) - (ay - cy) * (x - cx);
    return (e0 >= 0 && e1 >= 0 && e2 >= 0) || (e0 <= 0 && e1 <= 0 && e2 <= 0);
}
static u32 light_under(const Prim *p, int alpha, u32 hue) {
    int x0 = p->x[0], x1 = x0, y0 = p->y[0], y1 = y0;
    for (int j = 1; j < p->n; ++j) {
        if (p->x[j] < x0) x0 = p->x[j]; else if (p->x[j] > x1) x1 = p->x[j];
        if (p->y[j] < y0) y0 = p->y[j]; else if (p->y[j] > y1) y1 = p->y[j];
    }
    x0 = x0 < 0 ? 0 : x0 >> 4; y0 = y0 < 0 ? 0 : y0 >> 4;
    x1 = x1 < 0 ? -1 : x1 >> 4; y1 = y1 < 0 ? -1 : y1 >> 4;
    if (x1 >= LIGHT_W) x1 = LIGHT_W - 1;
    if (y1 >= LIGHT_H) y1 = LIGHT_H - 1;
    u32 add[3] = {(hue & 31) * (u32)alpha / 31, (hue >> 5 & 31) * (u32)alpha / 31, (hue >> 10 & 31) * (u32)alpha / 31};
    u32 sum[3] = {0, 0, 0}, cells = 0;
    int rect = (p->flags & P_RECT) != 0;
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) {
            /* the cells whose centre the primitive covers (a primitive inside one cell: that cell) */
            int cx = x * 16 + 8, cy = y * 16 + 8;
            int in = rect || (x0 == x1 && y0 == y1) ||
                     inside(cx, cy, p->x[0], p->y[0], p->x[1], p->y[1], p->x[2], p->y[2]) ||
                     (p->n == 4 && inside(cx, cy, p->x[1], p->y[1], p->x[2], p->y[2], p->x[3], p->y[3]));
            if (!in) continue;
            u8 *l = light_cells[y][x];
            for (int c = 0; c < 3; ++c) {
                sum[c] += l[c];
                u32 v = l[c] + add[c];
                l[c] = (u8)(v > 255 ? 255 : v);
            }
            ++cells;
        }
    u32 w = 0;
    for (int c = 0; c < 3; ++c) {
        u32 under = cells && cells <= ADD_STACK_CELLS ? sum[c] / cells : 0;
        u32 v = ADD_WHITEN + (under > ADD_STACK_MAX ? ADD_STACK_MAX : under);
        w |= (v > 31 ? 31 : v) << (5 * c);
    }
    return w;
}
/* two 15-bit colours multiplied (31 = 1) */
static __attribute__((noinline)) u32 modulate(u32 a, u32 b) {
    return ((a & 31) * (b & 31) / 31) | ((a >> 5 & 31) * (b >> 5 & 31) / 31) << 5 | ((a >> 10 & 31) * (b >> 10 & 31) / 31) << 10;
}
typedef struct { int used, x, y, count, dirty, wanted, index0, scale; } Palette; /* index0: TEXIMAGE bit 29 (emit_scene);
                                                                               * scale: colours x scale / 16 (plan_scale) */
/* The scale plan_prim's palettes get (16: as they are). The PS1 modulates textures by up to twice (vertex colour 255
 * is x2), the DS by up to once: a world map use brighter than x1 gets its palette pre-scaled by its brightest channel
 * and its vertex colours divided by as much (TUse.scale), exact unless a scaled colour saturates. */
static int plan_scale = 16;
static Palette palettes[PAL_SLOTS];

static void regions_touch(int x, int y, int w, int h);
static void dirty_cells(int x0, int y0, int x1, int y1);
static void ui_flush(void);
static void ui_note_write(int x, int y, int w, int h);
static void ui_dirty_rect(int x0, int y0, int x1, int y1);
static int ui_flushing;

/* gpu.c: StoreImage and MoveImage are about to read this rectangle: deferred UI drawing lands first. */
/* A small read of a field scene's buffer (Fossil Roo polls one pixel off the screen's edge each frame): its
 * recorded primitives over the rectangle are drawn into it there alone, and the pixels go back after the copy, so
 * the scene stays in 3D. (Each such poll used to send the scene to software, whose held state then crashed field
 * 1403 at its transition.) */
#define PEEK_MAX 64
static u16 peek_saved[PEEK_MAX];
static int peek_rect[4], peek_active;
/* A write over field buffers' scenes (Buffer.over) */
static void over_note(int x, int y, int w, int h) {
    for (int i = 0; i < 2; ++i) {
        Buffer *b = &buffers[i];
        if (!b->valid || b->soft || !b->count || x >= b->x + b->w || b->x >= x + w || y >= b->y + b->h ||
            b->y >= y + h)
            continue;
        int n = 0;
        for (int k = 0; k < b->over_count; ++k) /* (those this one contains, or ones from fewer primitives, go) */
            if (b->over[k].count == b->count &&
                !(x <= b->over[k].x0 && y <= b->over[k].y0 && b->over[k].x1 < x + w && b->over[k].y1 < y + h))
                b->over[n++] = b->over[k];
        if (n == 4) { /* (the oldest goes: a read there replays the scene, as without these) */
            memmove(&b->over[0], &b->over[1], 3 * sizeof b->over[0]);
            n = 3;
        }
        b->over[n].x0 = (s16)x; b->over[n].y0 = (s16)y; b->over[n].x1 = (s16)(x + w - 1);
        b->over[n].y1 = (s16)(y + h - 1); b->over[n].count = b->count;
        b->over_count = n + 1;
    }
}
static int over_covers(const Buffer *b, int x, int y, int w, int h) {
    for (int k = 0; k < b->over_count; ++k)
        if (b->over[k].count == b->count && b->over[k].x0 <= x && b->over[k].y0 <= y && x + w - 1 <= b->over[k].x1 &&
            y + h - 1 <= b->over[k].y1)
            return 1;
    return 0;
}

int psx_vram_peek_begin(int x, int y, int w, int h) {
    if (!active || !place->field || w <= 0 || h <= 0 || w * h > PEEK_MAX) return 0;
    ui_flush();
    for (int i = 0; i < 2; ++i) {
        const Buffer *b = &buffers[i];
        if (b->valid && !b->soft && b->count && x < b->x + b->w && b->x < x + w && y < b->y + b->h && b->y < y + h &&
            !over_covers(b, x, y, w, h) && (x < b->x || y < b->y || x + w > b->x + b->w || y + h > b->y + b->h))
            return 0; /* (straddles a buffer's edge: the whole path) */
    }
    for (int j = 0; j < h; ++j)
        for (int k = 0; k < w; ++k) peek_saved[j * w + k] = psx_vram[((y + j) & 511) * 1024 + ((x + k) & 1023)];
    peek_rect[0] = x; peek_rect[1] = y; peek_rect[2] = w; peek_rect[3] = h; peek_active = 1;
    for (int i = 0; i < 2; ++i) {
        Buffer *b = &buffers[i];
        if (!b->valid || b->soft || !b->count || x >= b->x + b->w || b->x >= x + w || y >= b->y + b->h ||
            b->y >= y + h || over_covers(b, x, y, w, h))
            continue;
        int clip[4] = {x, y, x + w - 1, y + h - 1};
        replay_clip(b, 0, b->count, -1, clip);
    }
    return 1;
}
void psx_vram_peek_end(void) {
    if (!peek_active) return;
    peek_active = 0;
    int x = peek_rect[0], y = peek_rect[1], w = peek_rect[2], h = peek_rect[3];
    for (int j = 0; j < h; ++j)
        for (int k = 0; k < w; ++k) psx_vram[((y + j) & 511) * 1024 + ((x + k) & 1023)] = peek_saved[j * w + k];
}

void psx_vram_read(int x, int y, int w, int h) {
    if (!active) return;
    ui_flush();
    if (!place->field) return;
    /* a field scene's PS1 VRAM holds only its UI: one the game reads (a screen capture for a transition) goes
     * software first, so it reads the scene as the PS1 drew it; shown, it is sent again as such */
    for (int i = 0; i < 2; ++i) {
        Buffer *b = &buffers[i];
        if (!b->valid || b->soft || !b->count || x >= b->x + b->w || b->x >= x + w || y >= b->y + b->h ||
            b->y >= y + h || over_covers(b, x, y, w, h))
            continue;
        key_fill(b);
        go_software(b);
        b->submitted = 0;
        gxh_stats_soft |= 64;
    }
}

/* gpu.c: LoadImage, MoveImage, fills are about to write this rectangle. */
static unsigned bgc_tex;   /* VRAM writes outside the scene buffers (texture pages, CLUTs): the composite's */
void psx_vram_touch(int x, int y, int w, int h) {
    if (!active) return;
    ui_flush();
    if (place->field) {
        over_note(x, y, w, h);
        int in_buffer = 0;
        for (int i = 0; i < 2; ++i)
            if (buffers[i].valid && x >= buffers[i].x && y >= buffers[i].y && x + w <= buffers[i].x + buffers[i].w &&
                y + h <= buffers[i].y + buffers[i].h) in_buffer = 1;
        if (!in_buffer) bgc_tex++;
    }
    ui_note_write(x, y, w, h);
    ui_dirty_rect(x, y, x + w - 1, y + h - 1);
    dirty_cells(x, y, x + w - 1, y + h - 1);
    if (clear_fill) { memset(clear_fill->touched, 0, sizeof clear_fill->touched); clear_fill = NULL; }
    regions_touch(x, y, w, h);
}

static void runs_touch(int x, int y, int w, int h);
static void regions_touch(int x, int y, int w, int h) {
    runs_touch(x, y, w, h);
    for (int i = 0; i < MAX_REGIONS; ++i) {
        Region *r = &regions[i];
        if (r->used && x < r->x + r->wh && r->x < x + w && y < r->y + r->h && r->y < y + h) r->dirty = 1;
        if (r->used && r->conv && r->depth < 2 && y <= r->clut_y && r->clut_y < y + h && x < r->clut_x + (r->depth ? 256 : 16) &&
            r->clut_x < x + w) r->dirty = 1;
    }
    for (int i = 0; i < PAL_SLOTS; ++i) {
        Palette *p = &palettes[i];
        if (p->used && y <= p->y && p->y < y + h && x < p->x + p->count && p->x < x + w) p->dirty = 1;
    }
}

/* Field background sprites: 16x16-texel blocks of their pages, copied in runs (consecutive blocks of a page row of
 * blocks that the scene samples) into rows of 256 x 16 texels. A sprite's texels are consecutive blocks of one page
 * row, all sampled by it, so a row of sprites merged into one quad (extend_rect) is in one run. Holding only the
 * blocks a scene uses fits the fields whose pages don't fit as regions (a 64-texel-stepped bounding box each, per
 * CLUT for the additive copies). Polygons, and sprites across a block row, keep regions. */
#define RUN_ROWS 256
#define MAX_RUNS 512
#define MAX_NEEDS 384
#define RUN_NONE 0xFFFF
#define RUN_GROUP 0xFE    /* prim_group: the primitive's texels are in a run */
#define RUN_SIZE_BITS (5u << 20 | 1u << 23) /* TEXIMAGE: 256 x 16 */
#define RUN_TABLES (RUN_ROWS * 4 + MAX_RUNS * 12 + MAX_NEEDS * 8 + 32) /* rows, runs, needs (+ alignment) */
/* a page row of blocks: page x / 64 (4 bits), page y / 256 (1), depth (2), additive copy (1), block row (4), and for
 * an additive copy its CLUT: x / 16 (6), y (9) */
#define STRIP_KEY(depth, conv, tx, ty, cx, cy, bv) \
    ((u32)(tx) >> 6 | (u32)(ty) >> 8 << 4 | (u32)(depth) << 5 | (u32)(conv) << 7 | (u32)(bv) << 8 | \
     (conv ? (u32)(cx) >> 4 << 12 | (u32)(cy) << 18 : 0))
#define KEY_TX(k) (((k) & 15) << 6)
#define KEY_TY(k) (((k) >> 4 & 1) << 8)
#define KEY_DEPTH(k) ((k) >> 5 & 3)
#define KEY_CONV(k) ((k) >> 7 & 1)
#define KEY_BV(k) ((k) >> 8 & 15)
#define KEY_CX(k) (((k) >> 12 & 63) << 4)
#define KEY_CY(k) ((k) >> 18 & 511)
enum { RUN_USED = 1, RUN_DIRTY = 2, RUN_WANTED = 4 };
typedef struct { u8 unit, fmt_units; u16 slots; } Row;  /* fmt << 4 | units (0: free); slots: a bit per block in use */
typedef struct { u32 key; u8 bu, n, slot, flags, cpal, row; u16 next; } Run; /* blocks bu.., at slot.. of row */
typedef struct { u32 key; u16 mask, next; } Need;        /* the blocks of a page row a scene samples */
static Row *rows;         /* these three after the first field buffer in ovl_0a's dead code (use_place; NULL: none) */
static Run *runs;
static Need *needs;
static u16 run_hash[256], need_hash[256];
static int need_count, run_cursor;
static int field_runs;    /* this field's pages overflowed as regions: its sprites go to runs (until the mode ends) */

static inline int run_format(u32 key) { return KEY_CONV(key) ? 3 : (int)KEY_DEPTH(key); } /* 3: A5I3 */
static inline unsigned key_hash(u32 key) { return (key ^ key >> 8 ^ key >> 16 ^ key >> 24) & 255; }

static void runs_reset(void) {
    if (!runs) return;
    memset(rows, 0, RUN_ROWS * sizeof *rows);
    memset(runs, 0, MAX_RUNS * sizeof *runs);
    memset(run_hash, 0xFF, sizeof run_hash);
}

static void free_run(int i) {
    Run *r = &runs[i];
    Row *w = &rows[r->row];
    w->slots &= (u16)~(((1u << r->n) - 1) << r->slot);
    if (!w->slots) { memset(unit_used + w->unit, 0, w->fmt_units & 15); w->fmt_units = 0; }
    if (KEY_CONV(r->key)) cpal_users[r->cpal]--;
    u16 *link = &run_hash[key_hash(r->key)];
    while (*link != i) link = &runs[*link].next;
    *link = r->next;
    r->flags = 0;
}

/* the cached run holding blocks bu..bu_last of a page row, or NULL */
static Run *run_holding(u32 key, int bu, int bu_last) {
    for (u16 i = run_hash[key_hash(key)]; i != RUN_NONE; i = runs[i].next) {
        Run *r = &runs[i];
        if (r->key == key && r->bu <= bu && bu_last < r->bu + r->n) return r;
    }
    return NULL;
}

/* this scene samples blocks bu..bu_last of a page row (0: the table is full) */
static int need_blocks(u32 key, int bu, int bu_last) {
    unsigned h = key_hash(key);
    u16 i = need_hash[h];
    while (i != RUN_NONE && needs[i].key != key) i = needs[i].next;
    if (i == RUN_NONE) {
        if (need_count == MAX_NEEDS) return 0;
        i = (u16)need_count++;
        needs[i] = (Need){key, 0, need_hash[h]};
        need_hash[h] = i;
    }
    needs[i].mask |= (u16)(((2u << (bu_last - bu)) - 1) << bu);
    return 1;
}

static Run *place_run(u32 key, int bu, int n) {
    int fmt = run_format(key), units = fmt == 0 ? 1 : fmt == 2 ? 4 : 2, row = -1, slot = 0;
    u16 span = (u16)((1u << n) - 1);
    for (int w = 0; w < RUN_ROWS && row < 0; ++w) {
        if (rows[w].fmt_units != (fmt << 4 | units)) continue;
        for (int s = 0; s + n <= 16; ++s)
            if (!(rows[w].slots >> s & span)) { row = w; slot = s; break; }
    }
    int fresh = row < 0;
    if (fresh) {
        for (row = 0; row < RUN_ROWS && rows[row].fmt_units; ++row) {}
        if (row == RUN_ROWS) return NULL;
        int unit = allocate_units(units);
        if (unit < 0) return NULL;
        rows[row] = (Row){(u8)unit, (u8)(fmt << 4 | units), 0};
    }
    int conv = KEY_CONV(key), cpal = conv ? cpal_take(KEY_DEPTH(key), KEY_CX(key), KEY_CY(key)) : 0;
    int i = run_cursor, tries = MAX_RUNS;
    while (runs[i].flags && --tries) i = (i + 1) % MAX_RUNS;
    if (cpal < 0 || runs[i].flags) {
        if (cpal >= 0 && conv) cpal_users[cpal]--;
        if (fresh) { memset(unit_used + rows[row].unit, 0, units); rows[row].fmt_units = 0; }
        return NULL;
    }
    run_cursor = (i + 1) % MAX_RUNS;
    rows[row].slots |= (u16)(span << slot);
    unsigned h = key_hash(key);
    runs[i] = (Run){key, (u8)bu, (u8)n, (u8)slot, RUN_USED | RUN_DIRTY, (u8)cpal, (u8)row, run_hash[h]};
    run_hash[h] = (u16)i;
    return &runs[i];
}

/* Mark the cached runs holding the scene's blocks wanted; with place_missing, put the others in rows. Returns the
 * runs missing (not placed). */
static int plan_runs(int place_missing) {
    int missing = 0;
    for (int k = 0; k < need_count; ++k) {
        const Need *q = &needs[k];
        for (int bu = 0; bu < 16;) {
            if (!(q->mask >> bu & 1)) { ++bu; continue; }
            int n = 1;
            while (bu + n < 16 && (q->mask >> (bu + n) & 1)) ++n;
            Run *r = run_holding(q->key, bu, bu + n - 1);
            if (!r && place_missing) r = place_run(q->key, bu, n);
            if (r) r->flags |= RUN_WANTED;
            else missing++;
            bu += n;
        }
    }
    return missing;
}

static void runs_touch(int x, int y, int w, int h) {
    if (!runs) return;
    for (int i = 0; i < MAX_RUNS; ++i) {
        Run *r = &runs[i];
        if (!r->flags) continue;
        u32 key = r->key;
        int sh = texel_shift(KEY_DEPTH(key)), rx = KEY_TX(key) + (r->bu * 16 >> sh), ry = KEY_TY(key) + KEY_BV(key) * 16;
        if (x < rx + (r->n * 16 >> sh) && rx < x + w && y < ry + 16 && ry < y + h) r->flags |= RUN_DIRTY;
        if (KEY_CONV(key)) {
            int cx = KEY_CX(key), cy = KEY_CY(key);
            if (y <= cy && cy < y + h && x < cx + (KEY_DEPTH(key) ? 256 : 16) && cx < x + w) r->flags |= RUN_DIRTY;
        }
    }
}

/* Copy a run's blocks from PS1 VRAM to its row (banks LCDC) */
static int conv_choose(int depth, int x, int y, int wh, int h, int clut_x, int clut_y, int cpal, u16 *reps, u8 *code);
static int run_code_cpal = -1; /* the palette upload_run's codes are for (-1 at each upload) */
static void upload_run(const Run *r, u8 *base) {
    u32 key = r->key;
    int depth = KEY_DEPTH(key), conv = KEY_CONV(key), per = texels_per_halfword(depth), fmt = run_format(key);
    int row_bytes = fmt == 0 ? 128 : fmt == 2 ? 512 : 256;
    int x = KEY_TX(key) + (r->bu * 16 >> texel_shift(depth)), wh = r->n * 16 >> texel_shift(depth), y = KEY_TY(key) + KEY_BV(key) * 16;
    u8 *dst = base + rows[r->row].unit * TEX_UNIT + r->slot * (fmt == 0 ? 8 : fmt == 2 ? 32 : 16);
    static u8 code[256];
    static u16 pair4[256];
    if (conv && run_code_cpal != r->cpal) { /* the CLUT's codes, kept for the next run through the palette */
        u16 reps[8];
        conv_choose(depth, 0, 0, 0, 0, KEY_CX(key), KEY_CY(key), r->cpal, reps, code);
        if (!depth) for (int b = 0; b < 256; ++b) pair4[b] = code[b & 15] | code[b >> 4] << 8;
        run_code_cpal = r->cpal;
    }
    for (int line = 0; line < 16; ++line) {
        const u16 *src = psx_vram + ((y + line) & 511) * 1024;
        u16 *out = (u16 *)(dst + line * row_bytes);
        if (conv) {
            if (depth) for (int k = 0; k < wh; ++k) { u16 c = src[(x + k) & 1023]; out[k] = code[c & 255] | code[c >> 8] << 8; }
            else for (int k = 0; k < wh; ++k) { u16 c = src[(x + k) & 1023]; out[2 * k] = pair4[c & 255]; out[2 * k + 1] = pair4[c >> 8]; }
        } else if (depth == 2) {
            for (int k = 0; k < wh; ++k) { u16 c = src[(x + k) & 1023]; out[k] = c ? c | 0x8000 : 0; }
        } else {
            for (int k = 0; k < wh; ++k) out[k] = src[(x + k) & 1023];
        }
    }
}

/* Per-scene texture plan: one group per (depth, page) with its texels' bounding box (halfwords, rows). */
#define MAX_GROUPS 96
typedef struct { int depth, tx, ty, x0, y0, x1, y1; Region *region; int conv, cx, cy; } Group;
static Group groups[MAX_GROUPS];
static int group_count;
/* per recorded primitive (prim_tables: in the resident mode overlay's dead range, use_place) */
static u8 *prim_group, *prim_palette;

/* hints for plan_textures' searches: the group or palette slot a key's hash last found (checked before use) */
static u8 group_hash[256], palette_hash[256];

/* Which layer each primitive goes to: the 3D engine (all of a battle's), or in a field the software background layer
 * or the 2D layer (field_layers). */
enum { L_3D, L_UI, L_ADD /* field: an additive primitive, drawn exactly in software into the added layer */,
       L_BACK /* field: a background sprite behind every 3D primitive: only in the composite (bgc) */ };
static inline int in_scene(const Prim *p, int k) { (void)p; return prim_layer[k] == L_3D; }

/* One textured primitive's texture group and palette slot (0xFF: none) for plan_textures. */
static __attribute__((noinline)) void plan_prim(const Prim *p, int use_runs, int *last_group, int *last_palette, u8 *group_out,
                             u8 *palette_out) {
    *group_out = 0xFF;
    *palette_out = 0xFF;
    int sh = texel_shift(p->depth);
    int u0 = p->u[0], u1 = u0, v0 = p->v[0], v1 = v0;
    for (int i = 1; i < p->n; ++i) {
        if (p->u[i] < u0) u0 = p->u[i];
        if (p->u[i] > u1) u1 = p->u[i];
        if (p->v[i] < v0) v0 = p->v[i];
        if (p->v[i] > v1) v1 = p->v[i];
    }
    /* Additive polygons (modes 1, 3) use a 15-bit copy through their CLUT whose dark texels are transparent:
     * under alpha blending a black background would darken what an additive one leaves alone. */
    int conv = (p->flags & P_SEMI) && p->abr != 0; /* modes 1-3: brightness as alpha (conv_upload) */
    if (use_runs && (p->flags & P_RECT) && !(conv && p->depth == 2) && v0 >> 4 == v1 >> 4 &&
        need_blocks(STRIP_KEY(p->depth, conv, p->tx, p->ty, p->clut_x, p->clut_y, v0 >> 4), u0 >> 4, u1 >> 4)) {
        *group_out = RUN_GROUP;
        goto palette;
    }
    int x0, x1, y0, y1;
    if (conv) { /* effect-sized: the texels' own box, in 8-texel steps */
        u0 &= ~7; u1 |= 7;
        x0 = p->tx + (u0 >> sh); x1 = p->tx + (u1 >> sh); y0 = p->ty + (v0 & ~7); y1 = p->ty + (v1 | 7);
    } else {
        /* Stable regions: U and V in 64-texel steps, so a page keeps its region (and copies nothing) while the
         * camera moves over it, without holding the parts of the page no primitive uses. */
        u0 &= ~63; u1 |= 63; v0 &= ~63; v1 |= 63;
        x0 = p->tx + (u0 >> sh); x1 = p->tx + (u1 >> sh); y0 = p->ty + v0; y1 = p->ty + v1;
    }
    /* consecutive primitives nearly always share a page and CLUT: try the last match first */
#define SAME_GROUP(q) ((q).depth == p->depth && (q).tx == p->tx && (q).ty == p->ty && (q).conv == conv && \
                   (!conv || p->depth == 2 || ((q).cx == p->clut_x && (q).cy == p->clut_y)))
    int g = *last_group;
    if (g >= group_count || !SAME_GROUP(groups[g])) {
        /* then the group this key last mapped to (a hash of page, depth and CLUT), then a search */
        unsigned h = (p->tx >> 6 ^ p->ty >> 4 ^ p->depth << 4 ^ conv << 6 ^ p->clut_x >> 4 ^ p->clut_y * 7) & 255;
        g = group_hash[h];
        if (g >= group_count || !SAME_GROUP(groups[g])) {
            for (g = 0; g < group_count; ++g)
                if (SAME_GROUP(groups[g])) break;
            if (g < group_count) group_hash[h] = (u8)g;
            else if (group_count < MAX_GROUPS) group_hash[h] = (u8)group_count;
        }
    }
#undef SAME_GROUP
    *last_group = g;
    if (g == group_count) {
        if (group_count == MAX_GROUPS) return;
        groups[group_count++] = (Group){p->depth, p->tx, p->ty, x0, y0, x1, y1, NULL, conv, p->clut_x, p->clut_y};
    } else {
        Group *q = &groups[g];
        if (x0 < q->x0) q->x0 = x0;
        if (x1 > q->x1) q->x1 = x1;
        if (y0 < q->y0) q->y0 = y0;
        if (y1 > q->y1) q->y1 = y1;
    }
    *group_out = (u8)g;
palette:
    if (p->depth < 2) {
        int count = p->depth ? 256 : 16, s = *last_palette, empty = -1;
        Palette *lq = &palettes[s];
        if (!(lq->used && lq->x == p->clut_x && lq->y == p->clut_y && lq->count >= count && lq->scale == plan_scale)) {
            /* then the slot this CLUT last had, then a search (which also finds the first free slot) */
            unsigned h = (p->clut_x >> 4 ^ p->clut_y * 13) & 255;
            s = palette_hash[h];
            lq = &palettes[s];
            if (!(s < PAL_SLOTS && lq->used && lq->x == p->clut_x && lq->y == p->clut_y && lq->count >= count &&
                  lq->scale == plan_scale)) {
                for (s = 0; s < PAL_SLOTS; ++s) {
                    Palette *q = &palettes[s];
                    if (q->used && q->x == p->clut_x && q->y == p->clut_y && q->count >= count && q->scale == plan_scale) break;
                    if (!q->used && empty < 0) empty = s;
                }
                if (s < PAL_SLOTS) palette_hash[h] = (u8)s;
            }
        }
        if (s == PAL_SLOTS) {
            if (empty < 0)
                for (int t = 0; t < PAL_SLOTS; ++t) if (!palettes[t].wanted) { empty = t; break; }
            if (empty < 0) return;
            s = empty;
            palettes[s] = (Palette){1, p->clut_x, p->clut_y, count, 1, 0, 0, plan_scale};
        }
        palettes[s].wanted = 1;
        *palette_out = (u8)s;
        *last_palette = s;
    }
}

static __attribute__((noinline)) int plan_textures(Buffer *b) {
    int last_group = 0, last_palette = 0;
    group_count = 0;
    for (int i = 0; i < MAX_REGIONS; ++i) regions[i].wanted = 0;
    for (int i = 0; i < PAL_SLOTS; ++i) palettes[i].wanted = 0;
    int use_runs = runs && place->field && field_runs;
    need_count = 0;
    if (use_runs) {
        memset(need_hash, 0xFF, sizeof need_hash);
        for (int i = 0; i < MAX_RUNS; ++i) runs[i].flags &= (u8)~RUN_WANTED;
    }
    for (int k = 0; k < b->count; ++k) {
        prim_group[k] = 0xFF;
        if (!(b->prims[k].flags & P_TEXTURED) || !in_scene(&b->prims[k], k)) continue;
        PrimCopy local; /* (the stores below can't alias it: emit_scene) */
        copy_prim(&local, &b->prims[k]);
        plan_prim(&local.p, use_runs, &last_group, &last_palette, &prim_group[k], &prim_palette[k]);
    }
    /* the world terrain's textures: each page and CLUT as a primitive covering its texels */
    struct Terrain *tr = scene_terrain(b);
    if (tr)
        for (int i = 0; i < tr->nuses; ++i) {
            TUse *u = &tr->uses[i];
            MeshUse mu = {(u32)u->tpage | (u32)u->clut << 16, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
            Prim q;
            q.n = 4; q.flags = P_TEXTURED;
            use_page(&mu, &q);
            q.u[0] = q.u[2] = u->u0; q.u[1] = q.u[3] = u->u1; q.v[0] = q.v[1] = u->v0; q.v[2] = q.v[3] = u->v1;
            plan_scale = u->scale;
            plan_prim(&q, 0, &last_group, &last_palette, &u->group, &u->palette);
            plan_scale = 16;
        }
    FStore *fst = scene_models(b); /* the field models' (fm_uses: field_frame) */
    if (fst)
        for (int i = 0; i < fst->nuses; ++i) {
            FUse *u = &fst->uses[i];
            MeshUse mu = {(u32)u->tpage | (u32)u->clut << 16, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
            Prim q;
            q.n = 4; q.flags = P_TEXTURED;
            use_page(&mu, &q);
            q.u[0] = q.u[2] = u->u0; q.u[1] = q.u[3] = u->u1; q.v[0] = q.v[1] = u->v0; q.v[2] = q.v[3] = u->v1;
            plan_prim(&q, 0, &last_group, &last_palette, &u->group, &u->palette);
        }
    /* the native meshes' textures: each use as a primitive covering its texels */
    if (b->meshes && b->mesh_gen == b->scene_gen)
        for (int m = 0; m < b->meshes; ++m) {
            MeshInst *mi = mesh_at(b, m);
            for (int i = 0; i < mi->nuses; ++i) {
                MeshUse *u = &mi->uses[i];
                Prim q;
                q.n = 4; q.flags = (u8)(P_TEXTURED | (u->semi ? P_SEMI : 0));
                use_page(u, &q);
                q.u[0] = q.u[2] = u->u0; q.u[1] = q.u[3] = u->u1; q.v[0] = q.v[1] = u->v0; q.v[2] = q.v[3] = u->v1;
                plan_prim(&q, 0, &last_group, &last_palette, &u->group, &u->palette);
            }
        }
    /* Keep regions that already hold a group's texels; place the rest, largest first, after freeing the unwanted.
     * If that fails (fragmentation), start the texture memory over and place every group largest first. */
    int missing = 0;
    for (int g = 0; g < group_count; ++g) {
        Group *q = &groups[g];
        q->region = region_containing(q->depth, q->conv, q->cx, q->cy, q->x0, q->y0, q->x1 - q->x0 + 1,
                                      q->y1 - q->y0 + 1);
        if (q->region) q->region->wanted = 1;
        else missing = 1;
    }
    if (need_count && plan_runs(0)) missing = 1;
    if (!missing) { gxh_stats_unplaced = 0; return 1; }
    for (int i = 0; i < MAX_REGIONS; ++i) if (regions[i].used && !regions[i].wanted) free_region(&regions[i]);
    if (use_runs) for (int i = 0; i < MAX_RUNS; ++i) if (runs[i].flags && !(runs[i].flags & RUN_WANTED)) free_run(i);
    for (int attempt = 0; attempt < 2; ++attempt) {
        int failed = 0;
        static u8 tried[MAX_GROUPS];
        memset(tried, 0, sizeof tried);
        for (;;) {
            int best = -1, best_area = -1;
            for (int g = 0; g < group_count; ++g) {
                Group *q = &groups[g];
                int area = (q->x1 - q->x0 + 1) * texels_per_halfword(q->depth) * (q->y1 - q->y0 + 1);
                if (!q->region && !tried[g] && area > best_area) { best = g; best_area = area; }
            }
            if (best < 0) break;
            Group *q = &groups[best];
            tried[best] = 1;
            int wh = q->x1 - q->x0 + 1, h = q->y1 - q->y0 + 1;
            q->region = region_containing(q->depth, q->conv, q->cx, q->cy, q->x0, q->y0, wh, h);
            if (!q->region) q->region = place_region(q->depth, q->conv, q->cx, q->cy, q->x0, q->y0, wh, h);
            if (q->region) q->region->wanted = 1;
            else failed = 1;
        }
        int runs_left = need_count ? plan_runs(1) : 0; /* after the regions: rows fill the gaps they leave */
        if (!failed && !runs_left) { gxh_stats_unplaced = 0; break; }
        if (attempt) {
            gxh_stats_unplaced = runs_left;
            for (int g = 0; g < group_count; ++g) gxh_stats_unplaced += !groups[g].region;
            if (runs && place->field && !field_runs) { /* again with sprites in runs (cheaper while pages fit) */
                field_runs = 1;
                for (int i = 0; i < MAX_REGIONS; ++i) if (regions[i].used) free_region(&regions[i]);
                return plan_textures(b);
            }
            break;
        }
        for (int g = 0; g < group_count; ++g) groups[g].region = NULL;
        for (int i = 0; i < MAX_REGIONS; ++i) if (regions[i].used) free_region(&regions[i]);
        if (use_runs) for (int i = 0; i < MAX_RUNS; ++i) if (runs[i].flags) free_run(i);
    }
    return 1;
}

/* An additive-blend copy (PS1 modes 1 and 3: destination + source). The DS only alpha-blends, so each texel's
 * alpha is its brightest channel and its colour is normalised by it: source x alpha is the PS1 texel, and what
 * remains of the destination, (1 - alpha), is exact for dim texels and fades only under bright ones. A gradient
 * of one hue normalises to one colour, so 8 per texture (A5I3, chosen across its CLUT) are enough; texels without
 * the semi-transparency bit are opaque, colour 0 transparent. */
/* 31 / m in 16.16 rounded up (exact for c <= m), for the per-texel normalisation (the ARM9 divides in software) */
static const u32 inv31[32] = {
    0, 2031616, 1015808, 677206, 507904, 406324, 338603, 290231, 253952, 225736, 203162, 184693, 169302, 156279,
    145116, 135442, 126976, 119507, 112868, 106928, 101581, 96744, 92347, 88332, 84651, 81265, 78140, 75246, 72558, 70056, 67721, 65536};

static inline u16 normalised(u16 c, int *bright) {
    int cr = c & 31, cg = (c >> 5) & 31, cb = (c >> 10) & 31, m = cr > cg ? (cr > cb ? cr : cb) : (cg > cb ? cg : cb);
    *bright = m;
    u32 k = inv31[m];
    return (u16)((cr * k) >> 16 | ((cg * k) >> 16) << 5 | ((cb * k) >> 16) << 10);
}

static inline int colour_distance(u16 a, u16 b) {
    int dr = (a & 31) - (b & 31), dg = ((a >> 5) & 31) - ((b >> 5) & 31), db = ((a >> 10) & 31) - ((b >> 10) & 31);
    return dr * dr + dg * dg + db * db;
}

/* the A5I3 alpha of a texel: transparent 0x0000, opaque without the semi-transparency bit, else its brightness */
static inline u8 texel_alpha(u16 c, int bright) { return !c ? 0 : (u8)(c & 0x8000 ? bright : 31); }

static u32 conv_scratch[32768 / 32]; /* conv_choose's colours seen (a bit each), conv_upload's nearest choices */

/* Choose an additive copy's 8 colours (from its 4- or 8-bit CLUT, or a sample of its 15-bit texels at x, y, wh, h)
 * and write them as palette cpal; code: each CLUT index's A5I3 texel (4- and 8-bit). Returns the colours chosen. */
static __attribute__((noinline)) int conv_choose(int depth, int x, int y, int wh, int h, int clut_x, int clut_y, int cpal,
                                                 u16 *reps, u8 *code) {
    /* the colours to choose from: the CLUT's, or (15-bit) a sample of the texels */
    enum { CHOICES = 256 };
    static u16 norm[CHOICES];
    static u8 alpha[CHOICES];
    int count = 0;
    const u16 *clut = psx_vram + (clut_y & 511) * 1024;
    if (depth < 2) {
        count = depth ? 256 : 16;
        for (int i = 0; i < count; ++i) {
            u16 c = clut[(clut_x + i) & 1023];
            int m;
            norm[i] = normalised(c, &m);
            alpha[i] = texel_alpha(c, m);
        }
    } else {
        int step_y = h / 16 > 0 ? h / 16 : 1, step_x = wh / 16 > 0 ? wh / 16 : 1;
        for (int j = 0; j < h && count < CHOICES; j += step_y)
            for (int i = 0; i < wh && count < CHOICES; i += step_x) {
                u16 c = psx_vram[((y + j) & 511) * 1024 + ((x + i) & 1023)];
                int m;
                norm[count] = normalised(c, &m);
                alpha[count++] = texel_alpha(c, m);
            }
    }
    /* representatives: the brightest entry's colour, then repeatedly the colour farthest from those chosen. A
     * CLUT's entries mostly share a few hues, so the search runs over the distinct normalised colours (the
     * brightest entry of each), with each one's distance to the chosen set kept as it grows. */
    u32 *seen = conv_scratch; /* zero between uses (conv_upload clears its use) */
    static u16 cand[CHOICES];
    static u8 cand_alpha[CHOICES];
    static int cand_d[CHOICES];
    int ncand = 0;
    for (int i = 0; i < count; ++i) {
        if (!alpha[i]) continue;
        u16 n = norm[i];
        if (seen[n >> 5] >> (n & 31) & 1) {
            for (int k = 0; k < ncand; ++k) if (cand[k] == n && alpha[i] > cand_alpha[k]) cand_alpha[k] = alpha[i];
            continue;
        }
        seen[n >> 5] |= 1u << (n & 31);
        cand[ncand] = n; cand_alpha[ncand] = alpha[i]; cand_d[ncand] = 1 << 30; ++ncand;
    }
    for (int k = 0; k < ncand; ++k) seen[cand[k] >> 5] = 0;
    int nreps = 0;
    while (nreps < 8) {
        int best = -1, best_d = 0;
        for (int k = 0; k < ncand; ++k) {
            int d = nreps ? cand_d[k] : cand_alpha[k]; /* first: the brightest */
            if (d > best_d) { best_d = d; best = k; }
        }
        if (best < 0) break;
        reps[nreps++] = cand[best];
        for (int k = 0; k < ncand; ++k) {
            int e = colour_distance(cand[k], cand[best]);
            if (e < cand_d[k]) cand_d[k] = e;
        }
    }
    if (!nreps) reps[nreps++] = 0x7FFF;
    u16 *pal = (u16 *)VRAM_F + ADD_SLOT * 256 + cpal * 8;
    for (int k = 0; k < 8; ++k) pal[k] = k < nreps ? (u16)whiten(reps[k]) : 0;
    cpal_hue[cpal & 63] = reps[0];
    if (depth < 2)
        for (int i = 0; i < count; ++i) {
            int best = 0, best_d = 1 << 30;
            for (int k = 0; k < nreps; ++k) {
                int e = colour_distance(norm[i], reps[k]);
                if (e < best_d) { best_d = e; best = k; }
            }
            code[i] = (u8)(alpha[i] << 3 | best);
        }
    return nreps;
}

static __attribute__((noinline)) void conv_upload(Region *r, u8 *dst, int row_bytes) {
    u16 reps[8];
    static u8 code[256]; /* A5I3 texel per CLUT index */
    int nreps = conv_choose(r->depth, r->x, r->y, r->wh, r->h, r->clut_x, r->clut_y, r->cpal, reps, code);
    if (r->depth < 2) {
        /* texel pairs (VRAM ignores byte writes): 4-bit, one source byte is a pair, through a 256-entry table */
        static u16 pair4[256];
        if (!r->depth)
            for (int b = 0; b < 256; ++b) pair4[b] = code[b & 15] | code[b >> 4] << 8;
        for (int row = 0; row < r->h; ++row) {
            const u16 *src = psx_vram + ((r->y + row) & 511) * 1024;
            u16 *out = (u16 *)(dst + row * row_bytes);
            if (r->depth) {
                for (int k = 0; k < r->wh; ++k) { u16 w = src[(r->x + k) & 1023]; out[k] = code[w & 255] | code[w >> 8] << 8; }
            } else {
                for (int k = 0; k < r->wh; ++k) {
                    u16 w = src[(r->x + k) & 1023];
                    out[2 * k] = pair4[w & 255];
                    out[2 * k + 1] = pair4[w >> 8];
                }
            }
        }
        return;
    }
    /* 15-bit: each normalised colour's representative, found once per 12-bit key */
    u8 *nearest = (u8 *)conv_scratch;
    memset(nearest, 0xFF, 4096);
    for (int row = 0; row < r->h; ++row) {
        const u16 *src = psx_vram + ((r->y + row) & 511) * 1024;
        u16 *out = (u16 *)(dst + row * row_bytes);
        u8 pair[2];
        for (int k = 0; k < r->wh; ++k) {
            u16 c = src[(r->x + k) & 1023];
            int m;
            u16 n = normalised(c, &m);
            unsigned key = (n >> 1 & 15) | (n >> 6 & 15) << 4 | (n >> 11 & 15) << 8;
            if (nearest[key] == 0xFF) {
                int best = 0, best_d = 1 << 30;
                for (int q = 0; q < nreps; ++q) {
                    int e = colour_distance(n, reps[q]);
                    if (e < best_d) { best_d = e; best = q; }
                }
                nearest[key] = (u8)best;
            }
            pair[k & 1] = (u8)(texel_alpha(c, m) << 3 | nearest[key]);
            if (k & 1) out[k / 2] = pair[0] | pair[1] << 8;
        }
        if (r->wh & 1) out[r->wh / 2] = pair[0];
    }
    memset(conv_scratch, 0, sizeof conv_scratch);
}

/* Copy dirty regions and palettes from PS1 VRAM. Banks A-D, F and G must be mapped to the CPU (LCDC). */
int gxh_stats_upload_bytes, gxh_stats_upload_regions, gxh_stats_upload_palettes; /* the last scene's */

static __attribute__((noinline)) void upload(void) {
    u8 *base = (u8 *)VRAM_A;
    gxh_stats_upload_bytes = gxh_stats_upload_regions = gxh_stats_upload_palettes = 0;
    if (!ramp_written) {
        u16 *ramp = (u16 *)(base + RAMP_UNIT * TEX_UNIT);
        for (int row = 0; row < 8; ++row)
            for (int u = 0; u < 32; u += 2) ramp[row * 16 + u / 2] = (u16)(u << 3 | (u + 1) << 11);
        u16 *pal = (u16 *)VRAM_F + ADD_SLOT * 256 + RAMP_PALETTE * 8;
        for (int k = 0; k < 8; ++k) pal[k] = 0x7FFF;
        ramp_written = 1;
    }
    for (int i = 0; i < MAX_REGIONS; ++i) {
        Region *r = &regions[i];
        if (!r->used || !r->dirty) continue;
        r->dirty = 0;
        gxh_stats_upload_regions++;
        gxh_stats_upload_bytes += r->h * r->wh * 2;
        int row_bytes = r->tw * (r->conv ? 8 : r->depth == 0 ? 4 : r->depth == 1 ? 8 : 16) / 8;
        u8 *dst = base + r->unit * TEX_UNIT;
        if (r->conv) { conv_upload(r, dst, row_bytes); continue; }
        for (int row = 0; row < r->h; ++row) {
            const u16 *src = psx_vram + ((r->y + row) & 511) * 1024;
            u16 *out = (u16 *)(dst + row * row_bytes);
            if (r->depth < 2 && r->x + r->wh <= 1024) {
                memcpy(out, src + r->x, r->wh * 2);
            } else if (r->depth == 2 && !(r->x & 1) && !(r->wh & 1) && r->x + r->wh <= 1024) {
                /* two texels a word: bit 15 set on each nonzero one (adding 0x7FFF to its low 15 bits carries
                 * into bit 15 exactly when they aren't all zero) */
                const u32 *in = (const u32 *)(src + r->x);
                u32 *out2 = (u32 *)out;
                for (int k = 0; k < r->wh / 2; ++k) {
                    u32 w = in[k];
                    out2[k] = w | ((((w & 0x7FFF7FFFu) + 0x7FFF7FFFu) | w) & 0x80008000u);
                }
            } else {
                for (int k = 0; k < r->wh; ++k) {
                    u16 c = src[(r->x + k) & 1023];
                    /* 15-bit: the DS shows texels with bit 15 set; the PS1 hides only 0x0000 */
                    out[k] = r->depth == 2 ? (c ? c | 0x8000 : 0) : c;
                }
            }
        }
    }
    run_code_cpal = -1;
    if (runs)
        for (int i = 0; i < MAX_RUNS; ++i) {
            Run *r = &runs[i];
            if (!(r->flags & RUN_DIRTY)) continue;
            r->flags &= (u8)~RUN_DIRTY;
            gxh_stats_upload_regions++;
            gxh_stats_upload_bytes += r->n * 16 * 16 * 2 / texels_per_halfword(KEY_DEPTH(r->key));
            upload_run(r, base);
        }
    u16 *pal = (u16 *)VRAM_F;
    for (int i = 0; i < PAL_SLOTS; ++i) {
        Palette *p = &palettes[i];
        if (!p->used || !p->dirty) continue;
        p->dirty = 0;
        gxh_stats_upload_palettes++;
        const u16 *src = psx_vram + (p->y & 511) * 1024;
        if (p->scale == 16) {
            for (int k = 0; k < p->count; ++k) pal[i * 256 + k] = src[(p->x + k) & 1023];
        } else {
            for (int k = 0; k < p->count; ++k) {
                u32 c = src[(p->x + k) & 1023], r = (c & 31) * p->scale >> 4, g = (c >> 5 & 31) * p->scale >> 4,
                    bl = (c >> 10 & 31) * p->scale >> 4;
                pal[i * 256 + k] = (u16)((r > 31 ? 31 : r) | (g > 31 ? 31 : g) << 5 | (bl > 31 ? 31 : bl) << 10 | (c & 0x8000));
            }
        }
    }
}

/* ---- command emission -------------------------------------------------------------------------------------- */

/* TEXIMAGE's colour-0-transparent bit for a palette slot: the PS1 hides texels whose CLUT colour is 0x0000, the DS
 * index 0 of a palette, so the bit is set only when the CLUT's first colour is 0x0000 (FF9's usual layout; the world
 * map's sea CLUT has a colour there, and its texels showed as holes). A 0x0000 elsewhere in a CLUT stays opaque. */
static inline u32 index0_transparent(int pal) { return (u32)palettes[pal].index0; }

enum { CMD_COLOR = 0x20, CMD_TEXCOORD = 0x22, CMD_VTX16 = 0x23, CMD_VTXXY = 0x25, CMD_POLYATTR = 0x29,
       CMD_TEXIMAGE = 0x2A, CMD_PLTTBASE = 0x2B, CMD_BEGIN = 0x40 };
#define INITIAL_ATTR (3u << 6 | 31u << 16)

/* The scene's commands (packed FIFO format, one command per header), fed to the geometry FIFO by DMA while the
 * CPU goes on. Worst case per polygon: 4 state commands and 4 vertices of colour, texcoord and position. */
#define CHUNK_WORDS 512    /* two 2 KB halves: one fills while the other goes to the FIFO */
static u32 (*chunks)[CHUNK_WORDS]; /* two, in main RAM: the CPU writes VRAM (bank I before) more slowly */
static int chunk, chunk_words;

static void flush_chunk(void) {
    if (!chunk_words) return;
    DC_FlushRange(chunks[chunk], chunk_words * 4);
    while (dmaBusy(0)) {}
    while (dmaBusy(1) || dmaBusy(2) || dmaBusy(3)) {} /* libnds: no concurrent DMA when one feeds the FIFO */
    dmaSetParams(0, chunks[chunk], (void *)&GFX_FIFO, DMA_FIFO | (u32)chunk_words);
    chunk ^= 1;
    chunk_words = 0;
}

/* Room for one primitive's words (at most 3 state commands, a begin and 4 vertices of colour, texcoord and
 * position: 36), made before each, so put() needn't check. */
#define PRIM_WORDS 40
/* emit_scene writes through a local pointer (wp; with the port's -fno-strict-aliasing, the globals would be
 * reloaded at every word); reserve() hands its count back for a flush. */
#define reserve() do { \
        if (wp - chunks[chunk] > CHUNK_WORDS - PRIM_WORDS) { chunk_words = wp - chunks[chunk]; flush_chunk(); wp = chunks[chunk]; } \
    } while (0)
#define put(word) (*wp++ = (word))
#define fifo(cmd, param) (put(cmd), put(param))
/* Packed commands: a header word of up to four command IDs, then their parameters in order. */
#define pack_begin() (pack_header = wp++, pack_ids = 0, pack_shift = 0)
#define pack(cmd) (pack_ids |= (u32)(cmd) << pack_shift, pack_shift += 8)
#define pack_end() (*pack_header = pack_ids)

/* emit_scene's drawing state, for the field models' polygons (fm_emit, in ovl_0a's native area) */
typedef struct { u32 *wp; u32 attr, image, palette; int polys, vertices; } EmitState;
static void fm_emit(const Buffer *b, const FStore *fst, EmitState *es);

/* c * 31 / d for c, d <= 255, d > 0, by a reciprocal (ceil(2^24 / d); exact over that range) */
static u32 recip24[256];
static inline u32 div31(u32 c, u32 d) {
    if (!recip24[1]) for (u32 k = 1; k < 256; ++k) recip24[k] = (u32)(((1ull << 24) + k - 1) / k);
    return (u32)(((unsigned long long)(c * 31) * recip24[d]) >> 24);
}
/* vertex i's colour at full brightness: each channel x 31 / m (m its brightest channel or more) */
static __attribute__((noinline)) u32 hue_of(const Prim *p, int i, u32 m) {
    return div31(p->r[i], m) | div31(p->g[i], m) << 5 | div31(p->b[i], m) << 10;
}

/* additive: the polygon's brightest vertex channel, which went into its alpha (conv_upload); the vertex colours
 * keep the hue and the shading relative to it. */
static inline u32 rgb_for(const Prim *p, int i, int additive) {
    int r, g, b;
    if (additive && p->abr == 2) {
        r = g = b = 0; /* subtraction: black over the destination at the source's brightness */
    } else if (additive) {
        if (p->flags & P_RAW) return 0x7FFF;
        return whiten_by(hue_of(p, i, (u32)additive), add_white);
    } else if ((p->flags & P_SEMI) && p->abr == 2) {
        r = g = b = 0;
    } else if ((p->flags & (P_TEXTURED | P_RAW)) == (P_TEXTURED | P_RAW)) {
        r = g = b = 31;
    } else if (p->flags & P_TEXTURED) { /* PS1: 128 = x1.0; DS: 31 = x1.0 (brighter clamps) */
        r = p->r[i] >> 2; g = p->g[i] >> 2; b = p->b[i] >> 2;
        r = r > 31 ? 31 : r; g = g > 31 ? 31 : g; b = b > 31 ? 31 : b;
    } else {
        r = p->r[i] >> 3; g = p->g[i] >> 3; b = p->b[i] >> 3;
    }
    return (u32)(r | g << 5 | b << 10);
}

/* Twice a primitive's screen area. */
static int prim_area2(const Prim *p) {
    int a = (p->x[1] - p->x[0]) * (p->y[2] - p->y[0]) - (p->y[1] - p->y[0]) * (p->x[2] - p->x[0]);
    if (a < 0) a = -a;
    if (p->n == 4) {
        int c = (p->x[2] - p->x[1]) * (p->y[3] - p->y[1]) - (p->y[2] - p->y[1]) * (p->x[3] - p->x[1]);
        a += c < 0 ? -c : c;
    }
    return a;
}

/* A scene over the 3D engine's budget (2048 polygons, 6144 vertices: Ark and Taharka run 4-6% over) drops small
 * primitives from the start of the drawing order: in painter's order that is the far scene, whose small pieces are
 * the least visible and often covered later. Size alone would take the nearest characters, whose models are many
 * tiny triangles drawn last. The size bound grows only while the budget isn't met. */
static u8 *prim_drop;
static void plan_drops(Buffer *b) {
    int vertices = 0, prims = 0;
    for (int k = 0; k < b->count; ++k) if (in_scene(&b->prims[k], k)) vertices += b->prims[k].n, ++prims;
    if (b->meshes && b->mesh_gen == b->scene_gen) /* the native meshes' polygons */
        for (int m = 0; m < b->meshes; ++m) {
            const MeshInst *mi = mesh_at(b, m);
            prims += mi->polys;
            vertices += mi->vertices;
        }

    memset(prim_drop, 0, b->count);
    int over_p = prims - MAX_POLYS, over_v = vertices - MAX_VERTICES;
    int meshes = b->meshes && b->mesh_gen == b->scene_gen;
    b->mesh_drop = 0; b->mesh_quota = 0; b->mesh_quota_bound = 0;
    for (int t = 0; t < DROP_TIERS && (over_p > 0 || over_v > 0); ++t) {
        for (int k = 0; k < b->count && (over_p > 0 || over_v > 0); ++k)
            if (!prim_drop[k] && in_scene(&b->prims[k], k) && prim_area2(&b->prims[k]) < drop_bounds[t]) {
                prim_drop[k] = 1;
                --over_p;
                over_v -= b->prims[k].n;
            }
        if (meshes && (over_p > 0 || over_v > 0)) { /* then the meshes' polygons of that size, as many as it takes */
            int tp = 0, tv = 0;
            for (int m = 0; m < b->meshes; ++m) { tp += mesh_at(b, m)->tier_polys[t]; tv += mesh_at(b, m)->tier_vertices[t]; }
            int need = over_p, by_v = tp ? (over_v * tp + tv - 1) / tv : 0;
            if (by_v > need) need = by_v;
            if (need >= tp) { /* the whole tier */
                b->mesh_drop = drop_bounds[t];
                over_p -= tp; over_v -= tv;
            } else if (need > 0) {
                b->mesh_quota_bound = drop_bounds[t]; b->mesh_quota = need;
                over_p -= need; over_v -= tp ? need * tv / tp : 0;
            }
        }
    }
}

/* Translucent polygons with one ID don't blend over each other. Each gets its own, so overlapping layers add up,
 * except that one sharing an edge with a recent one, on the other side of that edge, takes that one's ID: the DS
 * draws a shared edge's pixels for both polygons, which would blend twice and leave a bright seam through a mesh
 * (whose polygons the ordering table interleaves with others). Polygons on the same side of their shared edge
 * overlap (a closed mesh's back faces folding behind its front ones at the silhouette) and keep their own IDs, so
 * both layers blend as on the PS1. */
#define RECENT 16
static const Prim *recent[RECENT];
static u32 recent_xy[RECENT][4]; /* their vertices packed (x | y << 16), for the cheap shared-vertex test */
static u32 recent_sig[RECENT];   /* a bit per vertex (a hash of its position): no common bit, no shared vertex */
static u8 recent_id[RECENT];
static int recent_at;

static inline int edge_side(const Prim *p, int a, int b, int x, int y) {
    long c = (long)(p->x[b] - p->x[a]) * (y - p->y[a]) - (long)(p->y[b] - p->y[a]) * (x - p->x[a]);
    return c > 0 ? 1 : c < 0 ? -1 : 0;
}

/* p and q share an edge with p's other vertices on one side of it and q's on the other */
static int neighbours(const Prim *p, const Prim *q) {
    int pa = -1, pb = -1, qa = -1, qb = -1;
    for (int i = 0; i < p->n; ++i)
        for (int j = 0; j < q->n; ++j)
            if (p->x[i] == q->x[j] && p->y[i] == q->y[j]) {
                if (pa < 0) { pa = i; qa = j; }
                else if (pb < 0 && i != pa) { pb = i; qb = j; }
            }
    if (pb < 0) return 0;
    int ps = 0, qs = 0;
    for (int i = 0; i < p->n; ++i) if (i != pa && i != pb) ps += edge_side(p, pa, pb, p->x[i], p->y[i]);
    for (int j = 0; j < q->n; ++j) if (j != qa && j != qb) qs += edge_side(p, pa, pb, q->x[j], q->y[j]);
    (void)qb;
    return (ps > 0 && qs < 0) || (ps < 0 && qs > 0);
}

static int translucent_id(const Prim *p, int polys) {
    int id = 1 + (polys & 31);
    u32 xy[4], sig = 0;
    for (int i = 0; i < p->n; ++i) {
        xy[i] = (u16)p->x[i] | (u32)(u16)p->y[i] << 16;
        sig |= 1u << ((xy[i] * 0x9E3779B1u) >> 27);
    }
    for (int k = 0; k < RECENT; ++k) {
        const Prim *q = recent[k];
        if (!q || !(sig & recent_sig[k])) continue;
        int shared = 0; /* two vertices in common before the side test */
        for (int i = 0; i < p->n; ++i)
            for (int j = 0; j < q->n; ++j) shared += xy[i] == recent_xy[k][j];
        if (shared >= 2 && neighbours(p, q)) { id = recent_id[k]; break; }
    }
    recent[recent_at] = p; recent_id[recent_at] = (u8)id; recent_sig[recent_at] = sig;
    for (int i = 0; i < p->n; ++i) recent_xy[recent_at][i] = xy[i];
    recent_at = (recent_at + 1) % RECENT;
    return id;
}

/* The native meshes' polygons one at a time, as GP0 would have recorded their packets (btl_drawMesh), with
 * each vertex's depth (emit_scene). Faces the PS1 culls (NCLIP <= 0 unless double-sided) and zero-area ones
 * (hybrid_poly) are skipped. */

static inline int sext11i(u32 v) { return (int)(v & 0x7FF) - (int)((v & 0x400) << 1); }

/* How many primitives the walk had recorded when it reached main-OT entry e (note_walk). */
static inline int entry_rank(const Buffer *b, int e) {
    int q = e >> RANK_SHIFT;
    return q >= b->last_bucket ? b->ranks[q] : b->count;
}

/* plan_drops' verdict on a native polygon of size tier `tier` */
static inline int mesh_dropped(Buffer *b, int tier) {
    if (drop_bounds[tier] <= b->mesh_drop) return 1;
    if (b->mesh_quota && drop_bounds[tier] <= b->mesh_quota_bound) { b->mesh_quota--; return 1; }
    return 0;
}

/* A background record's next polygon (1), one it skips (0) or its end (-1): btlbg_drawRecord's POLY_FT4/FT3,
 * colours scaled by the brightness as its GPF does. */
static void decode_bg(const Buffer *b, const MeshInst *mi, int k, Prim *p, u16 *zv, u8 *group, u8 *palette) {
    int nquad = mi->count[0];
    const u8 *rec = mi->src, *idx = rec + MH(rec, 8), *uvs = rec + MH(rec, 0xA);
    int quad = k < nquad, n = quad ? 4 : 3;
    const u8 *ix = quad ? idx + k * 8 : idx + nquad * 8 + (k - nquad) * 6;
    const u8 *uw = quad ? uvs + k * 8 : uvs + nquad * 8 + (k - nquad) * 6;
    const MeshUse *u = &mi->uses[(mi->info[k] >> 3) & 15];
    u32 c = mi->colors[k];
    p->n = (u8)n;
    p->flags = (u8)(P_TEXTURED | (c & 0x20000000 ? P_SEMI : 0));
    use_page(u, p);
    *group = u->group; *palette = u->palette;
    u32 r = c & 0xFF, g = (c >> 8) & 0xFF, bl = (c >> 16) & 0xFF;
    if (mi->bright != 0x80) {
        u32 br = mi->bright;
        r = r * br > 0x7FFF ? 0x7FFF >> 7 : r * br >> 7;
        g = g * br > 0x7FFF ? 0x7FFF >> 7 : g * br >> 7;
        bl = bl * br > 0x7FFF ? 0x7FFF >> 7 : bl * br >> 7;
    }
    u16 z = mi->z[k];
    for (int i = 0; i < n; ++i) {
        u32 xy = mi->xy[MH(ix, 2 * i) / 4];
        p->x[i] = (s16)(sext11i(xy) + b->ofs_x);
        p->y[i] = (s16)(sext11i(xy >> 16) + b->ofs_y);
        p->r[i] = (u8)r; p->g[i] = (u8)g; p->b[i] = (u8)bl;
        u16 w = MH(uw, 2 * i);
        p->u[i] = (u8)w; p->v[i] = (u8)(w >> 8);
        zv[i] = z;
    }
}

/* A model's polygon of type t at s (its info byte inf) as GP0 would have recorded its packet. */
static void decode_model(const Buffer *b, const MeshInst *mi, int t, const u8 *s, u8 inf, Prim *p, u16 *zv, u8 *group,
                         u8 *palette) {
    int quad = !(t & 1), n = quad ? 4 : 3;
    u32 i[4];
    i[0] = MW(s, 0) & 0xFFFF; i[1] = MW(s, 0) >> 16;
    if (quad) { i[2] = MW(s, 4) & 0xFFFF; i[3] = MW(s, 4) >> 16; } else i[2] = MH(s, 4);
    u8 f = MB(s, mesh_flags_at[t]);
    p->n = (u8)n;
    u32 col[4];
    *group = 0xFF; *palette = 0xFF;
    if (t < 4) { /* textured */
        const MeshUse *u = &mi->uses[(inf >> 3) & 15];
        use_page(u, p);
        *group = u->group; *palette = u->palette;
        p->flags = (u8)(P_TEXTURED | (f & 1 ? P_SEMI : 0));
        u16 uv[4];
        mesh_uvs(mi, t, s, uv);
        for (int v = 0; v < n; ++v) { p->u[v] = (u8)uv[v]; p->v[v] = (u8)(uv[v] >> 8); }
        if (t < 2) {
            col[0] = col[1] = col[2] = col[3] = MW(s, t == 0 ? 0x10 : 8);
        } else {
            p->flags |= P_GOURAUD;
            col[0] = mi->colors[MW(s, 0x10) & 0xFFFF]; col[1] = mi->colors[MW(s, 0x10) >> 16];
            if (t == 2) { col[2] = mi->colors[MW(s, 0x14) & 0xFFFF]; col[3] = mi->colors[MW(s, 0x14) >> 16]; }
            else col[2] = mi->colors[MH(s, 0x14)];
        }
    } else {
        p->tx = p->ty = p->clut_x = p->clut_y = 0; p->depth = 0;
        p->abr = mi->untex_abr;
        if (t < 6) {
            p->flags = P_GOURAUD;
            col[0] = mi->colors[MW(s, 8) & 0xFFFF]; col[1] = mi->colors[MW(s, 8) >> 16];
            if (t == 4) { col[2] = mi->colors[MW(s, 0xC) & 0xFFFF]; col[3] = mi->colors[MW(s, 0xC) >> 16]; }
            else col[2] = mi->colors[MH(s, 0xC)];
        } else {
            u32 w = MW(s, 8); /* its top byte is ored into the command: the semi-transparency bit */
            p->flags = w & 0x02000000 ? P_SEMI : 0;
            col[0] = col[1] = col[2] = col[3] = w;
        }
        for (int v = 0; v < n; ++v) p->u[v] = p->v[v] = 0;
    }
    for (int v = 0; v < n; ++v) {
        u32 xy = mi->xy[i[v]];
        p->x[v] = (s16)(sext11i(xy) + b->ofs_x);
        p->y[v] = (s16)(sext11i(xy >> 16) + b->ofs_y);
        p->r[v] = (u8)col[v]; p->g[v] = (u8)(col[v] >> 8); p->b[v] = (u8)(col[v] >> 16);
        zv[v] = mi->z[i[v]];
    }
}


/* The native polygons in the PS1's drawing order: each by the main-OT entry its packet would have linked into, far
 * to near, and among one entry's the most recently linked first (an entry's list starts at its newest packet). They
 * go out among the scene's primitives where the walk had them (before the primitive recorded when the walk reached
 * their entry), each at a depth above the last: the 3D engine draws them in the PS1's order, opaque or not, the depth
 * test ordering them as the OT did. (Per-vertex depths had sorted their pixels instead: a wing tip behind a wall
 * where the PS1 drew the whole wing over it.) The references sit in the buffer's pool after the snapshots. */
typedef struct { u8 m, pad; u16 pi, next; } NativeRef; /* instance, polygon, the next in its entry's list */
#define NO_REF 0xFFFF
static u16 *native_heads; /* per main-OT entry: its newest native polygon (NO_REF: none), in ovl_0b's dead code */
static NativeRef *native_refs;

/* a model's polygon pi: its type and source */
static const u8 *model_poly(const MeshInst *mi, int pi, int *type) {
    const u8 *s = mi->src;
    for (int t = 0; t < 8; ++t) {
        if (pi < mi->count[t]) { *type = t; return s + pi * mesh_size[t]; }
        pi -= mi->count[t];
        s += mi->count[t] * mesh_size[t];
    }
    *type = 0;
    return NULL;
}

/* The scene's native polygons to draw, in a list per main-OT entry as the PS1 links them (returns their count; ones
 * the pool has no room for are dropped). */
static int collect_native(Buffer *b) {
    unsigned at = (b->pool_used + 3) & ~3u;
    native_refs = (NativeRef *)(b->pool + at);
    int cap = at < POOL_BYTES ? (int)((POOL_BYTES - at) / sizeof(NativeRef)) : 0, n = 0;
    if (cap > NO_REF) cap = NO_REF;
    memset(native_heads, 0xFF, 0x1010 * sizeof *native_heads);
    for (int m = 0; m < b->meshes; ++m) {
        const MeshInst *mi = mesh_at(b, m);
        int np = mi->kind == MESH_BG ? mi->count[0] + mi->count[1] : 0;
        if (mi->kind != MESH_BG) for (int t = 0; t < 8; ++t) np += mi->count[t];
        const u8 *s = mi->src;
        for (int t = 0, k = 0, pi = 0; pi < np; ++pi) {
            u8 inf = mi->info[pi];
            int e;
            if (mi->kind == MESH_BG) {
                if (inf == 0xFF) continue;
                e = mi->z[pi];
            } else {
                while (k >= mi->count[t]) { s += mi->count[t] * mesh_size[t]; ++t; k = 0; }
                const u8 *q = s + k++ * mesh_size[t];
                if (inf == 0xFF) continue;
                /* the OT entry its packet would have: the depth sum >> 6 (quads), AVSZ3's OTZ / 4 (triangles) */
                const u16 *sz = mi->z;
                u32 zs = sz[MW(q, 0) & 0xFFFF] + sz[MW(q, 0) >> 16];
                if (!(t & 1)) e = (int)((zs + sz[MW(q, 4) & 0xFFFF] + sz[MW(q, 4) >> 16]) >> 6);
                else e = (int)(((s16)psx_gte.c[29] * (int)(zs + sz[MH(q, 4)]) >> 12) >> 2);
                e += 8 - mi->bias;
                if (e < 0) e = 0; else if (e > 0x100F) e = 0x100F;
            }
            if (n == cap) { gxh_stats_dropped++; continue; }
            native_refs[n] = (NativeRef){(u8)m, 0, (u16)pi, native_heads[e]}; /* (an entry's list starts at its newest) */
            native_heads[e] = (u16)n++;
        }
    }
    return n;
}


/* a collected native polygon (0: plan_drops dropped it) */
static int native_poly(Buffer *b, const NativeRef *r, Prim *p, u8 *group, u8 *palette) {
    const MeshInst *mi = mesh_at(b, r->m);
    u8 inf = mi->info[r->pi];
    if (mesh_dropped(b, inf & 7)) { gxh_stats_dropped++; return 0; }
    u16 zv[4];
    if (mi->kind == MESH_BG) {
        decode_bg(b, mi, r->pi, p, zv, group, palette);
    } else {
        int t;
        const u8 *s = model_poly(mi, r->pi, &t);
        decode_model(b, mi, t, s, inf, p, zv, group, palette);
    }
    return 1;
}

/* an untextured additive glow's alpha by mode and brightness: m * 31 / 255 (subtraction twice that, mode 3 a quarter),
 * at most 31 */
static u8 glow_alpha[4][256];

static __attribute__((noinline)) void emit_scene(Buffer *b) {
    if (!glow_alpha[1][255])
        for (int abr = 0; abr < 4; ++abr)
            for (int m = 0; m < 256; ++m) {
                int a = m * 31 * (abr == 2 ? 2 : 1) / (abr == 3 ? 1020 : 255);
                glow_alpha[abr][m] = (u8)(a > 31 ? 31 : a);
            }
    memset(recent, 0, sizeof recent);
    for (int i = 0; i < PAL_SLOTS; ++i) /* (index0_transparent) */
        if (palettes[i].used) palettes[i].index0 = psx_vram[(palettes[i].y & 511) * 1024 + (palettes[i].x & 1023)] ? 0 : 1 << 29;
    u32 attr = INITIAL_ATTR, image = 0, palette = ~0u;
    u32 *wp = chunks[chunk] + chunk_words;
    u32 *pack_header, pack_ids;
    int pack_shift;
    reserve();
    fifo(CMD_POLYATTR, attr);
    fifo(CMD_TEXIMAGE, 0);
    int polys = 0, vertices = 0, z = 0;
    if (b->slots && place->world) { /* the world map: its clear colour as a quad behind everything (the 3D engine's own clear, black,
                      * covers the letterbox rows outside the viewport, black on the PS1 too) */
        pack_begin();
        pack(CMD_COLOR); put(b->clear & 0x7FFF);
        pack(CMD_BEGIN); put(1);
        pack_end();
        static const s16 corner[4][2] = {{0, 0}, {320, 0}, {320, 256}, {0, 256}};
        for (int k = 0; k < 4; ++k) {
            pack_begin();
            pack(CMD_VTX16); put((u32)(u16)corner[k][0] | (u32)(u16)corner[k][1] << 16); put((u32)(u16)-32000);
            pack_end();
        }
        polys++;
        vertices += 4;
    }
    plan_drops(b);
    memset(light_cells, 0, sizeof light_cells);
    FStore *fst = scene_models(b);
    if (fst) { /* the field models first (depth tested: any order; their polygons before the rest's budget) */
        EmitState es = {wp, attr, image, palette, polys, vertices};
        fm_emit(b, fst, &es);
        wp = es.wp; attr = es.attr; image = es.image; palette = es.palette; polys = es.polys; vertices = es.vertices;
    }
    /* the recorded primitives at depth 2 (k + 1) (k + 1 without meshes), then the native meshes' polygons, each
     * vertex at 2 rank + 1: between the primitives recorded before and after the walk reached its OT entry */
    int with_meshes = b->meshes && b->mesh_gen == b->scene_gen;
    static PrimCopy mesh_ring[RECENT]; /* translucent_id keeps pointers to the last RECENT */
    int ring_at = 0, zc = 0, ne = -1, ri = NO_REF, erank = 0;
    if (with_meshes && collect_native(b)) ne = 0x100F;
    for (int k = 0;;) {
        const Prim *src;
        int native = 0;
        u8 grp, pal;
        /* a copy the command words can't alias (with -fno-strict-aliasing every store through wp would reload the
         * primitive's fields); translucent_id keeps pointers to the buffer's */
        PrimCopy local;
        while (ri == NO_REF && ne >= 0) { /* the next entry, far to near, with native polygons */
            ri = native_heads[ne];
            erank = ri == NO_REF ? 0 : entry_rank(b, ne);
            ne--;
        }
        if (ri != NO_REF && (k >= b->count || erank <= k)) {
            const NativeRef *r = &native_refs[ri];
            ri = r->next;
            if (!native_poly(b, r, &local.p, &grp, &pal)) continue; /* a native polygon, in OT order */
            native = 1;
            z = Z_STEP * ++zc;
        } else if (k < (b->slots ? 2 * b->count : b->count)) {
            int kk = k++;
            if (b->slots) {
                /* The world map: the opaque primitives first, from the last, at their OT entries' depths (the depth
                 * test keeps the first of equal depths: the PS1's topmost, however many share an entry), then the
                 * translucent ones in the walk's order, just in front of their entries (the 3D engine draws them
                 * after the opaque ones, without writing depth). */
                int opaque_pass = kk < b->count;
                kk = opaque_pass ? b->count - 1 - kk : kk - b->count;
                if (opaque_pass == !!(b->prims[kk].flags & P_SEMI)) continue;
            }
            src = &b->prims[kk];
            if (!in_scene(src, kk)) continue;
            if (prim_drop[kk]) { gxh_stats_dropped++; continue; }
            reserve();
            if (polys >= MAX_POLYS || vertices + src->n > MAX_VERTICES) { gxh_stats_dropped++; continue; }
            /* (a field's buffers hold more than 2047; the world map's at their OT entries' depths) */
            z = b->slots ? b->slots[kk] + (src->flags & P_SEMI ? 2 : 0) : with_meshes ? Z_STEP * ++zc : kk + 1;
            grp = prim_group[kk]; pal = prim_palette[kk];
            if ((src->flags & (P_RECT | P_TEXTURED | P_SEMI | P_GOURAUD | P_UI | P_TWIN)) == (P_RECT | P_TEXTURED) && src->n == 4) {
                /* An opaque textured sprite (a field's background tiles): the general path's words, without its
                 * blending cases. Its fields are read first: stores through wp would reload them. */
                int depth = src->depth, u0 = src->u[0], v0 = src->v[0], tu, tv;
                u32 unit, size_bits;
                if (grp == RUN_GROUP) {
                    const Run *r = run_holding(STRIP_KEY(depth, 0, src->tx, src->ty, src->clut_x, src->clut_y, v0 >> 4),
                                               u0 >> 4, src->u[1] >> 4);
                    if (!r) { gxh_stats_texture_fail++; continue; }
                    unit = rows[r->row].unit; size_bits = RUN_SIZE_BITS;
                    tu = (r->bu - r->slot) * 16;
                    tv = (v0 >> 4) * 16;
                } else {
                    const Region *r = grp == 0xFF ? NULL : groups[grp].region;
                    if (!r) { gxh_stats_texture_fail++; continue; }
                    if (r->conv) goto general;
                    unit = r->unit; size_bits = r->size_bits;
                    tu = (r->x - src->tx) * texels_per_halfword(depth);
                    tv = r->y - src->ty;
                }
                u32 want_image = (u32)(unit * TEX_UNIT / 8) | size_bits | (u32)(depth == 2 ? 7 : depth == 0 ? 3 : 4) << 26;
                u32 set_palette = ~0u;
                if (depth < 2) {
                    if (pal == 0xFF) { gxh_stats_palette_fail++; continue; }
                    want_image |= index0_transparent(pal);
                    u32 want_palette = (u32)pal * 512 / 16;
                    if (want_palette != palette) set_palette = want_palette;
                }
                u32 colour;
                if (src->flags & P_RAW) colour = 0x7FFF;
                else {
                    int r = src->r[0] >> 2, g = src->g[0] >> 2, bl = src->b[0] >> 2;
                    colour = (u32)((r > 31 ? 31 : r) | (g > 31 ? 31 : g) << 5 | (bl > 31 ? 31 : bl) << 10);
                }
                u32 xy[4], uv[4];
                static const u8 order[4] = {0, 1, 3, 2}; /* PS1 quad 0,1,2 + 1,2,3 is DS quad 0,1,3,2 */
                for (int j = 0; j < 4; ++j) {
                    int i = order[j];
                    xy[j] = (u32)(u16)src->x[i] | (u32)(u16)src->y[i] << 16;
                    uv[j] = (u32)(u16)((src->u[i] - tu + (i & 1)) * 16) | (u32)(u16)((src->v[i] - tv + (i >> 1)) * 16) << 16;
                }
                pack_begin();
                if (set_palette != ~0u) { pack(CMD_PLTTBASE); put(set_palette); palette = set_palette; }
                if (want_image != image) { pack(CMD_TEXIMAGE); put(want_image); image = want_image; }
                if (attr != INITIAL_ATTR) { pack(CMD_POLYATTR); put(INITIAL_ATTR); attr = INITIAL_ATTR; }
                pack(CMD_BEGIN); put(1);
                pack_end();
                put(CMD_COLOR | CMD_TEXCOORD << 8 | CMD_VTX16 << 16);
                put(colour); put(uv[0]); put(xy[0]); put((u32)(u16)z);
                for (int j = 1; j < 4; ++j) { put(CMD_TEXCOORD | CMD_VTXXY << 8); put(uv[j]); put(xy[j]); }
                ++polys;
                vertices += 4;
                continue;
            }
        general:
            copy_prim(&local, src);
        } else {
            break;
        }
        if (native) {
            reserve();
            if (polys >= MAX_POLYS || vertices + local.p.n > MAX_VERTICES) { gxh_stats_dropped++; continue; }
            src = &local.p;
            if (local.p.flags & P_SEMI) { /* (only translucent ones reach translucent_id, which keeps the pointer) */
                PrimCopy *keep = &mesh_ring[ring_at];
                ring_at = (ring_at + 1) % RECENT;
                copy_prim(keep, &local.p);
                src = &keep->p;
            }
        }
        const Prim *p = &local.p;
        int tu = 0, tv = 0, additive = 0;
        u32 want_image = 0, set_palette = ~0u; /* state to send with the primitive's BEGIN (~0: unchanged) */
        u32 tex_hue = 0x7FFF; /* an additive texture's colour, for light_under */
        if (p->flags & P_TEXTURED) {
            int conv, cpal, unit;
            u32 size_bits;
            if (grp == RUN_GROUP) { /* a sprite: u[0], v[0] are its first texels */
                int c = (p->flags & P_SEMI) && p->abr != 0;
                const Run *r = run_holding(STRIP_KEY(p->depth, c, p->tx, p->ty, p->clut_x, p->clut_y, p->v[0] >> 4),
                                           p->u[0] >> 4, p->u[1] >> 4);
                if (!r) { gxh_stats_texture_fail++; continue; }
                conv = c; cpal = r->cpal; unit = rows[r->row].unit; size_bits = RUN_SIZE_BITS;
                tu = (r->bu - r->slot) * 16;
                tv = (p->v[0] >> 4) * 16;
            } else {
                const Region *r = grp == 0xFF ? NULL : groups[grp].region;
                if (!r) { gxh_stats_texture_fail++; continue; }
                conv = r->conv; cpal = r->cpal; unit = r->unit; size_bits = r->size_bits;
                tu = (r->x - p->tx) * texels_per_halfword(p->depth);
                tv = r->y - p->ty;
            }
            if (p->depth < 2 && !conv && pal == 0xFF) { gxh_stats_palette_fail++; continue; }
            want_image = (u32)(unit * TEX_UNIT / 8) | size_bits | (u32)(conv ? 6 : p->depth == 2 ? 7 : p->depth == 0 ? 3 : 4) << 26;
            if (conv) {
                tex_hue = cpal_hue[cpal & 63];
                u32 want_palette = (u32)(ADD_SLOT * 512 + cpal * 16) / 16;
                if (want_palette != palette) set_palette = want_palette;
                additive = 1;
            } else if (p->depth < 2) {
                want_image |= index0_transparent(pal);
                u32 want_palette = (u32)pal * 512 / 16;
                if (want_palette != palette) set_palette = want_palette;
            }
        }
        int glow = !(p->flags & P_TEXTURED) && (p->flags & P_SEMI) && p->abr != 0;
        if (glow) {
            want_image = (u32)(RAMP_UNIT * TEX_UNIT / 8) | 2u << 20 | 0u << 23 | 6u << 26; /* 32x8 A5I3 */
            u32 want_palette = (u32)(ADD_SLOT * 512 + RAMP_PALETTE * 16) / 16;
            if (want_palette != palette) set_palette = want_palette;
        }
        u32 set_image = ~0u;
        if (want_image != image) set_image = want_image;
        int alpha = 31, id = 0;
        if (glow) {
            id = translucent_id(src, polys);
            if (p->abr != 2) { /* its brightness: the brightest vertex channel, as the ramp's U */
                int m = 0;
                for (int j = 0; j < p->n; ++j) {
                    int v = p->r[j] > p->g[j] ? (p->r[j] > p->b[j] ? p->r[j] : p->b[j]) : (p->g[j] > p->b[j] ? p->g[j] : p->b[j]);
                    if (v > m) m = v;
                }
                u32 hue = m ? hue_of(p, 0, (u32)m) : 0;
                add_white = light_under(p, m * 31 >> (p->abr == 3 ? 9 : 7) > 31 ? 31 : m * 31 >> (p->abr == 3 ? 9 : 7), hue);
                /* a flat glow is as bright to its edges: whitening past the base would show its shape */
                if (!(p->flags & P_GOURAUD)) add_white = ADD_BASE;
            }
        } else if (additive) {
            /* the source's brightness (PS1 128 = x1; mode 3 adds a quarter), at most opaque */
            int m = 0;
            for (int j = 0; j < p->n; ++j) {
                int v = p->r[j] > p->g[j] ? (p->r[j] > p->b[j] ? p->r[j] : p->b[j]) : (p->g[j] > p->b[j] ? p->g[j] : p->b[j]);
                if (v > m) m = v;
                if (!(p->flags & P_GOURAUD)) break;
            }
            if (p->flags & P_RAW) m = 128;
            alpha = m * 31 * (p->abr == 2 ? 2 : 1) >> (p->abr == 3 ? 9 : 7); /* mode 2: see the glow ramp */
            if (alpha > 31) alpha = 31;
            if (!alpha) continue; /* adds nothing (alpha 0 would be wireframe) */
            additive = m;
            id = translucent_id(src, polys);
            if (p->abr != 2)
                add_white = light_under(p, alpha, modulate(tex_hue, (p->flags & P_RAW) ? 0x7FFF : hue_of(p, 0, (u32)m)));
        } else if (p->flags & P_SEMI) {
            alpha = p->abr == 3 ? 8 : p->abr == 1 ? 24 : 16; /* additive: a brighter blend than 50% */
            if (p->abr == 2) {
                alpha = (p->r[0] + p->g[0] + p->b[0]) * 31 / (3 * 255); /* darkening as black at source luminance */
                if (!alpha) continue; /* alpha 0 would be wireframe */
            }
            id = translucent_id(src, polys);
        }
        u32 want_attr = 3u << 6 | (u32)alpha << 16 | (u32)id << 24;
        /* the state that changed and BEGIN, in one packed group (a primitive skipped above sends nothing and leaves
         * the state as it was) */
        pack_begin();
        if (set_palette != ~0u) { pack(CMD_PLTTBASE); put(set_palette); palette = set_palette; }
        if (set_image != ~0u) { pack(CMD_TEXIMAGE); put(set_image); image = set_image; }
        if (want_attr != attr) { pack(CMD_POLYATTR); put(want_attr); attr = want_attr; }
        pack(CMD_BEGIN); put(p->n == 4 ? 1 : 0);
        pack_end();
        static const int quad_order[4] = {0, 1, 3, 2}; /* PS1 quad 0,1,2 + 1,2,3 is DS quad 0,1,3,2 */
        for (int j = 0; j < p->n; ++j) {
            int i = p->n == 4 ? quad_order[j] : j;
            pack_begin(); /* the vertex's colour, texture coordinate and position */
            if (glow) {
                /* hue at full brightness; the brightness (a quarter of it in mode 3) is the ramp's U */
                int m = p->r[i] > p->g[i] ? (p->r[i] > p->b[i] ? p->r[i] : p->b[i]) : (p->g[i] > p->b[i] ? p->g[i] : p->b[i]);
                int k = p->flags & P_GOURAUD ? i : 0;
                if (k != i) m = p->r[k] > p->g[k] ? (p->r[k] > p->b[k] ? p->r[k] : p->b[k]) : (p->g[k] > p->b[k] ? p->g[k] : p->b[k]);
                u32 colour = m && p->abr != 2 ? whiten_by(hue_of(p, k, (u32)m), add_white) : 0;
                /* subtraction (dst - s, clamped) as dst x (1 - 2s): equal at mid grey, black where the PS1 is */
                int a = glow_alpha[p->abr][m];
                pack(CMD_COLOR); put(colour);
                pack(CMD_TEXCOORD); put((u32)(u16)(a * 16 + 8) | (u32)(u16)(4 * 16) << 16);
            } else if (j == 0 || (p->flags & P_GOURAUD)) { pack(CMD_COLOR); put(rgb_for(p, i, additive)); }
            if (p->flags & P_TEXTURED) {
                int u = p->u[i] - tu, v = p->v[i] - tv;
                if (p->flags & P_RECT) { u += i & 1; v += i >> 1; } /* vertices 1, 3 right; 2, 3 bottom */
                pack(CMD_TEXCOORD); put((u32)(u16)(u * 16) | (u32)(u16)(v * 16) << 16);
            }
            if (j == 0) { /* the primitive's depth; the others keep it */
                pack(CMD_VTX16); put((u32)(u16)p->x[i] | (u32)(u16)p->y[i] << 16); put((u32)(u16)z);
            } else {
                pack(CMD_VTXXY); put((u32)(u16)p->x[i] | (u32)(u16)p->y[i] << 16);
            }
            pack_end();
        }
        ++polys;
        vertices += p->n;
    }
    struct Terrain *tr = scene_terrain(b);
    int drop_below = 0, drop_quota = 0; /* the world: its smallest faces go first when the 3D engine's RAM is short */
    if (tr) {
        int over = 0, need = 0;
        for (int t = 0; t < TERRAIN_TIERS; ++t) need += tr->tier_polys[t];
        int room_p = MAX_POLYS - polys, room_v = (MAX_VERTICES - vertices) / 3;
        over = need - (room_p < room_v ? room_p : room_v);
        for (int t = 0; t < TERRAIN_TIERS && over > 0; ++t) {
            if (tr->tier_polys[t] <= over) { drop_below = t + 1; over -= tr->tier_polys[t]; }
            else { drop_quota = over; over = 0; }
        }
        gxh_stats_dropped += need - (room_p < room_v ? room_p : room_v) > 0 ? need - (room_p < room_v ? room_p : room_v) : 0;
    }
    if (tr) /* the world map's terrain and backdrop, after the primitives (depth tested: any order) */
        for (int f = 0; f < tr->nfaces; ++f) {
            const TFace tf = tr->faces[f];
            if (tf.tier < drop_below) continue;
            if (tf.tier == drop_below && drop_quota > 0) { drop_quota--; continue; }
            const TBlock *tb = &tr->blocks[tf.ref >> 27];
            const u32 *face = tb->faces + 4 * ((tf.ref >> 15) & 0xFFF);
            int otz = (int)((tf.ref >> 2) & 0x1FFF), has_fog = (int)(tf.ref >> 1) & 1, has_lit = (int)(tf.ref & 1);
            int backdrop = tb->variant == WORLD_BACKDROP, zb = world_entry_depth(0xFA0 + 8);
            u32 w = face[0];
            /* pass 0: the lit polygon (textured; black where only fog shows), fogged by the 3D engine unless it's the
             * backdrop's; pass 1, the backdrop's: its fog, added as the ramp's alpha over the lit one (translucent,
             * one polygon ID: shared edges blend once), or opaque where the lit one is black */
            for (int pass = backdrop && has_lit ? 0 : backdrop ? 1 : 0; pass < (backdrop && has_fog ? 2 : 1); ++pass) {
                reserve();
                if (polys >= MAX_POLYS || vertices + 3 > MAX_VERTICES) break;
                u32 uvw[3] = {0, 0, 0}, want_image = 0, set_palette = ~0u, want_attr;
                int tu = 0, tv = 0, textured = pass == 0 && has_lit, ramp = pass == 1 && has_lit;
                if (textured) {
                    const TUse *u = &tr->uses[tf.use];
                    const Region *r = u->group == 0xFF ? NULL : groups[u->group].region;
                    if (!r) { gxh_stats_texture_fail++; continue; }
                    int depth = (u->tpage >> 7) & 3;
                    if (depth == 3) depth = 2;
                    if (depth < 2 && u->palette == 0xFF) { gxh_stats_palette_fail++; continue; }
                    tu = (r->x - (u->tpage & 15) * 64) * texels_per_halfword(depth);
                    tv = r->y - ((u->tpage >> 4) & 1) * 256;
                    want_image = (u32)(r->unit * TEX_UNIT / 8) | r->size_bits | (u32)(depth == 2 ? 7 : depth == 0 ? 3 : 4) << 26;
                    if (depth < 2) {
                        want_image |= index0_transparent(u->palette);
                        u32 want_palette = (u32)u->palette * 512 / 16;
                        if (want_palette != palette) set_palette = want_palette;
                    }
                    terrain_uvs(face, tb->alt, !backdrop, otz, &uvw[0], &uvw[1], &uvw[2]);
                } else if (ramp) {
                    want_image = (u32)(RAMP_UNIT * TEX_UNIT / 8) | 2u << 20 | 0u << 23 | 6u << 26; /* 32x8 A5I3 */
                    u32 want_palette = (u32)(ADD_SLOT * 512 + RAMP_PALETTE * 16) / 16;
                    if (want_palette != palette) set_palette = want_palette;
                }
                want_attr = 3u << 6 | 31u << 16 | (ramp ? 48u << 24 : 0) | (has_fog && !backdrop ? 1u << 15 : 0);
                pack_begin();
                if (set_palette != ~0u) { pack(CMD_PLTTBASE); put(set_palette); palette = set_palette; }
                if (want_image != image) { pack(CMD_TEXIMAGE); put(want_image); image = want_image; }
                if (want_attr != attr) { pack(CMD_POLYATTR); put(want_attr); attr = want_attr; }
                pack(CMD_BEGIN); put(0);
                pack_end();
                u32 bf = tr->backdrop_fog, lit_of[3], inv_scale = textured ? (16u << 10) / tr->uses[tf.use].scale : 0;
                if (textured) { /* (TV_ALL_FOG: another vertex's colour) */
                    u32 other = 0;
                    for (int k = 0; k < 3; ++k) {
                        lit_of[k] = tr->verts[tb->vbase + ((w >> (8 * k)) & 0xFF)].colour;
                        if (lit_of[k] != TV_ALL_FOG) other = lit_of[k];
                    }
                    for (int k = 0; k < 3; ++k) if (lit_of[k] == TV_ALL_FOG) lit_of[k] = other;
                }
                u32 bf15 = (bf & 0xFF) >> 3 | ((bf >> 8 & 0xFF) >> 3) << 5 | ((bf >> 16 & 0xFF) >> 3) << 10;
                for (int k = 0; k < 3; ++k) {
                    const TVert *tv3 = &tr->verts[tb->vbase + ((w >> (8 * k)) & 0xFF)];
                    int x = (s16)tv3->xy + b->ofs_x, y = (s16)(tv3->xy >> 16) + b->ofs_y, z = tv3->z;
                    u32 colour = 0;
                    if (textured) { /* the vertex colour over the palette's scale */
                        u32 c = lit_of[k], sh = c & 0x8000 ? 3 : 2;
                        u32 r = ((c & 31) << sh) * inv_scale >> 12, g = ((c >> 5 & 31) << sh) * inv_scale >> 12,
                            bl = ((c >> 10 & 31) << sh) * inv_scale >> 12;
                        colour = (r > 31 ? 31 : r) | (g > 31 ? 31 : g) << 5 | (bl > 31 ? 31 : bl) << 10;
                    }
                    if (!backdrop && otz >= 0xFA0 && z >= zb) z = zb - 1; /* (the PS1 draws it before the backdrop) */
                    if (backdrop) { /* (its vertices' depth field holds their fog as an alpha) */
                        if (pass == 1) {
                            int a = tv3->z;
                            colour = ramp ? bf15 : ((bf15 & 31) * a / 31) | ((bf15 >> 5 & 31) * a / 31) << 5 | ((bf15 >> 10) * a / 31) << 10;
                        }
                        z = zb + pass;
                    }
                    pack_begin();
                    pack(CMD_COLOR); put(colour);
                    if (textured) {
                        int u = (int)(uvw[k] & 0xFF) - tu, v = (int)((uvw[k] >> 8) & 0xFF) - tv;
                        pack(CMD_TEXCOORD); put((u32)(u16)(u * 16) | (u32)(u16)(v * 16) << 16);
                    } else if (ramp) {
                        pack(CMD_TEXCOORD); put((u32)(u16)(tv3->z * 16 + 8) | (u32)(u16)(4 * 16) << 16);
                    }
                    pack(CMD_VTX16); put((u32)(u16)x | (u32)(u16)y << 16); put((u32)(u16)z);
                    pack_end();
                }
                ++polys;
                vertices += 3;
            }
        }
    reserve();
    fifo(0x50, GL_TRANS_MANUALSORT); /* SWAP_BUFFERS: shown from the next VBlank */
    chunk_words = wp - chunks[chunk];
    gxh_stats_polys = polys;
}
#undef reserve
#undef put
#undef fifo
#undef pack_begin
#undef pack
#undef pack_end

/* ---- backend ----------------------------------------------------------------------------------------------- */

static void soft_touch(int x0, int y0, int x1, int y1) {
    if (x0 < gp0_state.area_x0) x0 = gp0_state.area_x0;
    if (y0 < gp0_state.area_y0) y0 = gp0_state.area_y0;
    if (x1 > gp0_state.area_x1) x1 = gp0_state.area_x1;
    if (y1 > gp0_state.area_y1) y1 = gp0_state.area_y1;
    if (x1 < x0 || y1 < y0) return;
    dirty_cells(x0, y0, x1, y1);
    regions_touch(x0, y0, x1 - x0 + 1, y1 - y0 + 1);
    if (!ui_flushing) { ui_note_write(x0, y0, x1 - x0 + 1, y1 - y0 + 1); ui_dirty_rect(x0, y0, x1, y1); }
}

/* ---- UI redraw skipping ------------------------------------------------------------------------------------
 * The battle UI redraws every window, glyph and gauge into the displayed buffer each VBlank, nearly all of it as
 * before. Those primitives are deferred; at the next flush the list is compared with the previous one entry by
 * entry. Entries that differ (or whose texture page or CLUT row was written), their old positions and any other
 * writes to the buffer make a dirty area, and only primitives overlapping it are drawn, in order. That is exact
 * for opaque primitives: drawing one over its own pixels again changes nothing, and like the PS1 nothing restores
 * what a removed one left. A list with semi-transparent or mask-tested primitives is drawn whole. Any other VRAM
 * operation flushes first, so order against uploads, moves, fills, readbacks and scene drawing is kept. */
enum { UI_POLY, UI_LINE, UI_RECT };
/* A deferred primitive, packed (compared and kept every VBlank): its mode, vertices, and the drawing state the
 * software rasterizer reads (area, mask bits, texture page, texture window in 8-texel units). */
enum { UI_TEXTURED = 1, UI_RAW = 2, UI_SEMI = 4, UI_GOURAUD = 8, UI_SET_MASK = 16, UI_CHECK_MASK = 32 };
typedef struct { s16 x, y; u8 r, g, b, u, v, pad[3]; } UiVtx;
typedef struct {
    u8 kind, n, flags, abr;
    u8 depth, twin[3];                  /* twin: mask x, mask y, offset x (offset y below) / 8 */
    s16 w, h;
    u16 tx, ty, clut_x, clut_y;
    s16 area[4];                        /* x0, y0, x1, y1 inclusive */
    u32 texpage;
    u8 twin_off_y, pad[3];
    UiVtx v[4];
} UiPrim;

static void ui_mode(const UiPrim *p, Gp0Mode *m) {
    *m = (Gp0Mode){p->flags & UI_TEXTURED ? 1 : 0, p->flags & UI_RAW ? 1 : 0, p->flags & UI_SEMI ? 1 : 0,
                   p->flags & UI_GOURAUD ? 1 : 0, p->abr, p->depth, p->tx, p->ty, p->clut_x, p->clut_y};
}
#define UI_MAX 192   /* the battle UI draws about 110 a VBlank; more are drawn at once (ui_defer), as exactly */
static UiPrim *ui_list[2]; /* [0] the previous VBlank's, [1] this one's (swapped); in psx_spare_alloc's range */
static int ui_count[2], ui_overflow;
static Buffer *ui_buffer;             /* the displayed buffer the list belongs to */
#define UI_RECTS 64
static s16 ui_dirty[UI_RECTS][4];     /* x0, y0, x1, y1 inclusive */
static int ui_dirty_count, ui_dirty_all;

static void ui_bounds(const UiPrim *p, int *x0, int *y0, int *x1, int *y1) {
    if (p->kind == UI_RECT) { *x0 = p->v[0].x; *y0 = p->v[0].y; *x1 = p->v[0].x + p->w - 1; *y1 = p->v[0].y + p->h - 1; }
    else {
        *x0 = *x1 = p->v[0].x; *y0 = *y1 = p->v[0].y;
        for (int i = 1; i < p->n; ++i) {
            if (p->v[i].x < *x0) *x0 = p->v[i].x;
            if (p->v[i].x > *x1) *x1 = p->v[i].x;
            if (p->v[i].y < *y0) *y0 = p->v[i].y;
            if (p->v[i].y > *y1) *y1 = p->v[i].y;
        }
    }
    /* drawing offset already applied; the drawing area clips */
    if (*x0 < p->area[0]) *x0 = p->area[0];
    if (*y0 < p->area[1]) *y0 = p->area[1];
    if (*x1 > p->area[2]) *x1 = p->area[2];
    if (*y1 > p->area[3]) *y1 = p->area[3];
}

static void ui_dirty_rect(int x0, int y0, int x1, int y1) {
    if (x1 < x0 || y1 < y0 || ui_dirty_all) return;
    if (ui_dirty_count == UI_RECTS) { ui_dirty_all = 1; return; }
    s16 *r = ui_dirty[ui_dirty_count++];
    r[0] = (s16)x0; r[1] = (s16)y0; r[2] = (s16)x1; r[3] = (s16)y1;
}

/* Texture sources written since the previous list: an entry sampling them must count as changed. */
#define UI_SOURCES 64
static s16 ui_written[UI_SOURCES][4];
static int ui_written_count, ui_written_all;

static void ui_note_write(int x, int y, int w, int h) {
    if (ui_written_all) return;
    if (ui_written_count == UI_SOURCES) { ui_written_all = 1; return; }
    s16 *r = ui_written[ui_written_count++];
    r[0] = (s16)x; r[1] = (s16)y; r[2] = (s16)(x + w - 1); r[3] = (s16)(y + h - 1);
}

static int ui_source_written(const UiPrim *p) {
    if (!(p->flags & UI_TEXTURED)) return 0;
    if (ui_written_all) return 1;
    int per = texels_per_halfword(p->depth > 2 ? 2 : p->depth);
    int px0 = p->tx, px1 = p->tx + 256 / per - 1, py0 = p->ty, py1 = p->ty + 255;
    for (int i = 0; i < ui_written_count; ++i) {
        const s16 *r = ui_written[i];
        if (px0 <= r[2] && r[0] <= px1 && py0 <= r[3] && r[1] <= py1) return 1;
        if (p->depth < 2 && r[1] <= p->clut_y && p->clut_y <= r[3] && r[0] <= p->clut_x + 255 &&
            p->clut_x <= r[2]) return 1;
    }
    return 0;
}

static void soft_touch(int x0, int y0, int x1, int y1);

/* Draw p, within clip (x0, y0, x1, y1 inclusive) when given: its drawing area narrowed to it. */
static void ui_draw(const UiPrim *p, const s16 *clip) {
    Gp0State saved = gp0_state;
    gp0_state.area_x0 = p->area[0]; gp0_state.area_y0 = p->area[1];
    gp0_state.area_x1 = p->area[2]; gp0_state.area_y1 = p->area[3];
    gp0_state.texpage = p->texpage;
    gp0_state.twin_mask_x = p->twin[0] * 8u; gp0_state.twin_mask_y = p->twin[1] * 8u;
    gp0_state.twin_off_x = p->twin[2] * 8u; gp0_state.twin_off_y = p->twin_off_y * 8u;
    gp0_state.set_mask = !!(p->flags & UI_SET_MASK); gp0_state.check_mask = !!(p->flags & UI_CHECK_MASK);
    if (clip) {
        if (gp0_state.area_x0 < clip[0]) gp0_state.area_x0 = clip[0];
        if (gp0_state.area_y0 < clip[1]) gp0_state.area_y0 = clip[1];
        if (gp0_state.area_x1 > clip[2]) gp0_state.area_x1 = clip[2];
        if (gp0_state.area_y1 > clip[3]) gp0_state.area_y1 = clip[3];
        if (gp0_state.area_x1 < gp0_state.area_x0 || gp0_state.area_y1 < gp0_state.area_y0) { gp0_state = saved; return; }
    }
    Gp0Mode m;
    Gp0Vtx v[4];
    ui_mode(p, &m);
    for (int i = 0; i < p->n; ++i)
        v[i] = (Gp0Vtx){p->v[i].x, p->v[i].y, p->v[i].r, p->v[i].g, p->v[i].b, p->v[i].u, p->v[i].v};
    if (p->kind == UI_RECT) psx_softgpu.rect(&m, v, p->w, p->h);
    else if (p->kind == UI_LINE) psx_softgpu.line(&m, &v[0], &v[1]);
    else psx_softgpu.poly(&m, v, p->n);
    int x0, y0, x1, y1;
    ui_bounds(p, &x0, &y0, &x1, &y1);
    if (x1 >= x0 && y1 >= y0) soft_touch(x0, y0, x1, y1);
    gp0_state = saved;
}

int gxh_stats_ui_drawn, gxh_stats_ui_skipped;

/* entries are zeroed before they're filled (ui_defer), so padding compares equal */
static int ui_same(const UiPrim *a, const UiPrim *b) {
    _Static_assert(sizeof(UiPrim) % 4 == 0, "UiPrim compares by words");
    const u32 *x = (const u32 *)a, *y = (const u32 *)b;
    for (unsigned i = 0; i < sizeof(UiPrim) / 4; ++i) if (x[i] != y[i]) return 0;
    return 1;
}

static void ui_flush(void) {
    if (!ui_count[1] || ui_flushing) return;
    ui_flushing = 1;
    UiPrim *prev = ui_list[0], *cur = ui_list[1];
    int n = ui_count[1], m = ui_count[0];
    int whole = ui_overflow;
    for (int i = 0; i < n && !whole; ++i)
        if (cur[i].flags & (UI_SEMI | UI_CHECK_MASK)) whole = 1;
    if (!whole) {
        for (int i = 0; i < n || i < m; ++i) {
            int x0, y0, x1, y1;
            int same = i < n && i < m && ui_same(&cur[i], &prev[i]) && !ui_source_written(&cur[i]);
            if (same) continue;
            if (i < n) { ui_bounds(&cur[i], &x0, &y0, &x1, &y1); ui_dirty_rect(x0, y0, x1, y1); }
            if (i < m) { ui_bounds(&prev[i], &x0, &y0, &x1, &y1); ui_dirty_rect(x0, y0, x1, y1); }
        }
    }
    /* An entry over the dirty area is drawn clipped to it: drawn whole, an unchanged window background would
     * cover a skipped glyph outside the dirty area. (Only opaque lists get here, so dirty rectangles that
     * overlap each other draw some pixels twice, harmlessly.) */
    for (int i = 0; i < n; ++i) {
        int x0, y0, x1, y1;
        ui_bounds(&cur[i], &x0, &y0, &x1, &y1);
        if (whole || ui_dirty_all) { ui_draw(&cur[i], NULL); gxh_stats_ui_drawn++; continue; }
        int drawn = 0;
        for (int k = 0; k < ui_dirty_count; ++k) {
            const s16 *r = ui_dirty[k];
            if (x0 <= r[2] && r[0] <= x1 && y0 <= r[3] && r[1] <= y1) { ui_draw(&cur[i], r); drawn = 1; }
        }
        if (drawn) gxh_stats_ui_drawn++;
        else gxh_stats_ui_skipped++;
    }
    /* this list is the next one's reference; written sources and dirty areas start over */
    ui_list[0] = cur; ui_list[1] = prev;
    ui_count[0] = n;
    ui_count[1] = 0;
    ui_overflow = 0;
    ui_dirty_count = ui_dirty_all = 0;
    ui_written_count = ui_written_all = 0;
    ui_flushing = 0;
}

/* Defer a primitive drawn into the displayed buffer. Returns 0 when it has to be drawn now. */
/* b: the displayed buffer, which the drawing area is in */
static int ui_defer(Buffer *b, int kind, const Gp0Mode *m, const Gp0Vtx *v, int n, int w, int h) {
    if (ui_buffer != b) {
        /* another buffer is shown: its previous list is stale, and everything it holds may differ */
        ui_flush();
        ui_buffer = b;
        ui_count[0] = 0;
    }
    if (ui_count[1] == UI_MAX) { ui_overflow = 1; ui_flush(); return 0; }
    UiPrim *p = &ui_list[1][ui_count[1]++];
    memset(p, 0, sizeof *p); /* padding and unused vertices compare equal */
    p->kind = (u8)kind; p->n = (u8)n; p->w = (s16)w; p->h = (s16)h;
    p->flags = (u8)((m->textured ? UI_TEXTURED : 0) | (m->raw ? UI_RAW : 0) | (m->semi ? UI_SEMI : 0) |
                    (m->gouraud ? UI_GOURAUD : 0) | (gp0_state.set_mask ? UI_SET_MASK : 0) |
                    (gp0_state.check_mask ? UI_CHECK_MASK : 0));
    p->abr = (u8)m->abr; p->depth = (u8)m->depth;
    p->tx = (u16)m->tx; p->ty = (u16)m->ty; p->clut_x = (u16)m->clut_x; p->clut_y = (u16)m->clut_y;
    for (int i = 0; i < n; ++i)
        p->v[i] = (UiVtx){(s16)v[i].x, (s16)v[i].y, (u8)v[i].r, (u8)v[i].g, (u8)v[i].b, (u8)v[i].u, (u8)v[i].v, {0}};
    p->area[0] = (s16)gp0_state.area_x0; p->area[1] = (s16)gp0_state.area_y0;
    p->area[2] = (s16)gp0_state.area_x1; p->area[3] = (s16)gp0_state.area_y1;
    p->texpage = gp0_state.texpage;
    p->twin[0] = (u8)(gp0_state.twin_mask_x / 8); p->twin[1] = (u8)(gp0_state.twin_mask_y / 8);
    p->twin[2] = (u8)(gp0_state.twin_off_x / 8); p->twin_off_y = (u8)(gp0_state.twin_off_y / 8);
    return 1;
}

/* Field: what makes a scene go software (go_software); gxh_stats_soft gathers the reasons (status row) */
/* Field: the primitive just recorded is a UI page's: the 2D layer's, with its texture window if one is set */
static void mark_ui(Buffer *b) {
    Prim *p = &b->prims[b->count - 1];
    p->flags |= P_UI;
    if (gp0_state.twin_mask_x || gp0_state.twin_mask_y) {
        if (b->twin_count == (int)(sizeof b->twins / sizeof *b->twins)) {
            /* more than its table holds (Fossil Roo's track map: field 1403 wrote past it into the next buffer and
             * the statics after, then jumped to a garbage address): the scene goes software */
            gxh_stats_soft |= 128;
            go_software(b);
            return;
        }
        p->flags |= P_TWIN;
        b->twins[b->twin_count].k = (u16)(b->count - 1);
        b->twins[b->twin_count].mask_x = (u8)gp0_state.twin_mask_x; b->twins[b->twin_count].mask_y = (u8)gp0_state.twin_mask_y;
        b->twins[b->twin_count].off_x = (u8)gp0_state.twin_off_x; b->twins[b->twin_count].off_y = (u8)gp0_state.twin_off_y;
        ++b->twin_count;
    }
}

static int ui_page_of(const Gp0Mode *m) { return m->textured && m->ty == 256 && (m->tx == 896 || m->tx == 960); }
static int field_unexact(const Buffer *b, const Gp0Mode *m) {
    /* (a subtractive blend is decided when the scene is sent: field_layers); a texture window only on the UI pages,
     * which the replay applies (twins) */
    int twin = gp0_state.twin_mask_x || gp0_state.twin_mask_y;
    int why = (twin && (!ui_page_of(m) || b->twin_count == 32) ? 2 : 0) |
              (gp0_state.set_mask || gp0_state.check_mask ? 4 : 0) | (b->count >= prims_max ? 8 : 0);
    gxh_stats_soft |= why;
    return why;
}

static void hybrid_poly(const Gp0Mode *m, const Gp0Vtx *v, int n) {
    Buffer *shown, *b = target(&shown);
    if (b && b == shown && ui_defer(b, UI_POLY, m, v, n, 0, 0)) return;
    ui_flush();
    /* Untextured additive glows (Gouraud, black at the edges) go to the 3D layer through the alpha ramp; outside a
     * scene (offscreen) they are drawn in software, marked, over the clear value. */
    int glow = m->semi && !m->textured && (m->abr == 1 || m->abr == 3);
    if (b && b != shown) {
        if (!triangle_area(v, v + 1, v + 2) && (n == 3 || !triangle_area(v + 1, v + 2, v + 3))) return;
        /* A drawing area covering the whole buffer clips where the 3D engine's view volume does (the scene's
         * 320 x display-height window from the buffer's corner, submit), so only a smaller one needs clipping. */
        int clipped = 0;
        if (gp0_state.area_x0 > b->x || gp0_state.area_y0 > b->y || gp0_state.area_x1 + 1 < b->x + 320 ||
            gp0_state.area_y1 + 1 < b->y + b->h)
            for (int i = 0; i < n; ++i)
                if (v[i].x < gp0_state.area_x0 || v[i].x > gp0_state.area_x1 + 1 ||
                    v[i].y < gp0_state.area_y0 || v[i].y > gp0_state.area_y1 + 1) clipped = 1;
        if (place->field && clipped) gxh_stats_soft |= 16;
        if (place->field && (b->soft || clipped || field_unexact(b, m))) {
            if (!b->soft) go_software(b);
            fm_soft_upto(b, (int)psx_ot_slot);
            psx_softgpu.poly(m, v, n);
            return;
        }
        if (!clipped) {
            int before = b->count;
            record(b, m, v, n);
            if (place->field && b->count > before && ui_page_of(m)) mark_ui(b);
            else b->polys++;
            return;
        }
        Gp0Vtx t[3] = {v[0], v[1], v[2]};
        record_clipped_triangle(b, m, t);
        if (n == 4) { t[0] = v[1]; t[1] = v[2]; t[2] = v[3]; record_clipped_triangle(b, m, t); }
        return;
    }
    /* the 2D layer changes under the triangles the GPU draws: it skips those spanning 1024+ x or 512+ y */
    int x0 = 1 << 30, y0 = 1 << 30, x1 = -(1 << 30), y1 = -(1 << 30);
    for (int t = 0; t + 3 <= n; ++t) {
        int tx0 = v[t].x, tx1 = tx0, ty0 = v[t].y, ty1 = ty0;
        for (int i = t + 1; i < t + 3; ++i) {
            if (v[i].x < tx0) tx0 = v[i].x;
            if (v[i].x > tx1) tx1 = v[i].x;
            if (v[i].y < ty0) ty0 = v[i].y;
            if (v[i].y > ty1) ty1 = v[i].y;
        }
        if (tx1 - tx0 >= 1024 || ty1 - ty0 >= 512) continue;
        if (tx0 < x0) x0 = tx0;
        if (ty0 < y0) y0 = ty0;
        if (tx1 > x1) x1 = tx1;
        if (ty1 > y1) y1 = ty1;
    }
    if (x1 < x0) return;
    if (glow && b) psx_softgpu_set_marks(marks, MARK_ADD);
    psx_softgpu.poly(m, v, n);
    psx_softgpu_set_marks(NULL, 0);
    soft_touch(x0, y0, x1, y1);
}

/* Line a-b clipped to the drawing area (Liang-Barsky; colours interpolated): 0 when it lies outside */
static int clip_line(const Gp0Vtx *a, const Gp0Vtx *b, Gp0Vtx *ca, Gp0Vtx *cb) {
    int dx = b->x - a->x, dy = b->y - a->y;
    int p[4] = {-dx, dx, -dy, dy};
    int q[4] = {a->x - gp0_state.area_x0, gp0_state.area_x1 - a->x, a->y - gp0_state.area_y0, gp0_state.area_y1 - a->y};
    int t0n = 0, t0d = 1, t1n = 1, t1d = 1; /* t0 = t0n / t0d, t1 = t1n / t1d, as fractions of the line */
    for (int i = 0; i < 4; ++i) {
        if (!p[i]) { if (q[i] < 0) return 0; continue; }
        int n = q[i], d = p[i];
        if (d < 0) { n = -n; d = -d; if ((long long)n * t1d > (long long)t1n * d) return 0; /* entering */
                     if ((long long)n * t0d > (long long)t0n * d) t0n = n, t0d = d; }
        else { if ((long long)n * t0d < (long long)t0n * d) return 0; /* leaving */
               if ((long long)n * t1d < (long long)t1n * d) t1n = n, t1d = d; }
    }
#define AT(v, n, d, f) (v.f + (int)((long long)(b->f - a->f) * (n) / (d)))
    *ca = *a; *cb = *a;
    ca->x = AT((*a), t0n, t0d, x); ca->y = AT((*a), t0n, t0d, y);
    ca->r = AT((*a), t0n, t0d, r); ca->g = AT((*a), t0n, t0d, g); ca->b = AT((*a), t0n, t0d, b);
    cb->x = AT((*a), t1n, t1d, x); cb->y = AT((*a), t1n, t1d, y);
    cb->r = AT((*a), t1n, t1d, r); cb->g = AT((*a), t1n, t1d, g); cb->b = AT((*a), t1n, t1d, b);
#undef AT
    return 1;
}

static void hybrid_line(const Gp0Mode *m, const Gp0Vtx *a, const Gp0Vtx *b2) {
    Buffer *shown, *b = target(&shown);
    Gp0Vtx pair[2] = {*a, *b2};
    if (b && b == shown && ui_defer(b, UI_LINE, m, pair, 2, 0, 0)) return;
    ui_flush();
    /* A scene's line (a field's, in its order; a battle's translucent one: Burmecia's rain) to the 3D engine: a
     * triangle with two equal vertices is drawn as a one-pixel line, blending with the scene (on the 2D layer it
     * blended with the buffer's key or clear value and covered the scene opaquely) */
    Gp0Vtx ca, cb;
    if (b && b != shown && (place->field ? !b->soft && !field_unexact(b, m) : m->semi) && clip_line(a, b2, &ca, &cb)) {
        Gp0Vtx t[3] = {ca, cb, cb};
        Gp0Mode lm = *m;
        lm.textured = 0;
        record(b, &lm, t, 3);
        if (place->field) b->polys++;
        return;
    }
    if (place->field && b && b->soft) fm_soft_upto(b, (int)psx_ot_slot);
    psx_softgpu.line(m, a, b2);
    soft_touch(a->x < b2->x ? a->x : b2->x, a->y < b2->y ? a->y : b2->y,
               a->x > b2->x ? a->x : b2->x, a->y > b2->y ? a->y : b2->y);
}

/* A field's background is rows of sprites side by side with continuous texels: one continuing the scene's last
 * primitive (same mode, page, CLUT and colour, same rows, starting where it ends, texels following on) widens it, so
 * a row is one quad for the 3D engine's 2048 (a field scene can draw 2800 sprites and polygons). */
static int extend_rect(Buffer *b, const Gp0Mode *m, const Gp0Vtx *c, int w, int h) {
    if (!b->count) return 0;
    Prim *p = &b->prims[b->count - 1];
    u8 flags = (u8)((m->textured ? P_TEXTURED : 0) | (m->raw ? P_RAW : 0) | (m->semi ? P_SEMI : 0) | P_RECT |
                    (gp0_state.texpage & 0x200 ? P_DITHER : 0));
    int x = c->x - b->x, y = c->y - b->y;
    if (p->flags != flags || p->abr != m->abr || p->x[1] != x || p->y[0] != y || p->y[2] != y + h) return 0;
    if (p->r[0] != (u8)c->r || p->g[0] != (u8)c->g || p->b[0] != (u8)c->b) return 0;
    if (m->textured && (p->depth != (m->depth > 2 ? 2 : m->depth) || p->tx != m->tx || p->ty != m->ty ||
                        p->clut_x != m->clut_x || p->clut_y != m->clut_y || p->u[1] + 1 != c->u ||
                        p->v[0] != c->v || p->v[2] != c->v + h - 1)) return 0;
    p->x[1] = p->x[3] = (s16)(x + w);
    p->u[1] = p->u[3] = (u8)(c->u + w - 1);
    return 1;
}

/* hybrid_rect's field case for its common sprite (a background tile): inside the drawing area, without a texture
 * window, mask bits or a page wrap, not on the UI pages. Recorded as the general path records it, without its copies
 * (0: left to it). */
static int field_sprite(Buffer *b, const Gp0Mode *m, const Gp0Vtx *v, int w, int h) {
    if (b->soft || gp0_state.twin_mask_x || gp0_state.twin_mask_y || gp0_state.set_mask || gp0_state.check_mask ||
        b->count >= prims_max || w <= 0 || h <= 0 || ui_page_of(m))
        return 0;
    if (m->textured && (v->u + w > 256 || v->v + h > 256)) return 0;
    if (v->x < gp0_state.area_x0 || v->y < gp0_state.area_y0 || v->x + w > gp0_state.area_x1 + 1 ||
        v->y + h > gp0_state.area_y1 + 1)
        return 0;
    ui_flush();
    if (extend_rect(b, m, v, w, h)) return 1;
    note_walk(b);
    PrimCopy t;
    Prim *p = &t.p;
    p->n = 4;
    p->flags = (u8)((m->textured ? P_TEXTURED : 0) | (m->raw ? P_RAW : 0) | (m->semi ? P_SEMI : 0) | P_RECT |
                    (gp0_state.texpage & 0x200 ? P_DITHER : 0));
    p->abr = (u8)m->abr; p->depth = (u8)(m->depth > 2 ? 2 : m->depth);
    p->tx = (u16)m->tx; p->ty = (u16)m->ty; p->clut_x = (u16)m->clut_x; p->clut_y = (u16)m->clut_y;
    s16 x0 = (s16)(v->x - b->x), y0 = (s16)(v->y - b->y);
    p->x[0] = p->x[2] = x0; p->x[1] = p->x[3] = (s16)(x0 + w);
    p->y[0] = p->y[1] = y0; p->y[2] = p->y[3] = (s16)(y0 + h);
    u8 r = (u8)(v->r < 0 ? 0 : v->r > 255 ? 255 : v->r), g = (u8)(v->g < 0 ? 0 : v->g > 255 ? 255 : v->g),
       bl = (u8)(v->b < 0 ? 0 : v->b > 255 ? 255 : v->b);
    u8 u0 = (u8)v->u, u1 = (u8)(v->u + w - 1), v0 = (u8)v->v, v1 = (u8)(v->v + h - 1);
    for (int i = 0; i < 4; ++i) { p->r[i] = r; p->g[i] = g; p->b[i] = bl; }
    p->u[0] = p->u[2] = u0; p->u[1] = p->u[3] = u1;
    p->v[0] = p->v[1] = v0; p->v[2] = p->v[3] = v1;
    note_depth(b);
    store_prim(&b->prims[b->count++], &t);
    return 1;
}

static void hybrid_rect(const Gp0Mode *m, const Gp0Vtx *v, int w, int h) {
    Buffer *shown, *b = target(&shown);
    if (b && b != shown && place->field && field_sprite(b, m, v, w, h)) return;
    if (b && b == shown && ui_defer(b, UI_RECT, m, v, 1, w, h)) return;
    ui_flush();
    /* A semi-transparent sprite or tile in the scene is a quad of the 3D layer like a polygon, so it blends with it
     * (on the 2D layer it would cover the scene), unless it uses a texture window or its texels wrap past the page
     * edge, which 8-bit coordinates can't say. Opaque ones stay exact on the 2D layer: as quads they would go under
     * 2D pixels drawn before them (a window's background over its text). */
    /* In a field the opaque ones are the background: quads too, in the scene's order, but for the UI pages' sprites
     * (text, window parts: drawn last, over everything), which stay exact on the 2D layer. */
    int ui_page = m->textured && m->ty == 256 && (m->tx == 896 || m->tx == 960);
    if (b && b != shown && place->field) {
        /* a field records every sprite in order, the UI pages' too (P_UI); one past its page edge goes software */
        int twin = gp0_state.twin_mask_x || gp0_state.twin_mask_y;
        int wraps = m->textured && !twin && (v->u + w > 256 || v->v + h > 256);
        if (wraps) gxh_stats_soft |= 32;
        if (b->soft || field_unexact(b, m) || wraps) {
            if (!b->soft) go_software(b);
            fm_soft_upto(b, (int)psx_ot_slot);
            psx_softgpu.rect(m, v, w, h);
            return;
        }
    }
    int field_ui_twin = place->field && ui_page && (gp0_state.twin_mask_x || gp0_state.twin_mask_y);
    if (b && b != shown && (m->semi || place->field) &&
        ((!gp0_state.twin_mask_x && !gp0_state.twin_mask_y && (!m->textured || (v->u + w <= 256 && v->v + h <= 256))) ||
         field_ui_twin)) {
        /* clipped to the drawing area: texels follow pixels one to one */
        Gp0Vtx c = *v;
        int dx = gp0_state.area_x0 - c.x, dy = gp0_state.area_y0 - c.y;
        if (dx > 0) { c.x += dx; c.u += dx; w -= dx; }
        if (dy > 0) { c.y += dy; c.v += dy; h -= dy; }
        if (c.x + w > gp0_state.area_x1 + 1) w = gp0_state.area_x1 + 1 - c.x;
        if (c.y + h > gp0_state.area_y1 + 1) h = gp0_state.area_y1 + 1 - c.y;
        if (w <= 0 || h <= 0) return;
        if (place->field && !ui_page && extend_rect(b, m, &c, w, h)) return;
        Gp0Vtx q[4] = {c, c, c, c};
        q[1].x += w; q[1].u += w - 1;
        q[2].y += h; q[2].v += h - 1;
        q[3].x += w; q[3].u += w - 1; q[3].y += h; q[3].v += h - 1;
        Gp0Mode flat = *m;
        flat.gouraud = 0;
        int before = b->count;
        record(b, &flat, q, 4);
        if (b->count > before) {
            b->prims[b->count - 1].flags |= P_RECT;
            if (place->field && ui_page) mark_ui(b);
        }
        return;
    }
    psx_softgpu.rect(m, v, w, h);
    soft_touch(v->x, v->y, v->x + w - 1, v->y + h - 1);
}

static const Gp0Backend hybrid_backend = {hybrid_poly, hybrid_line, hybrid_rect, 0, 0};

/* A fill (ClearImage, GP0 02) over most of a buffer is that scene's clear: the 3D layer's clear colour. */
void psx_vram_filled(int x, int y, int w, int h, unsigned int rgb15) {
    if (!active) return;
    Buffer *b = buffer_at(x, y);
    if (b && w * h * 2 >= b->w * b->h) {
        int covers = x <= b->x && y <= b->y && x + w >= b->x + b->w && y + h >= b->y + b->h;
        if (b->shown && b != displayed()) start_scene(b, covers);
        b->clear = rgb15;
        if (covers) clear_fill = b; /* psx_vram_touch, next: the buffer is all its clear value */
    }
}

/* ---- presentation ------------------------------------------------------------------------------------------ */

#define BLOCK 32
static u8 block_shown[6][8];   /* the 2D layer has pixels in this 32x32 output block */
static Buffer *last_shown;
static u32 block_hash[6][8];
static int layer_bg = -1;
/* Field: a software frame's bitmap BG (BG3, 16-bit, bank A when bank_a_bg) behind the transparent 3D layer */
static int back_bg = -1;
static int bank_a_bg;     /* field: bank A is the software frame's bitmap, not textures */
static int bank_a_add;    /* field: and that bitmap is the scene's additive primitives, added over the 3D layer */
/* a scaled block, stride 256 (psx_spare_alloc): the 2D layer's and the background layer's */
static u16 *scaled_block(void) {
    static u16 *block;
    if (!block) block = psx_spare_alloc(BLOCK * 256 * sizeof *block);
    return block;
}
static void (*scale_fn)(u16 *dst, int x, int y, int w, int h);
static void reset_colours(void);     /* the 2D layer: an 8-bit bitmap BG in bank E with a palette built as colours appear */

/* The two scenes' primitives live in the battle overlay's MIPS code range in PS1 RAM, which the port never runs and
 * nothing reads while ovl_0b is resident (docs/ds-mode.md): buffer 0 in its first code segments, buffer 1 after the
 * embedded table. Only while ovl_0b is resident; a sector loaded over them empties them (gxh_ram_overwritten). */
_Static_assert(OVL0B_PRIMS_AT_0 + MAX_PRIMS * sizeof(Prim) <= OVL0B_FX_AREA_AT, "buffer 0 overruns the module area");
_Static_assert(OVL0A_PRIMS_AT_0 + OVL0A_FIELD_PRIMS * sizeof(Prim) <= OVL0A_PRIMS_AT_1, "field buffer 0 overruns buffer 1");
_Static_assert(OVL0A_PRIMS_AT_1 + OVL0A_FIELD_PRIMS * sizeof(Prim) <= OVL0A_CODE_END, "field buffer 1 overruns ovl_0a's code");
_Static_assert(OVL0B_PRIMS_AT_1 + MAX_PRIMS * sizeof(Prim) <= OVL0B_RANKS_AT, "buffer 1 overruns the rank tables");
_Static_assert(OVL0B_RANKS_AT + 2 * RANK_BUCKETS * 2 <= OVL0B_PRIMTAB_AT, "the rank tables overrun the primitive tables");
_Static_assert(Z_STEP * (BATTLE_PRIMS + POOL_BYTES / 8 + 1) < 32768, "battle depths overrun VTX16");
_Static_assert(OVL0B_PRIMTAB_AT + PRIM_TABLES * BATTLE_PRIMS <= OVL0B_HEADS_AT, "the primitive tables overrun the entry heads");
_Static_assert(OVL0B_HEADS_AT + 0x1010 * 2 <= OVL0B_CODE_END, "the entry heads overrun ovl_0b's code");
_Static_assert(OVL0A_PRIMS_AT_1 + OVL0A_FIELD_PRIMS * (sizeof(Prim) + PRIM_TABLES) <= OVL0A_CODE_END,
               "field buffer 1 and the primitive tables overrun ovl_0a's code");

static void buffers_overwritten(uintptr_t at, unsigned int len) {
    if (place->world) { /* (the world's slots and terrain after the buffers, in the same code range) */
        uintptr_t lo = (uintptr_t)psx_ram + place->at + OVL0C_PRIMS_AT_0, hi = (uintptr_t)psx_ram + place->at + OVL0C_CODE_END;
        if (at < hi && lo < at + len)
            for (int i = 0; i < 2; ++i) { buffers[i].count = 0; buffers[i].submitted = 0; }
        if (at < hi && lo < at + len) for (int i = 0; i < 3; ++i) terrain_reset(terrain_ring[i]);
    }
    for (int i = 0; i < 2; ++i) {
        uintptr_t lo = (uintptr_t)buffers[i].prims, hi = lo + (buffers[i].pool ? MAX_PRIMS : prims_max) * sizeof(Prim);
        if (at < hi && lo < at + len) { buffers[i].count = 0; buffers[i].submitted = 0; buffers[i].meshes = 0; buffers[i].pool_used = 0; }
    }
    if (place->field && fm_ring[0]) { /* the field models' ring, to the end of ovl_0a's code range (use_place) */
        uintptr_t lo = (uintptr_t)fm_ring[0], hi = (uintptr_t)psx_ram + place->at + OVL0A_CODE_END;
        if (at < hi && lo < at + len)
            for (int i = 0; i < 3; ++i) { fm_ring[i]->gen = ~0u; fm_ring[i]->ngroups = 0; }
    }
}
static void textures_reset(void);
void gxh_ram_overwritten(unsigned int off, unsigned int len) {
    buffers_overwritten((uintptr_t)psx_ram + off, len);
    uintptr_t at = (uintptr_t)psx_ram + off, lo = (uintptr_t)rows, hi = lo + RUN_TABLES;
    if (rows && at < hi && lo < at + len) textures_reset();
}
/* ovl_pack.c: native code was read over [at, at + len) */
void gxh_native_overwritten(uintptr_t at, unsigned int len) { buffers_overwritten(at, len); }

/* A field's second buffer: in slot 0's native code area after the field overlay's ARM code (the slot is sized for
 * battle's), the first then taking more of ovl_0a's MIPS range; or both in that range (OVL0A_FIELD_PRIMS each). */
extern char __psxovl_slot0_end[];
int port_ovl_native_extent(unsigned int id, uintptr_t *at, unsigned int *size);
/* ---- field models ----------------------------------------------------------------------------------------- */

/* fldmdl_drawMesh's groups (port/game/field_mesh.c) are drawn from the model: when the game would link a group's
 * packets, its vertices as fldmdl_projectMesh projected them are kept for the frame's walk (a ring of three by
 * generation, as the world's terrain: the scene is sent two frames later), each with its depth on the OT entries'
 * scale, and each polygon takes its prebuilt packet's colours and texture words (still those of the frame but for
 * texture animations; the user's choice of 2026-10-25: native drawing, per pixel depths). The PS1's culling is kept
 * (NCLIP <= 0 unless double-sided). Groups with translucent polygons, and scenes in software, keep their packets. */
int gxh_stats_fm, gxh_stats_bgc;

PORT_OVERLAY_CODE(0a) int port_native_field_mesh(const NativeFieldMesh *n) {
    /* (not the GPU's backend: a scene gone software keeps the software renderer until it's shown, after this) */
    if (!active || !place->field || !fm_ring[0] || soft_hold) return 0;
    /* (the buffer's soft flag is still its last scene's: this one is walked later) */
    Buffer *b = buffer_at(n->draw_x, n->draw_y);
    if (!b || !b->slots) return 0;
    FStore *st = fm_ring[psx_ot_generation % 3];
    if (st->gen != psx_ot_generation) {
        st->gen = psx_ot_generation; st->ngroups = 0; st->nverts = 0; st->npolys = 0;
        st->front_polys = 0; st->front_verts = 0;
    }
    int nv = n->nverts, np = 0;
    const u8 *g = n->group, *pk = n->packets;
    for (int t = 0; t < 8; ++t) np += *(const u16 *)(g + 2 + 2 * t);
    if (st->ngroups == FM_GROUPS || st->nverts + nv > fm_cap || st->npolys + np > FM_POLYS) return 0;
    /* the opaque polygons (code bit 1 clear in the packets): the translucent ones keep the PS1's order (their caller
     * links them) */
    int opaque = 0;
    for (int t = 0; t < 8; ++t)
        for (int k = *(const u16 *)(g + 2 + 2 * t); k > 0; --k, pk += fm_size[t]) opaque += !(pk[7] & 2);
    if (!opaque) return 0;
    FGroup *fg = &st->groups[st->ngroups++];
    fg->group = g; fg->packets = n->packets; fg->vert0 = (u16)st->nverts; fg->poly0 = (u16)st->npolys;
    fg->src = *(const u8 *const *)(g + 0x1C); fg->polys = (u16)np; fg->verts = (u16)nv;
    /* the PS1's culling (model_link: NCLIP <= 0 unless double-sided), now: the budget counts what's drawn */
    const u8 *src = *(const u8 *const *)(g + 0x1C);
    int pi = st->npolys;
    pk = n->packets;
    for (int t = 0; t < 8; ++t) {
        const int quad = !(t & 1), flags_at = mesh_flags_at[t];
        for (int k = *(const u16 *)(g + 2 + 2 * t); k > 0; --k, src += mesh_size[t], pk += fm_size[t], ++pi) {
            u32 bit = 1u << (pi & 31);
            st->front[pi >> 5] &= ~bit;
            if (pk[7] & 2) continue;
            if (!(src[flags_at] & 2)) {
                u32 s0 = (u32)n->sxy[*(const u16 *)src], s1 = (u32)n->sxy[*(const u16 *)(src + 2)];
                u32 s2 = (u32)n->sxy[*(const u16 *)(src + 4)];
                int x0 = (s16)s0, y0 = (s16)(s0 >> 16);
                if (((s16)s1 - x0) * ((s16)(s2 >> 16) - y0) - ((s16)s2 - x0) * ((s16)(s1 >> 16) - y0) <= 0) continue;
            }
            st->front[pi >> 5] |= bit;
            st->front_polys++;
            st->front_verts += quad ? 4 : 3;
        }
    }
    st->npolys = pi;
    /* xy as projected; a vertex's OT entry: the group's (its bias from the frame's OT) plus SZ / 4 */
    int base = (int)((const u32 *)n->ot - (const u32 *)psx_main_ot);
    u32 nearest = ~0u;
    for (int i = 0; i < nv; ++i) if ((u32)n->sz[i] < nearest) nearest = (u32)n->sz[i];
    nearest >>= 2;
    fg->depth0 = world_entry_depth(base + (int)nearest);
    u32 *vw = st->verts + st->nverts, far = 0;
    for (int i = 0; i < nv; ++i) {
        u32 xy = (u32)n->sxy[i], rel = ((u32)n->sz[i] >> 2) - nearest;
        if (rel > 1023) rel = 1023;
        if (rel > far) far = rel;
        vw[i] = (xy & 0x7FF) | (xy >> 16 & 0x7FF) << 11 | rel << 22;
    }
    fg->depth_far = fg->depth0 - (s32)far * 4;
    st->nverts += nv;
    gxh_stats_fm = st->ngroups;
    return opaque == np ? 1 : 2;
}

static FStore *scene_models(const Buffer *b) {
    /* (a soft scene's are drawn in software: fm_soft_upto) */
    if (!b->slots || !fm_ring[0] || b->soft || !ovl0a_resident()) return NULL;
    FStore *st = fm_ring[b->scene_gen % 3];
    return st->ngroups && st->gen == b->scene_gen ? st : NULL;
}

/* the group still as it was taken (its polygon records and counts) */
static inline int fm_live(const FGroup *fg) {
    const u8 *g = fg->group;
    if (*(const u8 *const *)(g + 0x1C) != fg->src) return 0;
    int np = 0;
    for (int t = 0; t < 8; ++t) np += *(const u16 *)(g + 2 + 2 * t);
    return np == fg->polys;
}

/* the scene's texture uses (fm_emit) */
typedef struct {
    const FStore *fst;
    u32 key[FM_USES], image[FM_USES], palette[FM_USES]; /* key: tpage << 16 | clut; image 0: not drawable */
    s16 tu[FM_USES], tv[FM_USES];
    int last_use, ofs_x, ofs_y;
} FmCtx;

/* a packet's colour word as the 3D engine's (textured: modulated, 0x80 the texel's own: x2 saturated; raw: white) */
static inline __attribute__((always_inline)) u32 fm_colour(u32 c, const int textured, int raw) {
    if (textured) {
        if (raw) return 0x7FFF;
        u32 r = (c & 0xFF) >> 2, g = (c >> 8 & 0xFF) >> 2, bl = (c >> 16 & 0xFF) >> 2;
        return (r > 31 ? 31 : r) | (g > 31 ? 31 : g) << 5 | (bl > 31 ? 31 : bl) << 10;
    }
    return (c & 0xFF) >> 3 | ((c >> 8 & 0xFF) >> 3) << 5 | ((c >> 16 & 0xFF) >> 3) << 10;
}

/* one kind's polygons of a group (t constant: the offsets and branches fold) */
static inline __attribute__((always_inline)) u32 *fm_kind(FmCtx *c, EmitState *es, u32 *wp, const int t, const FGroup *fg,
                                                          const u8 *src, const u8 *pk, int pi, int count) {
    const int quad = !(t & 1), nv = quad ? 4 : 3, textured = t < 4, gouraud = t >= 2 && t <= 5;
    const u32 *vw = c->fst->verts + fg->vert0, *front = c->fst->front;
    const int depth0 = fg->depth0, ofs_x = c->ofs_x, ofs_y = c->ofs_y;
    const u32 nverts = fg->verts;
    for (; count > 0; --count, src += mesh_size[t], pk += fm_size[t], ++pi) {
        if (!(front[pi >> 5] >> (pi & 31) & 1)) continue; /* (culled when taken) */
        if ((u32)*(const u16 *)src >= nverts || (u32)*(const u16 *)(src + 2) >= nverts ||
            (u32)*(const u16 *)(src + 4) >= nverts || (quad && (u32)*(const u16 *)(src + 6) >= nverts)) continue;
        if (wp - chunks[chunk] > CHUNK_WORDS - PRIM_WORDS) { chunk_words = wp - chunks[chunk]; flush_chunk(); wp = chunks[chunk]; }
        if (es->polys >= MAX_POLYS || es->vertices + nv > MAX_VERTICES) { gxh_stats_dropped++; continue; }
        u32 uv[4] = {0, 0, 0, 0}, *header = wp++, ids = 0;
        int shift = 0, tu = 0, tv = 0;
        if (textured) {
            for (int i = 0; i < nv; ++i) uv[i] = *(const u32 *)(pk + fm_xy_at[t][i] + 4);
            u32 key = uv[0] >> 16 | (uv[1] & 0xFFFF0000);
            int u = c->last_use;
            if (c->key[u] != key) {
                for (u = 0; u < c->fst->nuses && c->key[u] != key; ++u) {}
                if (u == c->fst->nuses) { wp = header; gxh_stats_texture_fail++; continue; }
                c->last_use = u;
            }
            if (!c->image[u]) { wp = header; gxh_stats_texture_fail++; continue; }
            tu = c->tu[u]; tv = c->tv[u];
            if (c->palette[u] != ~0u && c->palette[u] != es->palette) {
                ids |= (u32)CMD_PLTTBASE << shift; shift += 8; *wp++ = es->palette = c->palette[u];
            }
            if (c->image[u] != es->image) { ids |= (u32)CMD_TEXIMAGE << shift; shift += 8; *wp++ = es->image = c->image[u]; }
        }
        if (es->attr != INITIAL_ATTR) { ids |= (u32)CMD_POLYATTR << shift; shift += 8; *wp++ = es->attr = INITIAL_ATTR; }
        *header = ids | (u32)CMD_BEGIN << shift;
        *wp++ = quad ? 1 : 0;
        const int raw = pk[7] & 1;
        for (int j = 0; j < nv; ++j) {
            const int i = quad && j >= 2 ? 5 - j : j; /* (the 3D engine's quads go round: 0 1 3 2) */
            if (gouraud || j == 0) { /* (a flat polygon's colour holds for its vertices) */
                *wp++ = textured ? CMD_COLOR | CMD_TEXCOORD << 8 | CMD_VTX16 << 16 : CMD_COLOR | CMD_VTX16 << 8;
                *wp++ = fm_colour(*(const u32 *)(pk + (gouraud ? fm_xy_at[t][i] - 4 : 4)), textured, raw);
            } else *wp++ = textured ? CMD_TEXCOORD | CMD_VTX16 << 8 : CMD_VTX16;
            if (textured)
                *wp++ = (u32)(u16)(((int)(uv[i] & 0xFF) - tu) * 16) | (u32)(u16)(((int)(uv[i] >> 8 & 0xFF) - tv) * 16) << 16;
            u32 w = vw[*(const u16 *)(src + 2 * i)];
            int x = (s32)(w << 21) >> 21, y = (s32)(w << 10) >> 21, z = depth0 - (int)(w >> 22) * 4;
            z = z < -32000 ? -32000 : z > 32000 ? 32000 : z;
            *wp++ = (u32)(u16)(x + ofs_x) | (u32)(u16)(y + ofs_y) << 16;
            *wp++ = (u32)(u16)z;
        }
        es->polys++;
        es->vertices += nv;
    }
    return wp;
}

/* A scene gone software: its native models' polygons with OT entries from e up to the last drawn (farther), as the
 * game's own packets with the vertices it projected, through GP0's decoder into the software renderer (the PS1's
 * drawing; the walk's order between them and the recorded primitives by OT entry) */
PORT_OVERLAY_CODE(0a) __attribute__((target("arm"))) static void fm_soft_draw(Buffer *b, int e) {
    int next = fm_soft_next;
    fm_soft_next = e;
    if (!fm_ring[0] || !b->slots) return;
    const FStore *st = fm_ring[b->scene_gen % 3];
    if (st->gen != b->scene_gen || !st->ngroups) return;
    const Gp0State saved = gp0_state;
    const Gp0Backend *saved_backend = gp0_backend;
    gp0_backend = &psx_softgpu;
    gp0_state.ofs_x = b->x + b->ofs_x; gp0_state.ofs_y = b->y + b->ofs_y;
    gp0_state.area_x0 = b->x; gp0_state.area_y0 = b->y;
    gp0_state.area_x1 = b->x + b->w - 1; gp0_state.area_y1 = b->y + b->h - 1;
    gp0_state.twin_mask_x = gp0_state.twin_mask_y = gp0_state.twin_off_x = gp0_state.twin_off_y = 0;
    gp0_state.set_mask = gp0_state.check_mask = 0;
    for (int gi = 0; gi < st->ngroups; ++gi) {
        const FGroup *fg = &st->groups[gi];
        int e0 = 0x1010 - fg->depth0 / 4; /* its nearest vertex's entry */
        if (e0 >= next || e0 + 1023 < e || !fm_live(fg)) continue;
        const u32 *vw = st->verts + fg->vert0;
        const u8 *g = fg->group, *src = fg->src, *pk = fg->packets;
        int pi = fg->poly0;
        for (int t = 0; t < 8; ++t) {
            const int nv = t & 1 ? 3 : 4, len = fm_size[t] / 4 - 1;
            for (int k = *(const u16 *)(g + 2 + 2 * t); k > 0; --k, src += mesh_size[t], pk += fm_size[t], ++pi) {
                if (!(st->front[pi >> 5] >> (pi & 31) & 1)) continue;
                u32 vi[4], rel = 0;
                int bad = 0;
                for (int i = 0; i < nv; ++i) {
                    vi[i] = *(const u16 *)(src + 2 * i);
                    bad |= vi[i] >= fg->verts;
                    if (!bad) rel += vw[vi[i]] >> 22;
                }
                int ep = e0 + (int)(rel / (u32)nv); /* (the game's: the vertices' mean depth) */
                if (bad || ep < e || ep >= next) continue;
                unsigned int w[12];
                for (int i = 0; i < len; ++i) w[i] = *(const u32 *)(pk + 4 + 4 * i);
                for (int i = 0; i < nv; ++i) {
                    u32 vv = vw[vi[i]];
                    w[(fm_xy_at[t][i] - 4) / 4] = (vv & 0x7FF) | (vv >> 11 & 0x7FF) << 16 | (vv & 0x400 ? 0xF800 : 0) |
                                                  (vv & 0x200000 ? 0xF8000000 : 0);
                }
                gp0_run_plain(w, (unsigned)len);
            }
        }
    }
    gp0_backend = saved_backend;
    gp0_state = saved;
}

/* the scene's field models, before its primitives (depth tested: any order) */
PORT_OVERLAY_CODE(0a) __attribute__((target("arm"))) static void fm_emit(const Buffer *b, const FStore *fst, EmitState *es) {
    FmCtx c;
    c.fst = fst; c.last_use = 0; c.ofs_x = b->ofs_x; c.ofs_y = b->ofs_y;
    for (int u = 0; u < FM_USES; ++u) { c.key[u] = ~0u; c.image[u] = 0; }
    for (int u = 0; u < fst->nuses; ++u) {
        const FUse *fu = &fst->uses[u];
        const Region *r = fu->group == 0xFF ? NULL : groups[fu->group].region;
        int depth = (fu->tpage >> 7) & 3;
        if (depth == 3) depth = 2;
        c.key[u] = (u32)fu->tpage << 16 | fu->clut;
        if (!r || (depth < 2 && fu->palette == 0xFF)) continue;
        c.tu[u] = (s16)((r->x - (fu->tpage & 15) * 64) * texels_per_halfword(depth));
        c.tv[u] = (s16)(r->y - ((fu->tpage >> 4) & 1) * 256);
        c.image[u] = (u32)(r->unit * TEX_UNIT / 8) | r->size_bits | (u32)(depth == 2 ? 7 : depth == 0 ? 3 : 4) << 26;
        c.palette[u] = ~0u;
        if (depth < 2) {
            c.image[u] |= index0_transparent(fu->palette);
            c.palette[u] = (u32)fu->palette * 512 / 16;
        }
    }
    u32 *wp = es->wp;
    for (int gi = 0; gi < fst->ngroups; ++gi) {
        const FGroup *fg = &fst->groups[gi];
        if (!fm_live(fg)) continue;
        const u8 *g = fg->group, *src = fg->src, *pk = fg->packets;
        int pi = fg->poly0;
        for (int t = 0; t < 8; ++t) {
            int count = *(const u16 *)(g + 2 + 2 * t);
            if (!count) continue;
            switch (t) {
            case 0: wp = fm_kind(&c, es, wp, 0, fg, src, pk, pi, count); break;
            case 1: wp = fm_kind(&c, es, wp, 1, fg, src, pk, pi, count); break;
            case 2: wp = fm_kind(&c, es, wp, 2, fg, src, pk, pi, count); break;
            case 3: wp = fm_kind(&c, es, wp, 3, fg, src, pk, pi, count); break;
            case 4: wp = fm_kind(&c, es, wp, 4, fg, src, pk, pi, count); break;
            case 5: wp = fm_kind(&c, es, wp, 5, fg, src, pk, pi, count); break;
            case 6: wp = fm_kind(&c, es, wp, 6, fg, src, pk, pi, count); break;
            default: wp = fm_kind(&c, es, wp, 7, fg, src, pk, pi, count); break;
            }
            src += count * mesh_size[t]; pk += count * fm_size[t]; pi += count;
        }
    }
    es->wp = wp;
}

/* the scene's models' texture pages and CLUTs with their texels' bounds (plan_textures plans each): the drawn
 * polygons' (those culled when taken aren't) */
PORT_OVERLAY_CODE(0a) __attribute__((target("arm"))) static void fm_uses(FStore *st) {
    st->nuses = 0;
    int last = 0;
    for (int gi = 0; gi < st->ngroups; ++gi) {
        if (!fm_live(&st->groups[gi])) continue;
        const u8 *g = st->groups[gi].group, *pk = st->groups[gi].packets;
        int pi = st->groups[gi].poly0;
        for (int t = 0; t < 4; ++t) { /* (the textured kinds come first) */
            const int nv = t & 1 ? 3 : 4;
            for (int k = *(const u16 *)(g + 2 + 2 * t); k > 0; --k, pk += fm_size[t], ++pi) {
                if (!(st->front[pi >> 5] >> (pi & 31) & 1)) continue;
                u32 uv[4];
                for (int i = 0; i < nv; ++i) uv[i] = *(const u32 *)(pk + fm_xy_at[t][i] + 4);
                u16 clut = (u16)(uv[0] >> 16), tpage = (u16)(uv[1] >> 16);
                int u = last;
                if (u >= st->nuses || st->uses[u].tpage != tpage || st->uses[u].clut != clut)
                    for (u = 0; u < st->nuses; ++u) if (st->uses[u].tpage == tpage && st->uses[u].clut == clut) break;
                if (u == st->nuses) {
                    if (u == FM_USES) continue;
                    st->uses[u] = (FUse){tpage, clut, 255, 255, 0, 0, 0xFF, 0xFF};
                    st->nuses++;
                }
                last = u;
                FUse *fu = &st->uses[u];
                for (int i = 0; i < nv; ++i) {
                    u8 cu = (u8)uv[i], cv = (u8)(uv[i] >> 8);
                    if (cu < fu->u0) fu->u0 = cu;
                    if (cu > fu->u1) fu->u1 = cu;
                    if (cv < fu->v0) fu->v0 = cv;
                    if (cv > fu->v1) fu->v1 = cv;
                }
            }
        }
    }
}

static void use_place(const Place *p) {
    place = p;
    for (int i = 0; i < 2; ++i) buffers[i].prims = (Prim *)(psx_ram + p->at + p->prims_at[i]);
    prims_max = (int)p->max_prims;
    uintptr_t at;
    unsigned int size;
    if (p->field && port_ovl_native_extent(p->overlay, &at, &size)) {
        uintptr_t tail = (at + size + 31) & ~(uintptr_t)31;
        int fit = ((uintptr_t)__psxovl_slot0_end - tail) / sizeof(Prim), first = (OVL0A_CODE_END - p->prims_at[0]) / sizeof(Prim);
        if (fit > first) fit = first;
        if (fit > MAX_PRIMS) fit = MAX_PRIMS;
        /* (with the models native a scene records about half as many: the rest of the range holds their vertices) */
        if (fit > FIELD_PRIMS_NATIVE) fit = FIELD_PRIMS_NATIVE;
        /* the first buffer leaves room for the block runs' tables after it */
        int room = (int)((OVL0A_CODE_END - p->prims_at[0] - RUN_TABLES) / (sizeof(Prim) + PRIM_TABLES));
        if (fit > room) fit = room;
        if (fit > prims_max) {
            buffers[1].prims = (Prim *)tail;
            prims_max = fit;
        }
    }
    for (int i = 0; i < 2; ++i) {
        buffers[i].pool = NULL; buffers[i].ranks = NULL; buffers[i].meshes = 0; buffers[i].pool_used = 0;
        buffers[i].slots = NULL;
        if (p->world) {
            buffers[i].slots = (u16 *)(psx_ram + p->at + OVL0C_SLOTS_AT) + i * OVL0C_PRIMS;
            continue;
        }
        if (p->field) continue;
        buffers[i].pool = (u8 *)(buffers[i].prims + BATTLE_PRIMS); /* (MAX_PRIMS fit: the asserts above) */
        buffers[i].ranks = (u16 *)(psx_ram + p->at + OVL0B_RANKS_AT) + i * RANK_BUCKETS;
        buffers[i].last_bucket = RANK_BUCKETS;
    }
    for (int i = 0; i < 3; ++i) {
        terrain_ring[i] = p->world ? (struct Terrain *)(psx_ram + p->at + OVL0C_TERRAIN_AT + i * OVL0C_TERRAIN_BYTES) : NULL;
        if (terrain_ring[i]) terrain_reset(terrain_ring[i]);
    }
    rows = NULL; runs = NULL; needs = NULL;
    u8 *tables;
    if (p->world) {
        tables = psx_ram + p->at + OVL0C_PRIMTAB_AT;
    } else if (!p->field) {
        tables = psx_ram + p->at + OVL0B_PRIMTAB_AT;
    } else if (buffers[1].prims != (Prim *)(psx_ram + p->at + p->prims_at[1])) {
        u8 *t = (u8 *)(((uintptr_t)(buffers[0].prims + prims_max) + 31) & ~(uintptr_t)31);
        rows = (Row *)t; runs = (Run *)(t + RUN_ROWS * sizeof(Row)); needs = (Need *)((u8 *)runs + MAX_RUNS * sizeof(Run));
        tables = t + RUN_TABLES;
    } else {
        tables = (u8 *)(buffers[1].prims + prims_max);
    }
    native_heads = p->field || p->world ? NULL : (u16 *)(psx_ram + p->at + OVL0B_HEADS_AT);
    prim_group = tables; prim_palette = tables + prims_max; prim_drop = tables + 2 * prims_max;
    prim_layer = tables + 3 * prims_max;
#ifdef PORT_FIELD_GX
    /* fields: the primitives' depths (note_depth) after the tables, where ovl_0a's code range has room (when the
     * second buffer is at slot 0's free end); else the scenes stay in drawing order */
    for (int i = 0; i < 3; ++i) fm_ring[i] = NULL;
    if (p->field) {
        u8 *x = (u8 *)(((uintptr_t)(tables + PRIM_TABLES * prims_max) + 3) & ~(uintptr_t)3);
        u8 *end = psx_ram + p->at + OVL0A_CODE_END;
        if (x + 2u * prims_max * sizeof(u16) <= end) {
            for (int i = 0; i < 2; ++i) buffers[i].slots = (u16 *)x + i * prims_max;
            /* and the native models' ring with the room left (4 bytes a vertex) */
            u8 *m = (u8 *)(((uintptr_t)(x + 2u * prims_max * sizeof(u16)) + 3) & ~(uintptr_t)3);
            int cap = end > m + 3 * sizeof(FStore) ? (int)((end - m - 3 * sizeof(FStore)) / (3 * 4)) : 0;
            if (cap >= 256) {
                fm_cap = cap;
                u32 *v = (u32 *)(m + 3 * sizeof(FStore));
                for (int i = 0; i < 3; ++i) {
                    fm_ring[i] = (FStore *)m + i;
                    fm_ring[i]->gen = ~0u; fm_ring[i]->ngroups = 0;
                    fm_ring[i]->verts = v;
                    v += cap;
                }
            }
        }
    }
#endif
}

/* The textures' bookkeeping from scratch (the block runs' tables were overwritten: gxh_ram_overwritten) */
static void textures_reset(void) {
    for (int i = 0; i < MAX_REGIONS; ++i) if (regions[i].used) free_region(&regions[i]);
    runs_reset();
    memset(unit_used, 0, sizeof unit_used);
    unit_used[RAMP_UNIT] = 1;
    if (bank_a_bg) memset(unit_used, 1, 64); /* (bank A is a background) */
    field_runs = 0;
}

static void spare_buffers(void);
void gxh_init(void) {
    use_place(&places[0]);
    spare_buffers();
    static u32 main_chunks[2][CHUNK_WORDS] __attribute__((aligned(32)));
    chunks = main_chunks;
    marks = psx_spare_alloc(1024 * 512 / 8); /* in main's MIPS code range, see psx_spare_alloc */
    for (int i = 0; i < 2; ++i) ui_list[i] = psx_spare_alloc(UI_MAX * sizeof(UiPrim));
    memset(marks, 0, 1024 * 512 / 8);
}

int gxh_active(void) { return active; }

void gxh_enable(int on) {
    if (on == active) return;
    const Place *p = NULL; /* its primitives need the resident mode overlay's code range */
    for (unsigned int i = 0; i < sizeof places / sizeof places[0]; ++i)
        if (psx_ovl_resident[0] == places[i].overlay) p = &places[i];
    if (on && !p) return;
    if (on) use_place(p);
    active = on;
    if (on) {
        memset(block_shown, 0, sizeof block_shown);
        memset(block_hash, 0, sizeof block_hash);
        last_shown = NULL;
        videoSetMode(MODE_5_3D);
        vramSetPrimaryBanks(VRAM_A_TEXTURE, VRAM_B_TEXTURE, VRAM_C_TEXTURE, VRAM_D_TEXTURE);
        bank_a_bg = 0;
        bank_a_add = 0; /* (back_mode 0) */
        REG_BLDCNT = place->field ? BLEND_ALPHA | BLEND_SRC_BG0 | BLEND_DST_BG3 : 0;
        REG_BLDALPHA = place->field ? 16 : 0;
        soft_hold = 0;
        direct_soft = 0;
        vramSetBankE(VRAM_E_MAIN_BG);
        vramSetBankF(VRAM_F_TEX_PALETTE_SLOT0);
        vramSetBankG(VRAM_G_TEX_PALETTE_SLOT1);
        layer_bg = bgInit(2, BgType_Bmp8, BgSize_B8_256x256, 0, 0);
        bgSetRotateScale(layer_bg, 0, 256, 256);
        bgSetPriority(layer_bg, 0);
        bgUpdate();
        memset((void *)BG_BMP_RAM(0), 0, 256 * 256);
        if (place->field) {
            back_bg = bgInit(3, BgType_Bmp16, BgSize_B16_256x256, 8, 0);
            bgSetPriority(back_bg, 3);
            bgHide(back_bg);
        } else if (back_bg >= 0) {
            bgHide(back_bg);
        }
        reset_colours();
        glInit();
        glEnable(GL_TEXTURE_2D | GL_BLEND);
        glClearPolyID(63);
        glClearDepth(GL_MAX_DEPTH);
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        bgSetPriority(0, 1);
        memset(regions, 0, sizeof regions);
        memset(cpal_users, 0, sizeof cpal_users);
        runs_reset();
        field_runs = 0;
        memset(unit_used, 0, sizeof unit_used);
        unit_used[RAMP_UNIT] = 1;
        ramp_written = 0;
        memset(palettes, 0, sizeof palettes);
        for (int i = 0; i < 2; ++i) { buffers[i].valid = 0; buffers[i].count = 0; }
        ++buffers_version;
        gp0_backend = &hybrid_backend;
    } else {
        if (layer_bg >= 0) bgHide(layer_bg);
        if (back_bg >= 0) bgHide(back_bg);
        gp0_backend = &psx_softgpu;
    }
}

/* Field: the background layer (bank A, 16-bit with bit 15 set) from b's PS1 VRAM, scaled like the software display */
static void scale_back_rows(Buffer *b, int display_h, int oy0, int oy1);
static void scale_back(Buffer *b, int display_h) { scale_back_rows(b, display_h, 0, 1 << 30); }
/* output rows [oy0, oy1) only (in 32-row bands) */
static void scale_back_rows(Buffer *b, int display_h, int oy0, int oy1) {
    bgc.valid = 0; /* (bank A's bitmap is now the software frame's) */
    int h = display_h > 0 ? display_h : 224;
    int out_h = h * 4 / 5, top = (192 - out_h) / 2;
    u16 *band = scaled_block(), *bitmap = (u16 *)BG_BMP_RAM(8);
    if (oy1 > out_h) oy1 = out_h;
    for (int oy = oy0 & ~(BLOCK - 1); oy < oy1; oy += BLOCK) {
        int rows = out_h - oy < BLOCK ? out_h - oy : BLOCK, sy = b->y + oy * 5 / 4;
        scale_fn(band, b->x, sy, 320, rows * 5 / 4);
        /* opaque (bit 15) in cached RAM, then DMA: the ARM9's own stores to VRAM are slow */
        u32 *words = (u32 *)band;
        for (int k = 0; k < rows * 128; ++k) words[k] |= 0x80008000u;
        DC_FlushRange(band, rows * 512);
        dmaCopyWords(3, band, bitmap + (top + oy) * 256, rows * 512);
    }
}

/* Field frames: 3D, or software. A 3D frame is everything but the UI pages' primitives (the 2D layer, replayed in
 * software over the key colour), with all four texture banks. When a scene went software (go_software), or its
 * textures don't fit, the frame is the software renderer's: the whole scene in PS1 VRAM, scaled into bank A as a 16-bit
 * bitmap BG (the 3D layer transparent, the 2D layer hidden); bank A's textures are given up until a 3D frame takes it
 * back. After textures didn't fit, the next scenes stay software for a while (a field's textures stay as big). */
static void back_mode(int add);
static void bank_a_as_bg(int bg) {
    if (bg == bank_a_bg) return;
    bank_a_bg = bg;
    bgc.valid = 0;
    if (bg) {
        for (int i = 0; i < MAX_REGIONS; ++i)
            if (regions[i].used && regions[i].unit < 64) free_region(&regions[i]);
        if (runs)
            for (int i = 0; i < MAX_RUNS; ++i) if (runs[i].flags && rows[runs[i].row].unit < 64) free_run(i);
        for (int u = 0; u < 64; ++u) unit_used[u] = 1; /* not textures until a 3D frame */
        vramSetBankA(VRAM_A_MAIN_BG_0x06020000);
        dmaFillWords(0, BG_BMP_RAM(8), 256 * 192 * 2); /* the letterbox rows scale_back leaves: not old texels */
        bgShow(back_bg);
    } else {
        bgHide(back_bg);
        back_mode(0);
        vramSetBankA(VRAM_A_TEXTURE_SLOT0);
        for (int u = 0; u < 64; ++u) unit_used[u] = 0;
    }
}

/* Bank A's bitmap behind a transparent 3D layer (a software frame) or in front of it, added (BLDCNT with both
 * coefficients 16: A + B, saturating, as the PS1 adds) */
static void back_mode(int add) {
    if (add == bank_a_add) return;
    bank_a_add = add;
    bgSetPriority(back_bg, add ? 0 : 3); /* (in front of the 3D layer, behind the 2D layer: BG2 wins a tie) */
    /* behind: the 3D layer's translucent pixels over the bitmap by their own alpha (the background composite; a
     * software frame's 3D layer is all transparent) */
    REG_BLDCNT = add ? BLEND_ALPHA | BLEND_SRC_BG3 | BLEND_DST_BG0 : BLEND_ALPHA | BLEND_SRC_BG0 | BLEND_DST_BG3;
    REG_BLDALPHA = add ? 16 | 16 << 8 : 16;
}

/* ---- the field's background composite ------------------------------------------------------------------------
 * A field's opaque background sprites (most of a scene's primitives) drawn in software, in the PS1's order, into bank
 * A's bitmap behind the transparent 3D layer, again only when they or the texture pages under them change. Those
 * farther than every other 3D primitive and model vertex leave the 3D scene (L_BACK); the rest are drawn there too,
 * over the same pixels, so they still cover what goes behind them (per pixel, as the 3D scene's depths do). */

static u32 bgc_hash(const Buffer *b, int display_h) {
    u32 h = 2166136261u ^ (u32)display_h ^ b->clear << 12; /* (not the buffer's place: the sprites are relative) */
    for (int k = 0; k < b->count; ++k) {
        const Prim *p = &b->prims[k];
        if (!bg_prim(p)) continue;
        const u32 *w = (const u32 *)p;
        h = (h ^ w[0]) * 16777619u; h = (h ^ w[1]) * 16777619u; h = (h ^ w[2]) * 16777619u;
        h = (h ^ w[3]) * 16777619u; h = (h ^ w[5]) * 16777619u; h = (h ^ w[7]) * 16777619u;
        h = (h ^ w[10]) * 16777619u; h = (h ^ w[11]) * 16777619u;
    }
    return h;
}

/* the scene's PS1 frame buffer into bank A, a PS1 pixel per DS pixel at x * 5 / 4 (as the 3D scene samples them) */
PORT_OVERLAY_CODE(0a) __attribute__((target("arm"))) static void bgc_scale(const Buffer *b, int display_h) {
    int h = display_h > 0 ? display_h : 224, out_h = h * 4 / 5, top = (192 - out_h) / 2;
    u16 *band = scaled_block(), *bitmap = (u16 *)BG_BMP_RAM(8);
    for (int oy = 0; oy < out_h; oy += BLOCK) {
        int rows = out_h - oy < BLOCK ? out_h - oy : BLOCK;
        for (int r = 0; r < rows; ++r) {
            const u16 *src = psx_vram + ((b->y + ((5 * (oy + r)) >> 2)) & 511) * 1024 + b->x;
            u32 *out = (u32 *)(band + r * 256);
            for (int dx = 0; dx < 256; dx += 4, src += 5) { /* (x * 5 / 4: 0, 1, 2, 3 of each 5) */
                out[0] = (src[0] | (u32)src[1] << 16) | 0x80008000u;
                out[1] = (src[2] | (u32)src[3] << 16) | 0x80008000u;
                out += 2;
            }
        }
        DC_FlushRange(band, rows * 512);
        dmaCopyWords(3, band, bitmap + (top + oy) * 256, rows * 512);
    }
}

/* every background sprite drawn by the software renderer into the scene's frame buffer (the PS1's own drawing, in its
 * order), scaled into bank A; the frame buffer then its clear value again */
static void bgc_compose(Buffer *b, int display_h) {
    fill_buffer(b, b->clear == FIELD_KEY ? 0 : b->clear);
    replay(b, 0, b->count, -2);
    bgc_scale(b, display_h);
    fill_buffer(b, b->clear); /* (the key, or the game's clear: the 2D layer's "nothing") */
    dirty_cells(b->x, b->y, b->x + b->w - 1, b->y + b->h - 1);
}

/* The composite for this scene (0: the background stays in the 3D scene): drawn again when it changed; the far
 * sprites marked L_BACK, *polys and *vertices less theirs */
static int bgc_use(Buffer *b, const FStore *fst, int display_h, int *polys, int *vertices) {
    if (!ovl0a_resident() || !b->slots) return 0;
    /* per 16-pixel cell, the farthest 3D primitive or model group over it (the larger the depth, the nearer): a
     * background sprite farther than all of them in its cells covers none (the buffer's coordinates) */
    static u16 cell_far[16][24];
    memset(cell_far, 0xFF, sizeof cell_far);
    int any = 0;
#define BGC_MARK(X0, Y0, X1, Y1, D) do { \
        int cx0 = (X0) < 0 ? 0 : (X0) >> 4, cy0 = (Y0) < 0 ? 0 : (Y0) >> 4, cx1 = (X1) >> 4, cy1 = (Y1) >> 4; \
        if (cx1 > 23) cx1 = 23; \
        if (cy1 > 15) cy1 = 15; \
        for (int cy = cy0; cy <= cy1; ++cy) for (int cx = cx0; cx <= cx1; ++cx) if ((D) < cell_far[cy][cx]) cell_far[cy][cx] = (u16)(D); \
    } while (0)
    for (int k = 0; k < b->count; ++k)
        if (prim_layer[k] == L_3D) {
            const Prim *p = &b->prims[k];
            if (bg_prim(p)) { any = 1; continue; }
            int x0 = p->x[0], x1 = x0, y0 = p->y[0], y1 = y0;
            for (int i = 1; i < p->n; ++i) {
                if (p->x[i] < x0) x0 = p->x[i];
                if (p->x[i] > x1) x1 = p->x[i];
                if (p->y[i] < y0) y0 = p->y[i];
                if (p->y[i] > y1) y1 = p->y[i];
            }
            if (x1 < 0 || y1 < 0) continue;
            BGC_MARK(x0, y0, x1, y1, b->slots[k]);
        }
    if (!any) return 0;
    if (fst)
        for (int gi = 0; gi < fst->ngroups; ++gi) { /* (each group's screen bounds from its vertices) */
            const FGroup *fg = &fst->groups[gi];
            const u32 *vw = fst->verts + fg->vert0;
            int d = fg->depth_far < 0 ? 0 : fg->depth_far, x0 = 0x7FFF, y0 = 0x7FFF, x1 = -0x8000, y1 = -0x8000;
            for (int i = 0; i < fg->verts; ++i) {
                int x = (s32)(vw[i] << 21) >> 21, y = (s32)(vw[i] << 10) >> 21;
                if (x < x0) x0 = x;
                if (x > x1) x1 = x;
                if (y < y0) y0 = y;
                if (y > y1) y1 = y;
            }
            x0 += b->ofs_x; x1 += b->ofs_x; y0 += b->ofs_y; y1 += b->ofs_y;
            if (x1 < 0 || y1 < 0) continue;
            BGC_MARK(x0, y0, x1, y1, d);
        }
#undef BGC_MARK
    /* the sprites behind everything in their cells: only worth it if the rest then fits the 3D engine */
    int back_polys = 0, back_vertices = 0;
    for (int k = 0; k < b->count; ++k) {
        const Prim *p = &b->prims[k];
        if (prim_layer[k] != L_3D || !bg_prim(p)) continue;
        int x0 = p->x[0] < 0 ? 0 : p->x[0] >> 4, x1 = (p->x[1] - 1) >> 4, y0 = p->y[0] < 0 ? 0 : p->y[0] >> 4;
        int y1 = (p->y[2] - 1) >> 4, far = 0xFFFF;
        if (x1 > 23) x1 = 23;
        if (y1 > 15) y1 = 15;
        for (int cy = y0; cy <= y1; ++cy) for (int cx = x0; cx <= x1; ++cx) if (cell_far[cy][cx] < far) far = cell_far[cy][cx];
        if (b->slots[k] < far) { prim_layer[k] = L_BACK; back_polys++; back_vertices += p->n; }
    }
    if (*polys - back_polys > MAX_POLYS || *vertices - back_vertices > MAX_VERTICES) {
        for (int k = 0; k < b->count; ++k) if (prim_layer[k] == L_BACK) prim_layer[k] = L_3D;
        return 0;
    }
    *polys -= back_polys;
    *vertices -= back_vertices;
    u32 hash = bgc_hash(b, display_h);
    bank_a_as_bg(1);
    back_mode(0);
    if (!bgc.valid || bgc.hash != hash || bgc.tex != bgc_tex) {
        bgc_compose(b, display_h);
        bgc.valid = 1; bgc.hash = hash; bgc.tex = bgc_tex;
        gxh_stats_bgc++;
    }
    return 1;
}

static void field_frame(Buffer *b, int display_h) {
    if (!b->soft && !soft_hold) {
        int polys = 0, vertices = 0, adds = 0, add_area2 = 0;
        for (int k = 0; k < b->count; ++k) {
            const Prim *p = &b->prims[k];
            prim_layer[k] = p->flags & P_UI ? L_UI : (p->flags & P_SEMI) && (p->abr == 1 || p->abr == 3) ? L_ADD : L_3D;
            if (prim_layer[k] == L_3D) { polys++; vertices += p->n; }
            if (prim_layer[k] == L_ADD) { adds++; add_area2 += prim_area2(p); }
        }
        FStore *fst = scene_models(b); /* the native models (those facing the camera) */
        if (fst) { fm_uses(fst); polys += fst->front_polys; vertices += fst->front_verts; }
        /* the exact layer costs software drawing in proportion to the additive primitives' area (a sixth of the
         * screen: field 2162's lamp glows took it from 27 to 40 ms on DSi): beyond that, the 3D approximation */
        if (adds && add_area2 > 2 * 320 * 224 / 6) {
            for (int k = 0; k < b->count; ++k) if (prim_layer[k] == L_ADD) { prim_layer[k] = L_3D; polys++; vertices += b->prims[k].n; }
            adds = 0;
        }
        /* the background composite in bank A for a scene past the 3D engine's polygons or vertices (it would go
         * software), when the additive layer doesn't need bank A; kept a while (a scene near the limit would flip
         * bank A between textures and the bitmap) */
        static int bgc_keep;
        if (polys + adds > MAX_POLYS || vertices + adds * 4 > MAX_VERTICES) bgc_keep = 60;
        else if (bgc_keep) --bgc_keep;
        int composite = !adds && bgc_keep && bgc_use(b, fst, display_h, &polys, &vertices);
        /* the 3D engine's polygon and vertex RAM (2048, 6144 a frame): a scene past them stays exact in software */
        if (polys + adds > MAX_POLYS || vertices + adds * 4 > MAX_VERTICES) {
            gxh_stats_soft |= 8;
        } else {
            /* additive primitives (PS1 modes 1 and 3): exact in software onto black in bank A, added over the 3D
             * layer by the 2D compositor, if the textures fit in banks B-D; else approximated in 3D (conv_upload) */
            if (adds) bank_a_as_bg(1);
            else if (bank_a_bg && !composite) bank_a_as_bg(0);
            plan_textures(b);
            if (adds && gxh_stats_unplaced) {
                for (int k = 0; k < b->count; ++k) if (prim_layer[k] == L_ADD) prim_layer[k] = L_3D;
                adds = 0;
                bank_a_as_bg(0);
                plan_textures(b);
            }
            if (!gxh_stats_unplaced && adds) {
                /* only the rows the additive primitives cover (and those the last scene's covered, cleared) */
                int y0 = b->h, y1 = 0;
                for (int k = 0; k < b->count; ++k) {
                    if (prim_layer[k] != L_ADD) continue;
                    const Prim *p = &b->prims[k];
                    for (int i = 0; i < p->n; ++i) {
                        if (p->y[i] < y0) y0 = p->y[i];
                        if (p->y[i] > y1) y1 = p->y[i];
                    }
                }
                if (y0 < 0) y0 = 0;
                if (y1 >= b->h) y1 = b->h - 1;
                if (y0 > y1) y0 = y1 = 0;
                int h = display_h > 0 ? display_h : 224, top = (192 - h * 4 / 5) / 2;
                int oy0 = y0 * 4 / 5 & ~(BLOCK - 1), oy1 = ((y1 + 1) * 4 / 5 + BLOCK) & ~(BLOCK - 1);
                static int last_oy0, last_oy1;
                for (int oy = last_oy0; oy < last_oy1; ++oy) /* the last scene's band outside this one: clear */
                    if ((oy < oy0 || oy >= oy1) && top + oy < 192)
                        dmaFillWords(0, (u16 *)BG_BMP_RAM(8) + (top + oy) * 256, 512);
                last_oy0 = oy0; last_oy1 = oy1;
                int ry0 = oy0 * 5 / 4, ry1 = oy1 * 5 / 4 + 2; /* the buffer rows those bands sample */
                if (ry1 > b->h) ry1 = b->h;
                Buffer band = *b;
                band.y = b->y + ry0; band.h = ry1 - ry0;
                fill_buffer(&band, 0);
                replay(b, 0, b->count, L_ADD);
                scale_back_rows(b, display_h, oy0, oy1);
                fill_buffer(&band, b->clear); /* as it was: the key, or the game's clear (the 2D layer's "nothing") */
                back_mode(1);
                gxh_stats_soft |= 512;
            }
            if (!gxh_stats_unplaced) {
                replay(b, 0, b->count, L_UI);
                for (int k = 0; k < b->count; ++k) { /* where the UI landed: the 2D layer's other cells are clear */
                    if (prim_layer[k] != L_UI) continue;
                    const Prim *p = &b->prims[k];
                    int x0 = p->x[0], x1 = x0, y0 = p->y[0], y1 = y0;
                    for (int i = 1; i < p->n; ++i) {
                        if (p->x[i] < x0) x0 = p->x[i];
                        if (p->x[i] > x1) x1 = p->x[i];
                        if (p->y[i] < y0) y0 = p->y[i];
                        if (p->y[i] > y1) y1 = p->y[i];
                    }
                    dirty_cells(b->x + x0, b->y + y0, b->x + x1, b->y + y1);
                }
                return;
            }
            gxh_stats_soft |= 128;
        }
        soft_hold = 60;
    }
    if (!b->soft) { key_fill(b); go_software(b); }
    fm_soft_upto(b, FM_SOFT_ALL); /* (the models' polygons nearer than the last primitive) */
    /* hold software: the GPU straight to the software renderer until it ends (present_direct); not for a scene
     * that only filled its buffer (soft_hold 0): the next scene's models are already taken, for the hybrid */
    if (soft_hold) {
        direct_soft = 1;
        gp0_backend = &psx_softgpu;
    }
    bank_a_as_bg(1);
    back_mode(0);
    scale_back(b, display_h);
    memset(prim_layer, L_UI, b->count); /* (none left: the scene is all in PS1 VRAM) */
    plan_textures(b);
}

/* The 3D engine's fog for a world scene's terrain: the vertex pass's ramp, d = (SZ - 0x8000 + offset) >> 2 (>> 1 for
 * far blocks without the world's fog) over 4096, on the 15-bit fog depth. A terrain vertex at SZ is at
 * world_entry_depth(SZ / 8 + 8) = 16416 - SZ / 2 under glOrthof32(..., -32768, 32768), whose 15-bit depth is
 * (32768 - z) / 2 = 8176 + SZ / 4; the ramp starts there at SZ = 0x8000 - offset and spans 4096 (2048) of it:
 * 32 steps of 128 (64), FOG_SHIFT 3 (4). */
static void terrain_fog_setup(Buffer *b) {
    struct Terrain *t = scene_terrain(b);
    if (!t || !t->fog_mode) { glDisable(GL_FOG); return; }
    int shift = t->fog_mode == 2 ? 4 : 3;
    u32 c = t->fog_colour;
    glFogColor((c & 0xFF) >> 3, (c >> 8 & 0xFF) >> 3, (c >> 16 & 0xFF) >> 3, 31);
    glFogShift(shift);
    glFogOffset(8176 + (int)(0x8000 - t->fog_offset) / 4);
    for (int i = 0; i < 32; ++i) glFogDensity(i, i * 127 / 31);
    glEnable(GL_FOG);
}

/* Send a buffer's scene to the 3D engine; it is shown from the next frame on. */
static __attribute__((noinline)) void submit(Buffer *b, int display_h) {
    BB(0x47580001, b->count);
    unsigned int profile = port_profile_begin();
    while (dmaBusy(0)) {} /* the previous scene's list */
    if (place->field) field_frame(b, display_h);
    else { memset(prim_layer, L_3D, b->count); plan_textures(b); }
    gxh_stats_regions = gxh_stats_units = 0;
    for (int i = 0; i < MAX_REGIONS; ++i) if (regions[i].used) { gxh_stats_regions++; gxh_stats_units += regions[i].units; }
    if (runs) for (int i = 0; i < RUN_ROWS; ++i) gxh_stats_units += rows[i].fmt_units & 15;
    if (gxh_stats_units > gxh_stats_units_peak) gxh_stats_units_peak = gxh_stats_units;
    u32 saved = 0;
    if (place->field && bank_a_bg) { vramSetBankB(VRAM_B_LCD); vramSetBankC(VRAM_C_LCD); vramSetBankD(VRAM_D_LCD); }
    else saved = vramSetPrimaryBanks(VRAM_A_LCD, VRAM_B_LCD, VRAM_C_LCD, VRAM_D_LCD);
    vramSetBankF(VRAM_F_LCD);
    vramSetBankG(VRAM_G_LCD);
    BB(0x47580002, 0);
    upload();
    BB(0x47580003, 0);
    if (place->field && bank_a_bg) {
        vramSetBankB(VRAM_B_TEXTURE_SLOT1); vramSetBankC(VRAM_C_TEXTURE_SLOT2); vramSetBankD(VRAM_D_TEXTURE_SLOT3);
    } else vramRestorePrimaryBanks(saved);
    vramSetBankF(VRAM_F_TEX_PALETTE_SLOT0);
    vramSetBankG(VRAM_G_TEX_PALETTE_SLOT1);
    int h = display_h > 0 ? display_h : 224;
    int out_h = h * 4 / 5;
    glViewport(0, (192 - out_h) / 2, 255, (192 - out_h) / 2 + out_h - 1);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    /* v16 units: 1 = one PS1 pixel (as 1/4096); later primitives get larger z, nearer */
    /* (battle: depths up to Z_STEP x the scene's primitives and native polygons, emit_scene) */
    if (place->field && !b->slots) glOrthof32(0, 320, h, 0, -4096, 4096);
    else glOrthof32(0, 320, h, 0, -32768, 32768);
    terrain_fog_setup(b);
    glMatrixMode(GL_MODELVIEW);
    unsigned int c = b->clear;
    if (place->world) c = 0; /* (emit_scene draws its clear colour inside the viewport) */
    glClearColor(c & 31, (c >> 5) & 31, (c >> 10) & 31, place->field && bank_a_bg && !bank_a_add ? 0 : 31); /* a software frame shows */
    emit_scene(b);
    flush_chunk();
    b->submitted = 1;
    port_profile_detail_end(PORT_PROFILE_UPLOAD, profile); /* reported as gx_texture_upload */
}

/* 2D layer: the displayed buffer's 2D pixels on an 8-bit bitmap BG over the 3D layer. Polygons recorded for GX
 * never reach PS1 VRAM, so the buffer holds its scene's clearing fill plus 2D pixels: a pixel other than the clear
 * value is 2D (one exactly equal to it shows the 3D layer instead). Colours get palette entries as they appear
 * (index 0 is transparent); the battle UI uses far fewer than 255, so it is exact. When the palette fills, a
 * colour takes its nearest entry and the next frame rebuilds the layer with a fresh palette. 32x32 blocks are
 * rebuilt when their source cells (8x8 PS1 pixels) were drawn and their pixels hash differently (the battle UI
 * redraws every window each VBlank, mostly unchanged). */
static u64 cell_dirty[512 / 8][2]; /* a bit per 8x8 cell of PS1 VRAM: written since the 2D layer last looked */
#define CELL_DIRTY(y, x) (cell_dirty[(y) & 63][((x) & 127) >> 6] >> ((x) & 63) & 1)
static int last_x, last_y, last_w, last_h;
static u8 *colour_map;          /* 32768 RGB15 -> palette index (0: not mapped yet) */
/* The spare PS1 RAM's buffers all taken at start-up (gxh_init), so one that doesn't fit stops the port at boot
 * rather than in whichever scene first needs it */
static void spare_buffers(void) {
    colour_map = psx_spare_alloc(32768);
    (void)scaled_block();
}
static int colours, colours_full;

/* Palette: 1-64 a fixed 4x4x4 colour cube for software glows (gradients with many shades), 65-255 exact colours
 * as they appear (the battle UI uses under 200). */
#define CUBE 64
static const u8 cube_level[4] = {0, 10, 21, 31};

static void reset_colours(void) {
    memset(colour_map, 0, 32768);
    for (int i = 0; i < CUBE; ++i)
        BG_PALETTE[1 + i] = cube_level[i & 3] | cube_level[(i >> 2) & 3] << 5 | cube_level[i >> 4] << 10;
    colours = CUBE;
    colours_full = 0;
}

/* (v + 5) / 11: the nearest cube level of a 5-bit channel (no divide per pixel) */
static const u8 cube_step[32] = {0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 3, 3, 3, 3};

static inline u8 cube_of(u16 c) {
    return (u8)(1 + (cube_step[c & 31] | cube_step[(c >> 5) & 31] << 2 | cube_step[(c >> 10) & 31] << 4));
}

/* A colour's palette index: a new entry while there is room, else the nearest entry (remembered, so each colour
 * is searched once). Software glows are gradients with many shades; the battle UI alone uses under 200. */
static u8 colour_of(u16 c) {
    c &= 0x7FFF;
    u8 i = colour_map[c];
    if (i) return i;
    if (colours < 255) {
        i = (u8)++colours;
        BG_PALETTE[i] = c;
    } else {
        colours_full = 1;
        int best = 1, best_d = 1 << 30;
        for (int k = 1; k <= 255; ++k) {
            u16 p = BG_PALETTE[k];
            int dr = (p & 31) - (c & 31), dg = ((p >> 5) & 31) - ((c >> 5) & 31), db = ((p >> 10) & 31) - ((c >> 10) & 31);
            int dd = dr * dr + dg * dg + db * db;
            if (dd < best_d) { best_d = dd; best = k; }
        }
        i = (u8)best;
    }
    colour_map[c] = i;
    return i;
}

/* A pixel is on the 2D layer when it isn't the clear value, unless an additive glow left it near black (its edge
 * adds next to nothing; drawn opaque over the 3D layer it would be a dark fringe). */
static inline int shows(u16 c, u16 clear, u8 mark) {
    if (c == clear) return 0;
    if (mark && (c & 31) <= 3 && ((c >> 5) & 31) <= 3 && ((c >> 10) & 31) <= 3) return 0;
    return 1;
}

static void dirty_cells(int x0, int y0, int x1, int y1) {
    for (int y = y0 >> 3; y <= y1 >> 3; ++y)
        for (int x = x0 >> 3; x <= x1 >> 3; ++x) cell_dirty[y & 63][(x & 127) >> 6] |= 1ull << (x & 63);
    for (int i = 0; i < 2; ++i) { /* buffers start on 8-pixel columns and rows */
        Buffer *b = &buffers[i];
        int bx0 = (x0 - b->x) >> 3, bx1 = (x1 - b->x) >> 3, by0 = (y0 - b->y) >> 3, by1 = (y1 - b->y) >> 3;
        if (!b->valid || bx1 < 0 || by1 < 0 || bx0 > 63 || by0 > 63) continue;
        if (bx0 < 0) bx0 = 0;
        if (by0 < 0) by0 = 0;
        if (bx1 > 63) bx1 = 63;
        if (by1 > 63) by1 = 63;
        u64 bits = (bx1 == 63 ? ~0ull : (2ull << bx1) - 1) & ~((1ull << bx0) - 1);
        for (int y = by0; y <= by1; ++y) b->touched[y] |= bits;
    }
}

/* nothing that may differ from the clear value was written in PS1 rectangle (x, y, w, h) of buffer b */
static int untouched(const Buffer *b, int x, int y, int w, int h) {
    int bx0 = (x - b->x) >> 3, bx1 = (x + w - 1 - b->x) >> 3, by0 = (y - b->y) >> 3, by1 = (y + h - 1 - b->y) >> 3;
    if (bx0 < 0 || by0 < 0 || bx1 > 63 || by1 > 63) return 0;
    u64 bits = (bx1 == 63 ? ~0ull : (2ull << bx1) - 1) & ~((1ull << bx0) - 1);
    for (int r = by0; r <= by1; ++r) if (b->touched[r] & bits) return 0;
    return 1;
}

/* FNV-1a's prime, read at run time: as a constant the compiler multiplies by shifts and adds (9 instructions) */
static volatile const u32 fnv_prime = 16777619u;

/* Field, while textures don't fit (soft_hold): the GPU goes straight to the software renderer (the hybrid's per-
 * primitive bookkeeping costs 2 us a primitive), and each new frame shown is scaled into bank A; the hybrid takes the
 * primitives again when the hold ends. */
static void present_direct(const PsxDisplayInfo *d) {
    static int last_x = -1, last_y = -1;
    if (d->x == last_x && d->y == last_y) return; /* the same frame, shown again */
    last_x = d->x; last_y = d->y;
    Buffer *b = buffer_at(d->x, d->y);
    if (!b) return;
    bank_a_as_bg(1);
    back_mode(0);
    scale_back(b, d->h);
    if (soft_hold && --soft_hold) return;
    direct_soft = 0; /* the hybrid again: the buffers' next draws start scenes */
    gp0_backend = &hybrid_backend;
    for (int i = 0; i < 2; ++i) { buffers[i].shown = 1; buffers[i].soft = 1; buffers[i].submitted = 1; }
    last_x = last_y = -1;
}

void gxh_present(const PsxDisplayInfo *d, void (*scale)(u16 *dst, int x, int y, int w, int h)) {
    scale_fn = scale;
    if (direct_soft) {
        if (layer_bg >= 0) bgHide(layer_bg);
        present_direct(d);
        return;
    }
    ui_flush();
    Buffer *b = buffer_at(d->x, d->y);
    if (b) {
        b->shown = 1;
        scale_fn = scale;
        if (!b->submitted) submit(b, d->h);
    }
    if (place->field && layer_bg >= 0) {
        /* a scene that went software is all on the background layer */
        static int ui_hidden;
        int hide = b && b->soft;
        if (hide != ui_hidden) { if (hide) bgHide(layer_bg); else bgShow(layer_bg); ui_hidden = hide; }
        if (hide) return;
    }
    const u16 clear = b ? (u16)b->clear : 0;
    const u32 clear2 = clear | (u32)clear << 16;
    int w = d->w ? d->w : 320, h = d->h ? d->h : 224;
    if (w > 320) w = 320;
    if (h > 240) h = 240;
    int out_w = w * 4 / 5, out_h = h * 4 / 5, top = (192 - out_h) / 2;
    /* another buffer or area: every block's hash decides; a palette that filled starts over then (rebuilding
     * everything), so colours that left the screen give their entries back */
    int check_all = b != last_shown || d->x != last_x || d->y != last_y || w != last_w || h != last_h;
    /* (text coverage averages fill it with edge shades, which the nearest entries stand in for well: at most
     * one rebuild per 64 presents) */
    static unsigned presents, last_rebuild;
    ++presents;
    int rebuild = check_all && colours_full && presents - last_rebuild >= 64;
    if (rebuild) { reset_colours(); last_rebuild = presents; }
    last_shown = b; last_x = d->x; last_y = d->y; last_w = w; last_h = h;
    u16 *block = scaled_block();
    u32 memo_colour = 0x10000; /* none yet (colours are 15-bit plus STP): the palette may have been reset above */
    u8 memo_index = 0;
    u16 *bitmap = (u16 *)BG_BMP_RAM(0);
    int shown = 0;
    /* the shown buffer's untouched cells (a battle's reset only with the game's covering fill) */
    int field_scene = b && !b->soft && b->x == d->x && b->y == d->y;
    for (int by = 0, row = 0; by < out_h; by += BLOCK, ++row)
        for (int bx = 0, col = 0; bx < out_w; bx += BLOCK, ++col) {
            int sx0 = d->x + bx * 5 / 4, sy0 = d->y + by * 5 / 4;
            int bw = (bx + BLOCK < out_w ? BLOCK : out_w - bx), bh = (by + BLOCK < out_h ? BLOCK : out_h - by);
            int sw = bw * 5 / 4, sh = bh * 5 / 4;
            if (field_scene && untouched(b, sx0, sy0, sw, sh)) { /* the clear value only: nothing on the layer */
                if (block_shown[row][col] || rebuild) {
                    for (int oy = 0; oy < bh; ++oy) {
                        u16 *out = bitmap + ((top + by + oy) * 256 + bx) / 2;
                        for (int k = 0; k < bw / 2; ++k) out[k] = 0;
                    }
                    block_shown[row][col] = 0;
                    block_hash[row][col] = 0;
                }
                continue;
            }
            if (!check_all) {
                int dirty = 0;
                for (int y = sy0 >> 3; y <= (sy0 + sh) >> 3 && !dirty; ++y)
                    for (int x = sx0 >> 3; x <= (sx0 + sw) >> 3; ++x)
                        if (CELL_DIRTY(y, x)) { dirty = 1; break; }
                if (!dirty) { shown += block_shown[row][col]; continue; }
            }
            u32 any = 0, hash = 2166136261u, prime = fnv_prime;
            int hx0 = sx0 & ~1, hx1 = (sx0 + sw + 1) & ~1; /* whole words: a wider span only rebuilds more often */
            for (int y = sy0; y < sy0 + sh; ++y) {
                const u16 *vrow = psx_vram + (y & 511) * 1024;
                if (hx1 > 1024) {
                    for (int k = hx0; k < hx1; ++k) { u32 c = vrow[k & 1023]; any |= c ^ clear; hash = (hash ^ c) * prime; }
                    continue;
                }
                const u32 *words = (const u32 *)(vrow + hx0);
                for (int k = 0; k < (hx1 - hx0) / 2; ++k) { u32 c = words[k]; any |= c ^ clear2; hash = (hash ^ c) * prime; }
            }
            if (!rebuild && hash == block_hash[row][col] && block_shown[row][col] == !!any) {
                shown += block_shown[row][col];
                continue;
            }
            block_hash[row][col] = hash;
            block_shown[row][col] = !!any;
            if (any) scale(block, sx0, sy0, sw, sh);
            u16 col[BLOCK];
            for (int ox = 0; ox < bw; ++ox) col[ox] = (u16)((sx0 + ox * 5 / 4) & 1023);
            for (int oy = 0; oy < bh; ++oy) {
                u16 *out = bitmap + ((top + by + oy) * 256 + bx) / 2;
                if (!any) { for (int k = 0; k < bw / 2; ++k) out[k] = 0; continue; }
                const u16 *vrow = psx_vram + ((sy0 + oy * 5 / 4) & 511) * 1024;
                const u8 *mrow = marks + ((sy0 + oy * 5 / 4) & 511) * 128;
                const u16 *frow = block + oy * 256;
                for (int ox = 0; ox < bw; ox += 2) {
                    int a = col[ox], b2 = col[ox + 1 < bw ? ox + 1 : ox];
                    u8 ma = (mrow[a >> 3] >> (a & 7)) & 1, mb = (mrow[b2 >> 3] >> (b2 & 7)) & 1;
                    /* the UI's colours come in runs: the last one's index saves the 32 KB map's cache misses */
#define INDEX_OF(c) ((u16)(c) == memo_colour ? memo_index : (memo_colour = (u16)(c), memo_index = colour_of(memo_colour)))
                    u16 i0 = shows(vrow[a], clear, ma) ? ma ? cube_of(frow[ox]) : INDEX_OF(frow[ox]) : 0;
                    u16 i1 = ox + 1 < bw && shows(vrow[b2], clear, mb)
                                 ? mb ? cube_of(frow[ox + 1]) : INDEX_OF(frow[ox + 1]) : 0;
#undef INDEX_OF
                    out[ox / 2] = i0 | i1 << 8; /* halfword stores: VRAM ignores byte writes */
                }
            }
            shown += !!any;
        }
    for (int y = (d->y >> 3); y <= (d->y + h) >> 3; ++y)
        for (int x = (d->x >> 3); x <= (d->x + w) >> 3; ++x) cell_dirty[y & 63][(x & 127) >> 6] &= ~(1ull << (x & 63));
    gxh_stats_blocks = colours_full ? -colours : colours;
    (void)shown;
}
#endif
