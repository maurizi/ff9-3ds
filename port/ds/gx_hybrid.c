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
#include <nds.h>
#include <string.h>
#include <stdlib.h>

#include "gp0.h"
#include "psx_raster.h"
#include "psx_display.h"
#include "profile.h"
#include "gx_hybrid.h"

extern unsigned short *psx_vram;

#define MAX_PRIMS 2600   /* recorded; the hardware takes 2048 polygons and 6144 vertices a frame */
#define MAX_POLYS 2044
#define MAX_VERTICES 6140

int gxh_stats_polys, gxh_stats_dropped, gxh_stats_texture_fail, gxh_stats_blocks;
static int active;

/* ---- recorded primitives ----------------------------------------------------------------------------------- */

enum { P_TEXTURED = 1, P_RAW = 2, P_SEMI = 4, P_GOURAUD = 8 };

typedef struct {
    u8 n, flags, abr, depth;
    u16 tx, ty, clut_x, clut_y;
    s16 x[4], y[4];
    u8 r[4], g[4], b[4], u[4], v[4];
} Prim;

typedef struct {
    int x, y, w, h, valid;
    int shown;           /* displayed since its scene started: the next draw into it starts a new scene */
    Prim *prims;
    int count;
    unsigned int clear;  /* RGB15 of the scene's clearing fill */
    int submitted;       /* sent to the 3D engine since it last changed */
} Buffer;

static Buffer buffers[2];

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

static void note_buffer(int x, int y, int w, int h) {
    if (w < 256 || h < 192) return;
    for (int i = 0; i < 2; ++i)
        if (buffers[i].valid && buffers[i].x == x && buffers[i].y == y) {
            buffers[i].w = w; buffers[i].h = h;
            return;
        }
    Buffer *b = !buffers[0].valid ? &buffers[0] : !buffers[1].valid ? &buffers[1] : NULL;
    if (!b) b = displayed() == &buffers[0] ? &buffers[1] : &buffers[0]; /* a third area replaces the hidden one */
    b->x = x; b->y = y; b->w = w; b->h = h; b->valid = 1; b->shown = 1; b->count = 0; b->submitted = 0;
}

static void start_scene(Buffer *b) {
    b->shown = 0;
    b->count = 0;
    b->submitted = 0;
}

/* The buffer the current drawing area belongs to (NULL: offscreen), starting its scene if it was shown. */
static Buffer *target(void) {
    int x, y, w, h;
    psx_gpu_draw_area(&x, &y, &w, &h);
    note_buffer(x, y, w, h);
    Buffer *b = buffer_at(gp0_state.area_x0, gp0_state.area_y0);
    if (b && b->shown && b != displayed()) start_scene(b);
    return b;
}

static int triangle_area(const Gp0Vtx *a, const Gp0Vtx *b, const Gp0Vtx *c) {
    return (b->x - a->x) * (c->y - a->y) - (b->y - a->y) * (c->x - a->x);
}

static void record(Buffer *b, const Gp0Mode *m, const Gp0Vtx *v, int n) {
    if (b->count >= MAX_PRIMS) { gxh_stats_dropped++; return; }
    Prim *p = &b->prims[b->count++];
    p->n = (u8)n;
    p->flags = (u8)((m->textured ? P_TEXTURED : 0) | (m->raw ? P_RAW : 0) | (m->semi ? P_SEMI : 0) |
                    (m->gouraud ? P_GOURAUD : 0));
    p->abr = (u8)m->abr; p->depth = (u8)(m->depth > 2 ? 2 : m->depth);
    p->tx = (u16)m->tx; p->ty = (u16)m->ty; p->clut_x = (u16)m->clut_x; p->clut_y = (u16)m->clut_y;
    for (int i = 0; i < n; ++i) {
        p->x[i] = (s16)(v[i].x - b->x); p->y[i] = (s16)(v[i].y - b->y);
        p->r[i] = (u8)(v[i].r < 0 ? 0 : v[i].r > 255 ? 255 : v[i].r);
        p->g[i] = (u8)(v[i].g < 0 ? 0 : v[i].g > 255 ? 255 : v[i].g);
        p->b[i] = (u8)(v[i].b < 0 ? 0 : v[i].b > 255 ? 255 : v[i].b);
        p->u[i] = (u8)v[i].u; p->v[i] = (u8)v[i].v;
    }
}

