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
#include "psx_runtime.h"

extern unsigned short *psx_vram;

#define MAX_PRIMS 2500   /* recorded; the hardware takes 2048 polygons and 6144 vertices a frame */
#define MAX_POLYS 2044
#define MAX_VERTICES 6140

int gxh_stats_polys, gxh_stats_dropped, gxh_stats_texture_fail, gxh_stats_palette_fail, gxh_stats_blocks;
int gxh_stats_regions, gxh_stats_units, gxh_stats_unplaced; /* the last plan: regions and 2 KB units in use */
static int active;

/* ---- recorded primitives ----------------------------------------------------------------------------------- */

enum { P_TEXTURED = 1, P_RAW = 2, P_SEMI = 4, P_GOURAUD = 8,
       P_RECT = 16 /* a sprite: u, v are its last texels; the right and bottom vertices sample one further */ };

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
    for (int row = 0; row < b->h; ++row)
        memset(marks + (((b->y + row) & 511) * 1024 + b->x) / 8, 0,
               ((b->x + b->w <= 1024 ? b->w : 1024 - b->x) + 7) / 8); /* buffers start on 8-pixel columns */
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
    int conv, clut_x, clut_y;           /* conv: additive-blend copy of 4/8-bit texels through this CLUT (A5I3) */
    int cpal;                           /* conv: its 8-colour palette in ADD_SLOT */
} Region;

static Region regions[MAX_REGIONS];
static u8 unit_used[TEX_UNITS];
/* Untextured additive glows sample a fixed ramp whose alpha rises along U (A5I3, 32x8, white): each vertex's U is
 * its brightness, so the interpolated coordinate is a per-pixel alpha (the DS has none per vertex). */
#define RAMP_UNIT (TEX_UNITS - 1)
#define RAMP_PALETTE 31   /* its palette among ADD_SLOT's 8-colour palettes */
static int ramp_written;

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
    int cpal = 0;
    if (conv) { /* a free 8-colour palette */
        u32 taken = 0;
        for (int i = 0; i < MAX_REGIONS; ++i) if (regions[i].used && regions[i].conv) taken |= 1u << regions[i].cpal;
        while (cpal < RAMP_PALETTE && (taken >> cpal & 1)) ++cpal;
        if (cpal == RAMP_PALETTE) return NULL;
    }
    int bytes = ((1 << sw) << sh) * (conv ? 8 : depth == 0 ? 4 : depth == 1 ? 8 : 16) / 8;
    int units = 1 << log2_at_least((bytes + TEX_UNIT - 1) / TEX_UNIT);
    int unit = allocate_units(units);
    if (unit < 0) return NULL;
    for (int i = 0; i < MAX_REGIONS; ++i)
        if (!regions[i].used) {
            regions[i] = (Region){1, depth, x, y, wh, h, 1 << sw, 1 << sh, unit, units, 1, 1, conv, cx, cy, cpal};
            return &regions[i];
        }
    memset(unit_used + unit, 0, units);
    return NULL;
}

/* Texture palettes: 64 slots of 256 colours in F-G (32 KB). The last holds the additive copies' 32 palettes of 8
 * colours (A5I3); the others take CLUTs. */
#define PAL_SLOTS 63
#define ADD_SLOT 63
typedef struct { int used, x, y, count, dirty, wanted; } Palette;
static Palette palettes[PAL_SLOTS];

static void regions_touch(int x, int y, int w, int h);
static void dirty_cells(int x0, int y0, int x1, int y1);
static void ui_flush(void);
static void ui_note_write(int x, int y, int w, int h);
static void ui_dirty_rect(int x0, int y0, int x1, int y1);
static int ui_flushing;

/* gpu.c: StoreImage and MoveImage are about to read this rectangle: deferred UI drawing lands first. */
void psx_vram_read(int x, int y, int w, int h) {
    (void)x; (void)y; (void)w; (void)h;
    if (active) ui_flush();
}

/* gpu.c: LoadImage, MoveImage, fills are about to write this rectangle. */
void psx_vram_touch(int x, int y, int w, int h) {
    if (!active) return;
    ui_flush();
    ui_note_write(x, y, w, h);
    ui_dirty_rect(x, y, x + w - 1, y + h - 1);
    dirty_cells(x, y, x + w - 1, y + h - 1);
    regions_touch(x, y, w, h);
}

static void regions_touch(int x, int y, int w, int h) {
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

/* Per-scene texture plan: one group per (depth, page) with its texels' bounding box (halfwords, rows). */
#define MAX_GROUPS 96
typedef struct { int depth, tx, ty, x0, y0, x1, y1; Region *region; int conv, cx, cy; } Group;
static Group groups[MAX_GROUPS];
static int group_count;
static u8 prim_group[MAX_PRIMS];
static u8 prim_palette[MAX_PRIMS];

static int plan_textures(Buffer *b) {
    int last_group = 0, last_palette = 0;
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
        /* Additive polygons (modes 1, 3) use a 15-bit copy through their CLUT whose dark texels are transparent:
         * under alpha blending a black background would darken what an additive one leaves alone. */
        int conv = (p->flags & P_SEMI) && (p->abr == 1 || p->abr == 3);
        int x0, x1, y0, y1;
        if (conv) { /* effect-sized: the texels' own box, in 8-texel steps */
            u0 &= ~7; u1 |= 7;
            x0 = p->tx + u0 / per; x1 = p->tx + u1 / per; y0 = p->ty + (v0 & ~7); y1 = p->ty + (v1 | 7);
        } else {
            /* Stable regions: U and V in 64-texel steps, so a page keeps its region (and copies nothing) while the
             * camera moves over it, without holding the parts of the page no primitive uses. */
            u0 &= ~63; u1 |= 63; v0 &= ~63; v1 |= 63;
            x0 = p->tx + u0 / per; x1 = p->tx + u1 / per; y0 = p->ty + v0; y1 = p->ty + v1;
        }
        /* consecutive primitives nearly always share a page and CLUT: try the last match first */
#define SAME_GROUP(q) ((q).depth == p->depth && (q).tx == p->tx && (q).ty == p->ty && (q).conv == conv && \
                       (!conv || p->depth == 2 || ((q).cx == p->clut_x && (q).cy == p->clut_y)))
        int g = last_group;
        if (g >= group_count || !SAME_GROUP(groups[g]))
            for (g = 0; g < group_count; ++g)
                if (SAME_GROUP(groups[g])) break;
#undef SAME_GROUP
        last_group = g;
        if (g == group_count) {
            if (group_count == MAX_GROUPS) continue;
            groups[group_count++] = (Group){p->depth, p->tx, p->ty, x0, y0, x1, y1, NULL, conv, p->clut_x, p->clut_y};
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
            int count = p->depth ? 256 : 16, s = last_palette, empty = -1;
            Palette *lq = &palettes[s];
            if (!(lq->used && lq->x == p->clut_x && lq->y == p->clut_y && lq->count >= count))
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
            last_palette = s;
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
            q->region = region_containing(q->depth, q->conv, q->cx, q->cy, q->x0, q->y0, wh, h);
            if (!q->region) q->region = place_region(q->depth, q->conv, q->cx, q->cy, q->x0, q->y0, wh, h);
            if (q->region) q->region->wanted = 1;
            else failed = 1;
        }
        if (!failed || attempt) {
            gxh_stats_unplaced = 0;
            for (int g = 0; g < group_count; ++g) gxh_stats_unplaced += !groups[g].region;
            break;
        }
        for (int g = 0; g < group_count; ++g) groups[g].region = NULL;
        for (int i = 0; i < MAX_REGIONS; ++i) if (regions[i].used) free_region(&regions[i]);
    }
    return 1;
}

/* An additive-blend copy (PS1 modes 1 and 3: destination + source). The DS only alpha-blends, so each texel's
 * alpha is its brightest channel and its colour is normalised by it: source x alpha is the PS1 texel, and what
 * remains of the destination, (1 - alpha), is exact for dim texels and fades only under bright ones. A gradient
 * of one hue normalises to one colour, so 8 per texture (A5I3, chosen across its CLUT) are enough; texels without
 * the semi-transparency bit are opaque, colour 0 transparent. */
static inline u16 normalised(u16 c, int *bright) {
    int cr = c & 31, cg = (c >> 5) & 31, cb = (c >> 10) & 31, m = cr > cg ? (cr > cb ? cr : cb) : (cg > cb ? cg : cb);
    *bright = m;
    return m ? (u16)(cr * 31 / m | (cg * 31 / m) << 5 | (cb * 31 / m) << 10) : 0;
}

static inline int colour_distance(u16 a, u16 b) {
    int dr = (a & 31) - (b & 31), dg = ((a >> 5) & 31) - ((b >> 5) & 31), db = ((a >> 10) & 31) - ((b >> 10) & 31);
    return dr * dr + dg * dg + db * db;
}

/* the A5I3 alpha of a texel: transparent 0x0000, opaque without the semi-transparency bit, else its brightness */
static inline u8 texel_alpha(u16 c, int bright) { return !c ? 0 : (u8)(c & 0x8000 ? bright : 31); }