/* Clip against the drawing area (the 3D engine only clips to the viewport). */
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
    int unit, units;
    int dirty, wanted;
} Region;

static Region regions[MAX_REGIONS];
static u8 unit_used[TEX_UNITS];

static inline int texels_per_halfword(int depth) { return depth == 0 ? 4 : depth == 1 ? 2 : 1; }

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

static void free_region(Region *r) {
    memset(unit_used + r->unit, 0, r->units);
    r->used = 0;
}

static Region *region_containing(int depth, int x, int y, int wh, int h) {
    for (int i = 0; i < MAX_REGIONS; ++i) {
        Region *r = &regions[i];
        if (r->used && r->depth == depth && x >= r->x && y >= r->y && x + wh <= r->x + r->wh && y + h <= r->y + r->h)
            return r;
    }
    return NULL;
}

static Region *place_region(int depth, int x, int y, int wh, int h) {
    int sw = log2_at_least(wh * texels_per_halfword(depth)), sh = log2_at_least(h);
    if (sw < 3) sw = 3;
    if (sh < 3) sh = 3;
    if (sw > 10 || sh > 10) return NULL;
    int bytes = ((1 << sw) << sh) * (depth == 0 ? 4 : depth == 1 ? 8 : 16) / 8;
    int units = 1 << log2_at_least((bytes + TEX_UNIT - 1) / TEX_UNIT);
    int unit = allocate_units(units);
    if (unit < 0) return NULL;
    for (int i = 0; i < MAX_REGIONS; ++i)
        if (!regions[i].used) {
            regions[i] = (Region){1, depth, x, y, wh, h, 1 << sw, 1 << sh, unit, units, 1, 1};
            return &regions[i];
        }
    memset(unit_used + unit, 0, units);
    return NULL;
}

/* Texture palettes: 64 slots of 256 colours in F-G (32 KB). */
#define PAL_SLOTS 64
typedef struct { int used, x, y, count, dirty, wanted; } Palette;
static Palette palettes[PAL_SLOTS];

void psx_vram_touch(int x, int y, int w, int h) {
    if (!active) return;
    for (int i = 0; i < MAX_REGIONS; ++i) {
        Region *r = &regions[i];
        if (r->used && x < r->x + r->wh && r->x < x + w && y < r->y + r->h && r->y < y + h) r->dirty = 1;
    }
    for (int i = 0; i < PAL_SLOTS; ++i) {
        Palette *p = &palettes[i];
        if (p->used && y <= p->y && p->y < y + h && x < p->x + p->count && p->x < x + w) p->dirty = 1;
    }
}

/* Per-scene texture plan: one group per (depth, page) with its texels' bounding box (halfwords, rows). */
#define MAX_GROUPS 96
typedef struct { int depth, tx, ty, x0, y0, x1, y1; Region *region; } Group;
static Group groups[MAX_GROUPS];
static int group_count;
static u8 prim_group[MAX_PRIMS];
static u8 prim_palette[MAX_PRIMS];