static void conv_upload(Region *r, u8 *dst, int row_bytes) {
    /* the colours to choose from: the CLUT's, or (15-bit) a sample of the texels */
    enum { CHOICES = 256 };
    static u16 norm[CHOICES];
    static u8 alpha[CHOICES];
    int count = 0;
    const u16 *clut = psx_vram + (r->clut_y & 511) * 1024;
    if (r->depth < 2) {
        count = r->depth ? 256 : 16;
        for (int i = 0; i < count; ++i) {
            u16 c = clut[(r->clut_x + i) & 1023];
            int m;
            norm[i] = normalised(c, &m);
            alpha[i] = texel_alpha(c, m);
        }
    } else {
        int step_y = r->h / 16 > 0 ? r->h / 16 : 1, step_x = r->wh / 16 > 0 ? r->wh / 16 : 1;
        for (int y = 0; y < r->h && count < CHOICES; y += step_y)
            for (int x = 0; x < r->wh && count < CHOICES; x += step_x) {
                u16 c = psx_vram[((r->y + y) & 511) * 1024 + ((r->x + x) & 1023)];
                int m;
                norm[count] = normalised(c, &m);
                alpha[count++] = texel_alpha(c, m);
            }
    }
    /* representatives: the brightest entry's colour, then repeatedly the colour farthest from those chosen */
    u16 reps[8];
    int nreps = 0;
    for (;;) {
        int best = -1, best_d = 0;
        for (int i = 0; i < count; ++i) {
            if (!(alpha[i])) continue;
            int d = 1 << 30;
            for (int k = 0; k < nreps; ++k) {
                int e = colour_distance(norm[i], reps[k]);
                if (e < d) d = e;
            }
            if (!nreps) d = alpha[i]; /* first: the brightest */
            if (d > best_d) { best_d = d; best = i; }
        }
        if (best < 0) break;
        reps[nreps++] = norm[best];
        if (nreps == 8) break;
    }
    if (!nreps) reps[nreps++] = 0x7FFF;
    u16 *pal = (u16 *)VRAM_F + ADD_SLOT * 256 + r->cpal * 8;
    for (int k = 0; k < 8; ++k) pal[k] = k < nreps ? reps[k] : 0;
    if (r->depth < 2) {
        static u8 code[256]; /* A5I3 texel per CLUT index */
        for (int i = 0; i < count; ++i) {
            int best = 0, best_d = 1 << 30;
            for (int k = 0; k < nreps; ++k) {
                int e = colour_distance(norm[i], reps[k]);
                if (e < best_d) { best_d = e; best = k; }
            }
            code[i] = (u8)(alpha[i] << 3 | best);
        }
        const int per = texels_per_halfword(r->depth), bits = 16 / per, mask = (1 << bits) - 1;
        for (int row = 0; row < r->h; ++row) {
            const u16 *src = psx_vram + ((r->y + row) & 511) * 1024;
            u16 *out = (u16 *)(dst + row * row_bytes); /* texel pairs: VRAM ignores byte writes */
            for (int k = 0; k < r->wh; ++k) {
                u16 word = src[(r->x + k) & 1023];
                for (int t = 0; t < per; t += 2)
                    out[(k * per + t) / 2] = code[(word >> (t * bits)) & mask] | code[(word >> ((t + 1) * bits)) & mask] << 8;
            }
        }
        return;
    }
    /* 15-bit: each normalised colour's representative, found once per 12-bit key */
    static u8 nearest[4096];
    memset(nearest, 0xFF, sizeof nearest);
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
}