static int plan_textures(Buffer *b) {
    group_count = 0;
    for (int i = 0; i < MAX_REGIONS; ++i) regions[i].wanted = 0;
    for (int i = 0; i < PAL_SLOTS; ++i) palettes[i].wanted = 0;
    for (int k = 0; k < b->count; ++k) {
        Prim *p = &b->prims[k];
        prim_group[k] = 0xFF;
        if (!(p->flags & P_TEXTURED)) continue;
        int per = texels_per_halfword(p->depth);
        int u0 = p->u[0], u1 = u0, v0 = p->v[0], v1 = v0;
        for (int i = 1; i < p->n; ++i) {
            if (p->u[i] < u0) u0 = p->u[i];
            if (p->u[i] > u1) u1 = p->u[i];
            if (p->v[i] < v0) v0 = p->v[i];
            if (p->v[i] > v1) v1 = p->v[i];
        }
        int x0 = p->tx + u0 / per, x1 = p->tx + u1 / per, y0 = p->ty + v0, y1 = p->ty + v1;
        int g;
        for (g = 0; g < group_count; ++g)
            if (groups[g].depth == p->depth && groups[g].tx == p->tx && groups[g].ty == p->ty) break;
        if (g == group_count) {
            if (group_count == MAX_GROUPS) continue;
            groups[group_count++] = (Group){p->depth, p->tx, p->ty, x0, y0, x1, y1, NULL};
        } else {
            Group *q = &groups[g];
            if (x0 < q->x0) q->x0 = x0;
            if (x1 > q->x1) q->x1 = x1;
            if (y0 < q->y0) q->y0 = y0;
            if (y1 > q->y1) q->y1 = y1;
        }
        prim_group[k] = (u8)g;
        /* palette */
        prim_palette[k] = 0xFF;
        if (p->depth < 2) {
            int count = p->depth ? 256 : 16, s, empty = -1;
            for (s = 0; s < PAL_SLOTS; ++s) {
                Palette *q = &palettes[s];
                if (q->used && q->x == p->clut_x && q->y == p->clut_y && q->count >= count) break;
                if (!q->used && empty < 0) empty = s;
            }
            if (s == PAL_SLOTS) {
                if (empty < 0)
                    for (int t = 0; t < PAL_SLOTS; ++t) if (!palettes[t].wanted) { empty = t; break; }
                if (empty < 0) continue;
                s = empty;
                palettes[s] = (Palette){1, p->clut_x, p->clut_y, count, 1, 0};
            }
            palettes[s].wanted = 1;
            prim_palette[k] = (u8)s;
        }
    }
    /* Keep regions that already hold a group's texels; place the rest, largest first, after freeing the unwanted.
     * If that fails (fragmentation), start the texture memory over and place every group largest first. */
    int missing = 0;
    for (int g = 0; g < group_count; ++g) {
        Group *q = &groups[g];
        q->region = region_containing(q->depth, q->x0, q->y0, q->x1 - q->x0 + 1, q->y1 - q->y0 + 1);
        if (q->region) q->region->wanted = 1;
        else missing = 1;
    }
    if (!missing) return 1;
    for (int i = 0; i < MAX_REGIONS; ++i) if (regions[i].used && !regions[i].wanted) free_region(&regions[i]);
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
            q->region = region_containing(q->depth, q->x0, q->y0, wh, h);
            if (!q->region) q->region = place_region(q->depth, q->x0, q->y0, wh, h);
            if (q->region) q->region->wanted = 1;
            else failed = 1;
        }
        if (!failed || attempt) break;
        for (int g = 0; g < group_count; ++g) groups[g].region = NULL;
        for (int i = 0; i < MAX_REGIONS; ++i) if (regions[i].used) free_region(&regions[i]);
    }
    return 1;
}

/* Copy dirty regions and palettes from PS1 VRAM. Banks A-D, F and G must be mapped to the CPU (LCDC). */
static void upload(void) {
    u8 *base = (u8 *)VRAM_A;
    for (int i = 0; i < MAX_REGIONS; ++i) {
        Region *r = &regions[i];
        if (!r->used || !r->dirty) continue;
        r->dirty = 0;
        int row_bytes = r->tw * (r->depth == 0 ? 4 : r->depth == 1 ? 8 : 16) / 8;
        u8 *dst = base + r->unit * TEX_UNIT;
        for (int row = 0; row < r->h; ++row) {
            const u16 *src = psx_vram + ((r->y + row) & 511) * 1024;
            u16 *out = (u16 *)(dst + row * row_bytes);
            if (r->depth < 2 && r->x + r->wh <= 1024) {
                memcpy(out, src + r->x, r->wh * 2);
            } else {
                for (int k = 0; k < r->wh; ++k) {
                    u16 c = src[(r->x + k) & 1023];
                    /* 15-bit: the DS shows texels with bit 15 set; the PS1 hides only 0x0000 */
                    out[k] = r->depth == 2 ? (c ? c | 0x8000 : 0) : c;
                }
            }
        }
    }
    u16 *pal = (u16 *)VRAM_F;
    for (int i = 0; i < PAL_SLOTS; ++i) {
        Palette *p = &palettes[i];
        if (!p->used || !p->dirty) continue;
        p->dirty = 0;
        const u16 *src = psx_vram + (p->y & 511) * 1024;
        for (int k = 0; k < p->count; ++k) pal[i * 256 + k] = src[(p->x + k) & 1023];
    }
}

/* ---- command emission -------------------------------------------------------------------------------------- */

enum { CMD_COLOR = 0x20, CMD_TEXCOORD = 0x22, CMD_VTX16 = 0x23, CMD_POLYATTR = 0x29, CMD_TEXIMAGE = 0x2A,
       CMD_PLTTBASE = 0x2B, CMD_BEGIN = 0x40 };
#define INITIAL_ATTR (3u << 6 | 31u << 16)

/* The scene's commands (packed FIFO format, one command per header), fed to the geometry FIFO by DMA while the
 * CPU goes on. Worst case per polygon: 4 state commands and 4 vertices of colour, texcoord and position. */
#define LIST_WORDS (MAX_POLYS * 36 + 16)
static u32 *list;
static int list_words;

static inline void fifo(u32 cmd, u32 param) { list[list_words++] = cmd; list[list_words++] = param; }