/* Copy dirty regions and palettes from PS1 VRAM. Banks A-D, F and G must be mapped to the CPU (LCDC). */
static void upload(void) {
    u8 *base = (u8 *)VRAM_A;
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
#define CHUNK_WORDS 4096   /* two 16 KB halves: one fills while the other goes to the FIFO */
static u32 chunks[2][CHUNK_WORDS] __attribute__((aligned(32)));
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

static inline void put(u32 word) {
    chunks[chunk][chunk_words++] = word;
    if (chunk_words == CHUNK_WORDS) flush_chunk();
}

static inline void fifo(u32 cmd, u32 param) { put(cmd); put(param); }

/* additive: the polygon's brightest vertex channel, which went into its alpha (conv_upload); the vertex colours
 * keep the hue and the shading relative to it. */
static inline u32 rgb_for(const Prim *p, int i, int additive) {
    int r, g, b;
    if (additive) {
        r = (p->flags & P_RAW) ? 31 : p->r[i] * 31 / additive;
        g = (p->flags & P_RAW) ? 31 : p->g[i] * 31 / additive;
        b = (p->flags & P_RAW) ? 31 : p->b[i] * 31 / additive;
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
static u8 prim_drop[MAX_PRIMS];
static void plan_drops(Buffer *b) {
    int vertices = 0;
    for (int k = 0; k < b->count; ++k) vertices += b->prims[k].n;
    memset(prim_drop, 0, b->count);
    int over_p = b->count - MAX_POLYS, over_v = vertices - MAX_VERTICES;
    static const int bounds[] = {16, 64, 256, 1024, 4096, 1 << 30}; /* twice the area */
    for (unsigned t = 0; t < sizeof bounds / sizeof *bounds && (over_p > 0 || over_v > 0); ++t)
        for (int k = 0; k < b->count && (over_p > 0 || over_v > 0); ++k)
            if (!prim_drop[k] && prim_area2(&b->prims[k]) < bounds[t]) {
                prim_drop[k] = 1;
                --over_p;
                over_v -= b->prims[k].n;
            }
}

/* Translucent polygons with one ID don't blend over each other. Each gets its own, so overlapping layers add up,
 * except that one sharing an edge with a recent one takes that one's ID: the DS draws a shared edge's pixels for
 * both polygons, which would blend twice and leave a bright seam through a mesh (whose polygons the ordering table
 * interleaves with others). */
#define RECENT 16
static const Prim *recent[RECENT];
static u8 recent_id[RECENT];
static int recent_at;
static int translucent_id(const Prim *p, int polys) {
    int id = 1 + (polys & 31);
    for (int k = 0; k < RECENT; ++k) {
        const Prim *q = recent[k];
        if (!q) continue;
        int shared = 0;
        for (int i = 0; i < p->n; ++i)
            for (int j = 0; j < q->n; ++j) shared += p->x[i] == q->x[j] && p->y[i] == q->y[j];
        if (shared >= 2) { id = recent_id[k]; break; }
    }
    recent[recent_at] = p; recent_id[recent_at] = (u8)id; recent_at = (recent_at + 1) % RECENT;
    return id;
}

static void emit_scene(Buffer *b) {
    memset(recent, 0, sizeof recent);
    u32 attr = INITIAL_ATTR, image = 0, palette = ~0u;
    fifo(CMD_POLYATTR, attr);
    fifo(CMD_TEXIMAGE, 0);
    int polys = 0, vertices = 0, z = 0;
    plan_drops(b);
    for (int k = 0; k < b->count; ++k) {
        Prim *p = &b->prims[k];
        ++z;
        if (prim_drop[k]) { gxh_stats_dropped++; continue; }
        if (polys >= MAX_POLYS || vertices + p->n > MAX_VERTICES) { gxh_stats_dropped++; continue; }
        int tu = 0, tv = 0, additive = 0;
        u32 want_image = 0;
        if (p->flags & P_TEXTURED) {
            Region *r = prim_group[k] == 0xFF ? NULL : groups[prim_group[k]].region;
            if (!r) { gxh_stats_texture_fail++; continue; }
            if (p->depth < 2 && !r->conv && prim_palette[k] == 0xFF) { gxh_stats_palette_fail++; continue; }
            int per = texels_per_halfword(p->depth);
            tu = (r->x - p->tx) * per;
            tv = r->y - p->ty;
            want_image = (u32)(r->unit * TEX_UNIT / 8) | (u32)(log2_at_least(r->tw) - 3) << 20 |
                         (u32)(log2_at_least(r->th) - 3) << 23 |
                         (u32)(r->conv ? 6 : p->depth == 2 ? 7 : p->depth == 0 ? 3 : 4) << 26;
            if (r->conv) {
                u32 want_palette = (u32)(ADD_SLOT * 512 + r->cpal * 16) / 16;
                if (want_palette != palette) { fifo(CMD_PLTTBASE, want_palette); palette = want_palette; }
                additive = 1;
            } else if (p->depth < 2) {
                want_image |= 1u << 29; /* PS1 0x0000 at index 0 is transparent (FF9's CLUTs) */
                u32 want_palette = (u32)prim_palette[k] * 512 / 16;
                if (want_palette != palette) { fifo(CMD_PLTTBASE, want_palette); palette = want_palette; }
            }
        }
        int glow = !(p->flags & P_TEXTURED) && (p->flags & P_SEMI) && (p->abr == 1 || p->abr == 3);
        if (glow) {
            want_image = (u32)(RAMP_UNIT * TEX_UNIT / 8) | 2u << 20 | 0u << 23 | 6u << 26; /* 32x8 A5I3 */
            u32 want_palette = (u32)(ADD_SLOT * 512 + RAMP_PALETTE * 16) / 16;
            if (want_palette != palette) { fifo(CMD_PLTTBASE, want_palette); palette = want_palette; }
        }
        if (want_image != image) { fifo(CMD_TEXIMAGE, want_image); image = want_image; }
        int alpha = 31, id = 0;
        if (glow) {
            id = translucent_id(p, polys);
        } else if (additive) {
            /* the source's brightness (PS1 128 = x1; mode 3 adds a quarter), at most opaque */
            int m = 0;
            for (int j = 0; j < p->n; ++j) {
                int v = p->r[j] > p->g[j] ? (p->r[j] > p->b[j] ? p->r[j] : p->b[j]) : (p->g[j] > p->b[j] ? p->g[j] : p->b[j]);
                if (v > m) m = v;
                if (!(p->flags & P_GOURAUD)) break;
            }
            if (p->flags & P_RAW) m = 128;
            alpha = m * 31 / (p->abr == 3 ? 512 : 128);
            if (alpha > 31) alpha = 31;
            if (!alpha) continue; /* adds nothing (alpha 0 would be wireframe) */
            additive = m;
            id = translucent_id(p, polys);
        } else if (p->flags & P_SEMI) {
            alpha = p->abr == 3 ? 8 : p->abr == 1 ? 24 : 16; /* additive: a brighter blend than 50% */
            if (p->abr == 2) {
                alpha = (p->r[0] + p->g[0] + p->b[0]) * 31 / (3 * 255); /* darkening as black at source luminance */
                if (!alpha) continue; /* alpha 0 would be wireframe */
            }
            id = translucent_id(p, polys);
        }
        u32 want_attr = 3u << 6 | (u32)alpha << 16 | (u32)id << 24;
        if (want_attr != attr) { fifo(CMD_POLYATTR, want_attr); attr = want_attr; }
        static const int quad_order[4] = {0, 1, 3, 2}; /* PS1 quad 0,1,2 + 1,2,3 is DS quad 0,1,3,2 */
        fifo(CMD_BEGIN, p->n == 4 ? 1 : 0);
        for (int j = 0; j < p->n; ++j) {
            int i = p->n == 4 ? quad_order[j] : j;
            if (glow) {
                /* hue at full brightness; the brightness (a quarter of it in mode 3) is the ramp's U */
                int m = p->r[i] > p->g[i] ? (p->r[i] > p->b[i] ? p->r[i] : p->b[i]) : (p->g[i] > p->b[i] ? p->g[i] : p->b[i]);
                int k = p->flags & P_GOURAUD ? i : 0;
                if (k != i) m = p->r[k] > p->g[k] ? (p->r[k] > p->b[k] ? p->r[k] : p->b[k]) : (p->g[k] > p->b[k] ? p->g[k] : p->b[k]);
                u32 colour = m ? (u32)(p->r[k] * 31 / m | (p->g[k] * 31 / m) << 5 | (p->b[k] * 31 / m) << 10) : 0;
                int a = m * 31 / (p->abr == 3 ? 1020 : 255);
                fifo(CMD_COLOR, colour);
                fifo(CMD_TEXCOORD, (u32)(u16)(a * 16 + 8) | (u32)(u16)(4 * 16) << 16);
            } else if (j == 0 || (p->flags & P_GOURAUD)) fifo(CMD_COLOR, rgb_for(p, i, additive));
            if (p->flags & P_TEXTURED) {
                int u = p->u[i] - tu, v = p->v[i] - tv;
                if (p->flags & P_RECT) { u += i & 1; v += i >> 1; } /* vertices 1, 3 right; 2, 3 bottom */
                fifo(CMD_TEXCOORD, (u32)(u16)(u * 16) | (u32)(u16)(v * 16) << 16);
            }
            put(CMD_VTX16);
            put((u32)(u16)p->x[i] | (u32)(u16)p->y[i] << 16);
            put((u32)(u16)z);
        }
        ++polys;
        vertices += p->n;
    }
    fifo(0x50, GL_TRANS_MANUALSORT); /* SWAP_BUFFERS: shown from the next VBlank */
    gxh_stats_polys = polys;
}

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
typedef struct {
    u8 kind, n;
    s16 w, h;
    Gp0Mode m;
    Gp0Vtx v[4];
    Gp0State state;
} UiPrim;
#define UI_MAX 256   /* the battle UI draws about 110 a VBlank */
static UiPrim ui_list[2][UI_MAX];     /* [0] the previous VBlank's, [1] this one's */
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
    if (*x0 < p->state.area_x0) *x0 = p->state.area_x0;
    if (*y0 < p->state.area_y0) *y0 = p->state.area_y0;
    if (*x1 > p->state.area_x1) *x1 = p->state.area_x1;
    if (*y1 > p->state.area_y1) *y1 = p->state.area_y1;
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
    if (!p->m.textured) return 0;
    if (ui_written_all) return 1;
    int per = texels_per_halfword(p->m.depth > 2 ? 2 : p->m.depth);
    int px0 = p->m.tx, px1 = p->m.tx + 256 / per - 1, py0 = p->m.ty, py1 = p->m.ty + 255;
    for (int i = 0; i < ui_written_count; ++i) {
        const s16 *r = ui_written[i];
        if (px0 <= r[2] && r[0] <= px1 && py0 <= r[3] && r[1] <= py1) return 1;
        if (p->m.depth < 2 && r[1] <= p->m.clut_y && p->m.clut_y <= r[3] && r[0] <= p->m.clut_x + 255 &&
            p->m.clut_x <= r[2]) return 1;
    }
    return 0;
}

static void soft_touch(int x0, int y0, int x1, int y1);

/* Draw p, within clip (x0, y0, x1, y1 inclusive) when given: its drawing area narrowed to it. */
static void ui_draw(const UiPrim *p, const s16 *clip) {
    Gp0State saved = gp0_state;
    gp0_state = p->state;
    if (clip) {
        if (gp0_state.area_x0 < clip[0]) gp0_state.area_x0 = clip[0];
        if (gp0_state.area_y0 < clip[1]) gp0_state.area_y0 = clip[1];
        if (gp0_state.area_x1 > clip[2]) gp0_state.area_x1 = clip[2];
        if (gp0_state.area_y1 > clip[3]) gp0_state.area_y1 = clip[3];
        if (gp0_state.area_x1 < gp0_state.area_x0 || gp0_state.area_y1 < gp0_state.area_y0) { gp0_state = saved; return; }
    }
    if (p->kind == UI_RECT) psx_softgpu.rect(&p->m, p->v, p->w, p->h);
    else if (p->kind == UI_LINE) psx_softgpu.line(&p->m, &p->v[0], &p->v[1]);
    else psx_softgpu.poly(&p->m, p->v, p->n);
    int x0, y0, x1, y1;
    ui_bounds(p, &x0, &y0, &x1, &y1);
    if (x1 >= x0 && y1 >= y0) soft_touch(x0, y0, x1, y1);
    gp0_state = saved;
}

int gxh_stats_ui_drawn, gxh_stats_ui_skipped;

static void ui_flush(void) {
    if (!ui_count[1] || ui_flushing) return;
    ui_flushing = 1;
    UiPrim *prev = ui_list[0], *cur = ui_list[1];
    int n = ui_count[1], m = ui_count[0];
    int whole = ui_overflow;
    for (int i = 0; i < n && !whole; ++i)
        if (cur[i].m.semi || cur[i].state.check_mask) whole = 1;
    if (!whole) {
        for (int i = 0; i < n || i < m; ++i) {
            int x0, y0, x1, y1;
            int same = i < n && i < m && !memcmp(&cur[i], &prev[i], sizeof *cur) && !ui_source_written(&cur[i]);
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
    memcpy(prev, cur, n * sizeof *cur);
    ui_count[0] = n;
    ui_count[1] = 0;
    ui_overflow = 0;
    ui_dirty_count = ui_dirty_all = 0;
    ui_written_count = ui_written_all = 0;
    ui_flushing = 0;
}

/* Defer a primitive drawn into the displayed buffer. Returns 0 when it has to be drawn now. */
static int ui_defer(int kind, const Gp0Mode *m, const Gp0Vtx *v, int n, int w, int h) {
    Buffer *b = displayed();
    if (!b || buffer_at(gp0_state.area_x0, gp0_state.area_y0) != b) return 0;
    if (ui_buffer != b) {
        /* another buffer is shown: its previous list is stale, and everything it holds may differ */
        ui_flush();
        ui_buffer = b;
        ui_count[0] = 0;
    }
    if (ui_count[1] == UI_MAX) { ui_overflow = 1; ui_flush(); return 0; }
    UiPrim *p = &ui_list[1][ui_count[1]++];
    memset(p, 0, sizeof *p); /* padding compares equal */
    p->kind = (u8)kind; p->n = (u8)n; p->w = (s16)w; p->h = (s16)h;
    p->m = *m;
    for (int i = 0; i < n; ++i) p->v[i] = v[i];
    p->state = gp0_state;
    return 1;
}

static void hybrid_poly(const Gp0Mode *m, const Gp0Vtx *v, int n) {
    Buffer *b = target();
    if (b && b == displayed() && ui_defer(UI_POLY, m, v, n, 0, 0)) return;
    ui_flush();
    /* Untextured additive glows (Gouraud, black at the edges) go to the 3D layer through the alpha ramp; outside a
     * scene (offscreen) they are drawn in software, marked, over the clear value. */
    int glow = m->semi && !m->textured && (m->abr == 1 || m->abr == 3);
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

static void hybrid_line(const Gp0Mode *m, const Gp0Vtx *a, const Gp0Vtx *b2) {
    Buffer *b = target();
    Gp0Vtx pair[2] = {*a, *b2};
    if (b && b == displayed() && ui_defer(UI_LINE, m, pair, 2, 0, 0)) return;
    ui_flush();
    psx_softgpu.line(m, a, b2);
    soft_touch(a->x < b2->x ? a->x : b2->x, a->y < b2->y ? a->y : b2->y,
               a->x > b2->x ? a->x : b2->x, a->y > b2->y ? a->y : b2->y);
}

static void hybrid_rect(const Gp0Mode *m, const Gp0Vtx *v, int w, int h) {
    Buffer *b = target();
    if (b && b == displayed() && ui_defer(UI_RECT, m, v, 1, w, h)) return;
    ui_flush();
    /* A sprite or tile in the scene is a quad of the 3D layer like a polygon (so it blends with it), unless it
     * uses a texture window or its texels wrap past the page edge, which 8-bit coordinates can't say. */
    if (b && b != displayed() && !gp0_state.twin_mask_x && !gp0_state.twin_mask_y &&
        (!m->textured || (v->u + w <= 256 && v->v + h <= 256))) {
        /* clipped to the drawing area: texels follow pixels one to one */
        Gp0Vtx c = *v;
        int dx = gp0_state.area_x0 - c.x, dy = gp0_state.area_y0 - c.y;
        if (dx > 0) { c.x += dx; c.u += dx; w -= dx; }
        if (dy > 0) { c.y += dy; c.v += dy; h -= dy; }
        if (c.x + w > gp0_state.area_x1 + 1) w = gp0_state.area_x1 + 1 - c.x;
        if (c.y + h > gp0_state.area_y1 + 1) h = gp0_state.area_y1 + 1 - c.y;
        if (w <= 0 || h <= 0) return;
        Gp0Vtx q[4] = {c, c, c, c};
        q[1].x += w; q[1].u += w - 1;
        q[2].y += h; q[2].v += h - 1;
        q[3].x += w; q[3].u += w - 1; q[3].y += h; q[3].v += h - 1;
        Gp0Mode flat = *m;
        flat.gouraud = 0;
        int before = b->count;
        record(b, &flat, q, 4);
        if (b->count > before) b->prims[b->count - 1].flags |= P_RECT;
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
        if (b->shown && b != displayed()) start_scene(b);
        b->clear = rgb15;
    }
}

/* ---- presentation ------------------------------------------------------------------------------------------ */

#define BLOCK 32
static u8 block_shown[6][8];   /* the 2D layer has pixels in this 32x32 output block */
static Buffer *last_shown;
static u32 block_hash[6][8];
static int layer_bg = -1;
static void reset_colours(void);     /* the 2D layer: an 8-bit bitmap BG in bank E with a palette built as colours appear */

void gxh_init(void) {
    /* in PS1 RAM the port never uses (main's MIPS code range), see psx_spare_alloc */
    for (int i = 0; i < 2; ++i) buffers[i].prims = psx_spare_alloc(MAX_PRIMS * sizeof(Prim));
    marks = psx_spare_alloc(1024 * 512 / 8);
    memset(marks, 0, 1024 * 512 / 8);
}

int gxh_active(void) { return active; }

void gxh_enable(int on) {
    if (on == active) return;
    active = on;
    if (on) {
        memset(block_shown, 0, sizeof block_shown);
        memset(block_hash, 0, sizeof block_hash);
        last_shown = NULL;
        videoSetMode(MODE_5_3D);
        vramSetPrimaryBanks(VRAM_A_TEXTURE, VRAM_B_TEXTURE, VRAM_C_TEXTURE, VRAM_D_TEXTURE);
        vramSetBankE(VRAM_E_MAIN_BG);
        vramSetBankF(VRAM_F_TEX_PALETTE_SLOT0);
        vramSetBankG(VRAM_G_TEX_PALETTE_SLOT1);
        layer_bg = bgInit(2, BgType_Bmp8, BgSize_B8_256x256, 0, 0);
        bgSetRotateScale(layer_bg, 0, 256, 256);
        bgSetPriority(layer_bg, 0);
        bgUpdate();
        memset((void *)BG_BMP_RAM(0), 0, 256 * 256);
        reset_colours();
        glInit();
        glEnable(GL_TEXTURE_2D | GL_BLEND);
        glClearPolyID(63);
        glClearDepth(GL_MAX_DEPTH);
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        bgSetPriority(0, 1);
        memset(regions, 0, sizeof regions);
        memset(unit_used, 0, sizeof unit_used);
        unit_used[RAMP_UNIT] = 1;
        ramp_written = 0;
        memset(palettes, 0, sizeof palettes);
        for (int i = 0; i < 2; ++i) { buffers[i].valid = 0; buffers[i].count = 0; }
        gp0_backend = &hybrid_backend;
    } else {
        if (layer_bg >= 0) bgHide(layer_bg);
        gp0_backend = &psx_softgpu;
    }
}

/* Send a buffer's scene to the 3D engine; it is shown from the next frame on. */
static void submit(Buffer *b, int display_h) {
    unsigned int profile = port_profile_begin();
    while (dmaBusy(0)) {} /* the previous scene's list */
    plan_textures(b);
    gxh_stats_regions = gxh_stats_units = 0;
    for (int i = 0; i < MAX_REGIONS; ++i) if (regions[i].used) { gxh_stats_regions++; gxh_stats_units += regions[i].units; }
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
static u8 cell_dirty[512 / 8][1024 / 8];
static int last_x, last_y, last_w, last_h;
static u8 *colour_map;          /* 32768 RGB15 -> palette index (0: not mapped yet) */
static int colours, colours_full;

/* Palette: 1-64 a fixed 4x4x4 colour cube for software glows (gradients with many shades), 65-255 exact colours
 * as they appear (the battle UI uses under 200). */
#define CUBE 64
static const u8 cube_level[4] = {0, 10, 21, 31};

static void reset_colours(void) {
    if (!colour_map) colour_map = psx_spare_alloc(32768);
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
        for (int x = x0 >> 3; x <= x1 >> 3; ++x) cell_dirty[y & 63][x & 127] = 1;
}

void gxh_present(const PsxDisplayInfo *d, void (*scale)(u16 *dst, int x, int y, int w, int h)) {
    ui_flush();
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
    static u16 block[BLOCK * 256] __attribute__((aligned(4)));   /* a scaled block, stride 256 */
    u16 *bitmap = (u16 *)BG_BMP_RAM(0);
    int shown = 0;
    for (int by = 0, row = 0; by < out_h; by += BLOCK, ++row)
        for (int bx = 0, col = 0; bx < out_w; bx += BLOCK, ++col) {
            int sx0 = d->x + bx * 5 / 4, sy0 = d->y + by * 5 / 4;
            int bw = (bx + BLOCK < out_w ? BLOCK : out_w - bx), bh = (by + BLOCK < out_h ? BLOCK : out_h - by);
            int sw = bw * 5 / 4, sh = bh * 5 / 4;
            if (!check_all) {
                int dirty = 0;
                for (int y = sy0 >> 3; y <= (sy0 + sh) >> 3 && !dirty; ++y)
                    for (int x = sx0 >> 3; x <= (sx0 + sw) >> 3; ++x)
                        if (cell_dirty[y & 63][x & 127]) { dirty = 1; break; }
                if (!dirty) { shown += block_shown[row][col]; continue; }
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
                    u16 i0 = shows(vrow[a], clear, ma) ? ma ? cube_of(frow[ox]) : colour_of(frow[ox]) : 0;
                    u16 i1 = ox + 1 < bw && shows(vrow[b2], clear, mb)
                                 ? mb ? cube_of(frow[ox + 1]) : colour_of(frow[ox + 1]) : 0;
                    out[ox / 2] = i0 | i1 << 8; /* halfword stores: VRAM ignores byte writes */
                }
            }
            shown += !!any;
        }
    for (int y = (d->y >> 3); y <= (d->y + h) >> 3; ++y)
        for (int x = (d->x >> 3); x <= (d->x + w) >> 3; ++x) cell_dirty[y & 63][x & 127] = 0;
    gxh_stats_blocks = colours_full ? -colours : colours;
    (void)shown;
}
#endif