static inline u32 rgb_for(const Prim *p, int i) {
    int r, g, b;
    if ((p->flags & P_SEMI) && p->abr == 2) {
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

static void emit_scene(Buffer *b) {
    u32 attr = INITIAL_ATTR, image = 0, palette = ~0u;
    list_words = 0;
    fifo(CMD_POLYATTR, attr);
    fifo(CMD_TEXIMAGE, 0);
    int polys = 0, vertices = 0, z = 0;
    for (int k = 0; k < b->count; ++k) {
        Prim *p = &b->prims[k];
        ++z;
        if (polys >= MAX_POLYS || vertices + p->n > MAX_VERTICES) { gxh_stats_dropped++; continue; }
        int tu = 0, tv = 0;
        u32 want_image = 0;
        if (p->flags & P_TEXTURED) {
            Region *r = prim_group[k] == 0xFF ? NULL : groups[prim_group[k]].region;
            if (!r || (p->depth < 2 && prim_palette[k] == 0xFF)) { gxh_stats_texture_fail++; continue; }
            int per = texels_per_halfword(p->depth);
            tu = (r->x - p->tx) * per;
            tv = r->y - p->ty;
            want_image = (u32)(r->unit * TEX_UNIT / 8) | (u32)(log2_at_least(r->tw) - 3) << 20 |
                         (u32)(log2_at_least(r->th) - 3) << 23 | (u32)(p->depth == 0 ? 3 : p->depth == 1 ? 4 : 7) << 26;
            if (p->depth < 2) {
                want_image |= 1u << 29; /* PS1 0x0000 at index 0 is transparent (FF9's CLUTs) */
                u32 want_palette = (u32)prim_palette[k] * 512 / 16;
                if (want_palette != palette) { fifo(CMD_PLTTBASE, want_palette); palette = want_palette; }
            }
        }
        if (want_image != image) { fifo(CMD_TEXIMAGE, want_image); image = want_image; }
        int alpha = 31, id = 0;
        if (p->flags & P_SEMI) {
            alpha = p->abr == 3 ? 8 : 16;
            if (p->abr == 2) {
                alpha = (p->r[0] + p->g[0] + p->b[0]) * 31 / (3 * 255); /* darkening as black at source luminance */
                if (!alpha) continue; /* alpha 0 would be wireframe */
            }
            id = 1 + (polys & 31); /* translucent polygons with one ID don't blend over each other */
        }
        u32 want_attr = 3u << 6 | (u32)alpha << 16 | (u32)id << 24;
        if (want_attr != attr) { fifo(CMD_POLYATTR, want_attr); attr = want_attr; }
        static const int quad_order[4] = {0, 1, 3, 2}; /* PS1 quad 0,1,2 + 1,2,3 is DS quad 0,1,3,2 */
        fifo(CMD_BEGIN, p->n == 4 ? 1 : 0);
        for (int j = 0; j < p->n; ++j) {
            int i = p->n == 4 ? quad_order[j] : j;
            if (j == 0 || (p->flags & P_GOURAUD)) fifo(CMD_COLOR, rgb_for(p, i));
            if (p->flags & P_TEXTURED)
                fifo(CMD_TEXCOORD, (u32)(u16)((p->u[i] - tu) * 16) | (u32)(u16)((p->v[i] - tv) * 16) << 16);
            list[list_words++] = CMD_VTX16;
            list[list_words++] = (u32)(u16)p->x[i] | (u32)(u16)p->y[i] << 16;
            list[list_words++] = (u32)(u16)z;
        }
        ++polys;
        vertices += p->n;
    }
    fifo(0x50, GL_TRANS_MANUALSORT); /* SWAP_BUFFERS: shown from the next VBlank */
    gxh_stats_polys = polys;
}

/* ---- backend ----------------------------------------------------------------------------------------------- */

static void dirty_cells(int x0, int y0, int x1, int y1);

static void soft_touch(int x0, int y0, int x1, int y1) {
    if (x0 < gp0_state.area_x0) x0 = gp0_state.area_x0;
    if (y0 < gp0_state.area_y0) y0 = gp0_state.area_y0;
    if (x1 > gp0_state.area_x1) x1 = gp0_state.area_x1;
    if (y1 > gp0_state.area_y1) y1 = gp0_state.area_y1;
    if (x1 < x0 || y1 < y0) return;
    dirty_cells(x0, y0, x1, y1);
    psx_vram_touch(x0, y0, x1 - x0 + 1, y1 - y0 + 1);
}

static void hybrid_poly(const Gp0Mode *m, const Gp0Vtx *v, int n) {
    Buffer *b = target();
    if (b && b != displayed()) {
        if (!triangle_area(v, v + 1, v + 2) && (n == 3 || !triangle_area(v + 1, v + 2, v + 3))) return;
        int clipped = 0;
        for (int i = 0; i < n; ++i)
            if (v[i].x < gp0_state.area_x0 || v[i].x > gp0_state.area_x1 + 1 ||
                v[i].y < gp0_state.area_y0 || v[i].y > gp0_state.area_y1 + 1) clipped = 1;
        if (!clipped) { record(b, m, v, n); return; }
        Gp0Vtx t[3] = {v[0], v[1], v[2]};
        record_clipped_triangle(b, m, t);
        if (n == 4) { t[0] = v[1]; t[1] = v[2]; t[2] = v[3]; record_clipped_triangle(b, m, t); }
        return;
    }
    int x0 = v[0].x, x1 = x0, y0 = v[0].y, y1 = y0;
    for (int i = 1; i < n; ++i) {
        if (v[i].x < x0) x0 = v[i].x;
        if (v[i].x > x1) x1 = v[i].x;
        if (v[i].y < y0) y0 = v[i].y;
        if (v[i].y > y1) y1 = v[i].y;
    }
    psx_softgpu.poly(m, v, n);
    soft_touch(x0, y0, x1, y1);
}

static void hybrid_line(const Gp0Mode *m, const Gp0Vtx *a, const Gp0Vtx *b2) {
    target();
    psx_softgpu.line(m, a, b2);
    soft_touch(a->x < b2->x ? a->x : b2->x, a->y < b2->y ? a->y : b2->y,
               a->x > b2->x ? a->x : b2->x, a->y > b2->y ? a->y : b2->y);
}

static void hybrid_rect(const Gp0Mode *m, const Gp0Vtx *v, int w, int h) {
    target();
    psx_softgpu.rect(m, v, w, h);
    soft_touch(v->x, v->y, v->x + w - 1, v->y + h - 1);
}

static const Gp0Backend hybrid_backend = {hybrid_poly, hybrid_line, hybrid_rect, 0, 0};

/* A fill (ClearImage, GP0 02) over most of a buffer is that scene's clear: the 3D layer's clear colour. */
void psx_vram_filled(int x, int y, int w, int h, unsigned int rgb15) {
    if (!active) return;
    Buffer *b = buffer_at(x, y);
    if (b && w * h * 2 >= b->w * b->h) {
        if (b->shown && b != displayed()) start_scene(b);
        b->clear = rgb15;
    }
}

/* ---- presentation ------------------------------------------------------------------------------------------ */

#define OBJ_BLOCK 32
#define OBJ_SLOTS 32   /* 2 KB each in bank E */
static s8 block_slot[6][8];
static u8 slot_used[OBJ_SLOTS];
static Buffer *last_shown;
static u32 block_hash[6][8];

void gxh_init(void) {
    for (int i = 0; i < 2; ++i) buffers[i].prims = malloc(MAX_PRIMS * sizeof(Prim));
    list = malloc(LIST_WORDS * 4);
}

int gxh_active(void) { return active; }

void gxh_enable(int on) {
    if (on == active) return;
    active = on;
    if (on) {
        memset(block_slot, -1, sizeof block_slot);
        memset(block_hash, 0, sizeof block_hash);
        memset(slot_used, 0, sizeof slot_used);
        last_shown = NULL;
        videoSetMode(MODE_0_3D | DISPLAY_SPR_ACTIVE | SpriteMapping_Bmp_1D_128);
        vramSetPrimaryBanks(VRAM_A_TEXTURE, VRAM_B_TEXTURE, VRAM_C_TEXTURE, VRAM_D_TEXTURE);
        vramSetBankE(VRAM_E_MAIN_SPRITE);
        vramSetBankF(VRAM_F_TEX_PALETTE_SLOT0);
        vramSetBankG(VRAM_G_TEX_PALETTE_SLOT1);
        oamInit(&oamMain, SpriteMapping_Bmp_1D_128, false);
        glInit();
        glEnable(GL_TEXTURE_2D | GL_BLEND);
        glClearPolyID(63);
        glClearDepth(GL_MAX_DEPTH);
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        bgSetPriority(0, 1);
        memset(regions, 0, sizeof regions);
        memset(unit_used, 0, sizeof unit_used);
        memset(palettes, 0, sizeof palettes);
        for (int i = 0; i < 2; ++i) { buffers[i].valid = 0; buffers[i].count = 0; }
        gp0_backend = &hybrid_backend;
    } else {
        oamClear(&oamMain, 0, 0);
        oamUpdate(&oamMain);
        gp0_backend = &psx_softgpu;
    }
}

/* Send a buffer's scene to the 3D engine; it is shown from the next frame on. */
static void submit(Buffer *b, int display_h) {
    unsigned int profile = port_profile_begin();
    while (dmaBusy(0)) {} /* the previous scene's list */
    plan_textures(b);
    u32 saved = vramSetPrimaryBanks(VRAM_A_LCD, VRAM_B_LCD, VRAM_C_LCD, VRAM_D_LCD);
    vramSetBankF(VRAM_F_LCD);
    vramSetBankG(VRAM_G_LCD);
    upload();
    vramRestorePrimaryBanks(saved);
    vramSetBankF(VRAM_F_TEX_PALETTE_SLOT0);
    vramSetBankG(VRAM_G_TEX_PALETTE_SLOT1);
    int h = display_h > 0 ? display_h : 224;
    int out_h = h * 4 / 5;
    glViewport(0, (192 - out_h) / 2, 255, (192 - out_h) / 2 + out_h - 1);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    /* v16 units: 1 = one PS1 pixel (as 1/4096); later primitives get larger z, nearer */
    glOrthof32(0, 320, h, 0, -4096, 4096);
    glMatrixMode(GL_MODELVIEW);
    unsigned int c = b->clear;
    glClearColor(c & 31, (c >> 5) & 31, (c >> 10) & 31, 31);
    emit_scene(b);
    DC_FlushRange(list, list_words * 4);
    while (dmaBusy(1) || dmaBusy(2) || dmaBusy(3)) {} /* libnds: no concurrent DMA when one feeds the FIFO */
    dmaSetParams(0, list, (void *)&GFX_FIFO, DMA_FIFO | (u32)list_words);
    b->submitted = 1;
    port_profile_detail_end(PORT_PROFILE_UPLOAD, profile); /* reported as gx_texture_upload */
}

/* 2D layer: the displayed buffer's 2D pixels as 32x32 bitmap OBJs over the 3D layer. Polygons recorded for GX
 * never reach PS1 VRAM, so the buffer holds its scene's clearing fill plus 2D pixels: a pixel other than the clear
 * value is 2D (one exactly equal to it shows the 3D layer instead). Blocks are rebuilt when their source cells
 * (8x8 PS1 pixels) were drawn and their pixels hash differently (the battle UI redraws every window each VBlank,
 * mostly unchanged); a block keeps its OBJ slot. */
static u8 cell_dirty[512 / 8][1024 / 8];
static int last_x, last_y, last_w, last_h;

static void dirty_cells(int x0, int y0, int x1, int y1) {
    for (int y = y0 >> 3; y <= y1 >> 3; ++y)
        for (int x = x0 >> 3; x <= x1 >> 3; ++x) cell_dirty[y & 63][x & 127] = 1;
}

void gxh_present(const PsxDisplayInfo *d, void (*scale)(u16 *dst, int x, int y, int w, int h)) {
    Buffer *b = buffer_at(d->x, d->y);
    if (b) {
        b->shown = 1;
        if (!b->submitted) submit(b, d->h);
    }
    const u16 clear = b ? (u16)b->clear : 0;
    const u32 clear2 = clear | (u32)clear << 16;
    int w = d->w ? d->w : 320, h = d->h ? d->h : 224;
    if (w > 320) w = 320;
    if (h > 240) h = 240;
    int out_w = w * 4 / 5, out_h = h * 4 / 5, top = (192 - out_h) / 2;
    /* another buffer or area: every block's hash decides */
    int check_all = b != last_shown || d->x != last_x || d->y != last_y || w != last_w || h != last_h;
    last_shown = b; last_x = d->x; last_y = d->y; last_w = w; last_h = h;
    static u16 block[OBJ_BLOCK * 256] __attribute__((aligned(4)));   /* a scaled block, stride 256 */
    u16 *gfx = (u16 *)SPRITE_GFX;
    int changed = 0;
    for (int by = 0, row = 0; by < out_h; by += OBJ_BLOCK, ++row)
        for (int bx = 0, col = 0; bx < out_w; bx += OBJ_BLOCK, ++col) {
            int sx0 = d->x + bx * 5 / 4, sy0 = d->y + by * 5 / 4;
            int bw = (bx + OBJ_BLOCK < out_w ? OBJ_BLOCK : out_w - bx), bh = (by + OBJ_BLOCK < out_h ? OBJ_BLOCK : out_h - by);
            int sw = bw * 5 / 4, sh = bh * 5 / 4;
            if (!check_all) {
                int dirty = 0;
                for (int y = sy0 >> 3; y <= (sy0 + sh) >> 3 && !dirty; ++y)
                    for (int x = sx0 >> 3; x <= (sx0 + sw) >> 3; ++x)
                        if (cell_dirty[y & 63][x & 127]) { dirty = 1; break; }
                if (!dirty) continue;
            }
            u32 any = 0, hash = 2166136261u;
            int hx0 = sx0 & ~1, hx1 = (sx0 + sw + 1) & ~1; /* whole words: a wider span only rebuilds more often */
            for (int y = sy0; y < sy0 + sh; ++y) {
                const u16 *vrow = psx_vram + (y & 511) * 1024;
                if (hx1 > 1024) {
                    for (int k = hx0; k < hx1; ++k) { u32 c = vrow[k & 1023]; any |= c ^ clear; hash = (hash ^ c) * 16777619u; }
                    continue;
                }
                const u32 *words = (const u32 *)(vrow + hx0);
                for (int k = 0; k < (hx1 - hx0) / 2; ++k) { u32 c = words[k]; any |= c ^ clear2; hash = (hash ^ c) * 16777619u; }
            }
            if (!check_all && hash == block_hash[row][col]) continue;
            if (check_all && hash == block_hash[row][col] && (block_slot[row][col] >= 0) == (any != 0)) continue;
            block_hash[row][col] = hash;
            int s = block_slot[row][col];
            if (!any) {
                if (s >= 0) { oamClearSprite(&oamMain, s); slot_used[s] = 0; block_slot[row][col] = -1; changed = 1; }
                continue;
            }
            if (s < 0) {
                for (s = 0; s < OBJ_SLOTS && slot_used[s]; ++s) {}
                if (s == OBJ_SLOTS) { gxh_stats_blocks = -1; continue; }
                slot_used[s] = 1; block_slot[row][col] = (s8)s;
            }
            scale(block, sx0, sy0, sw, sh);
            u16 *dst = gfx + s * OBJ_BLOCK * OBJ_BLOCK;
            for (int oy = 0; oy < OBJ_BLOCK; ++oy) {
                u32 *out = (u32 *)(dst + oy * OBJ_BLOCK);
                if (oy >= bh) { for (int k = 0; k < OBJ_BLOCK / 2; ++k) out[k] = 0; continue; }
                const u16 *vrow = psx_vram + ((sy0 + oy * 5 / 4) & 511) * 1024;
                const u16 *frow = block + oy * 256;
                for (int ox = 0; ox < OBJ_BLOCK; ox += 2) {
                    u32 c0 = ox < bw && vrow[(sx0 + ox * 5 / 4) & 1023] != clear ? frow[ox] | 0x8000 : 0;
                    u32 c1 = ox + 1 < bw && vrow[(sx0 + (ox + 1) * 5 / 4) & 1023] != clear ? frow[ox + 1] | 0x8000 : 0;
                    out[ox / 2] = c0 | c1 << 16; /* word stores: VRAM ignores byte writes */
                }
            }
            oamSet(&oamMain, s, bx, top + by, 0, 15, SpriteSize_32x32, SpriteColorFormat_Bmp,
                   dst, -1, false, false, false, false, false);
            changed = 1;
        }
    for (int y = (d->y >> 3); y <= (d->y + h) >> 3; ++y)
        for (int x = (d->x >> 3); x <= (d->x + w) >> 3; ++x) cell_dirty[y & 63][x & 127] = 0;
    int n = 0;
    for (int s = 0; s < OBJ_SLOTS; ++s) n += slot_used[s];
    if (gxh_stats_blocks >= 0) gxh_stats_blocks = n;
    if (changed) oamUpdate(&oamMain);
}
#endif
