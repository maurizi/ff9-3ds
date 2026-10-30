/* Host diagnostic (FF9_OCC=1 with --render): how many opaque primitives end up fully hidden by later ones (the PS1
 * draws in OT order, no depth buffer), an upper bound on what an occlusion cull could save. A wrapper of the software
 * renderer's backend keeps a primitive id per VRAM pixel: each opaque primitive's covered pixels take its id
 * (textures counted solid: the bound is high where texels are transparent), semi-transparent ones take none, a fill
 * clears. When the display shows a buffer, the opaque primitives drawn into it since it was last shown that own no
 * pixel of it were hidden. Totals on stderr every 600 frames and at exit. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gp0.h"
#include "psx_raster.h"
#include "psx_display.h"

#define W 1024
#define H 512
#define MAXP (1 << 20)
static unsigned *ids;                 /* per VRAM pixel: the last opaque primitive's id (1-based), 0 none */
static struct { short x, y; unsigned char poly, opaque, empty; } *prims; /* by id: where it was drawn (a vertex), its kind */
static unsigned nprims;
static struct { short x[4], y[4]; unsigned char n, opaque; } *geo;
static unsigned long long blk_culled, blk_total;

static unsigned char *owned;
static unsigned long long tot[2], hid[2], empty[2]; /* [0] rectangles, [1] polygons; empty: no pixel at all */
static unsigned drawn; /* (the current primitive's pixels) */
static unsigned frames;
static int occ_mode;

static void put(int x, int y, unsigned id) {
    if (x < gp0_state.area_x0 || x > gp0_state.area_x1 || y < gp0_state.area_y0 || y > gp0_state.area_y1) return;
    ids[(y & (H - 1)) * W + (x & (W - 1))] = id;
    drawn++;
}
static long edge(int ax, int ay, int bx, int by, int px, int py) { return (long)(bx - ax) * (py - ay) - (long)(by - ay) * (px - ax); }
static void tri(const Gp0Vtx *a, const Gp0Vtx *b, const Gp0Vtx *c, unsigned id) {
    int x0 = a->x, x1 = a->x, y0 = a->y, y1 = a->y;
    const Gp0Vtx *v[3] = {a, b, c};
    for (int i = 1; i < 3; ++i) {
        if (v[i]->x < x0) x0 = v[i]->x;
        if (v[i]->x > x1) x1 = v[i]->x;
        if (v[i]->y < y0) y0 = v[i]->y;
        if (v[i]->y > y1) y1 = v[i]->y;
    }
    if (x1 - x0 > 1023 || y1 - y0 > 511) return; /* (the PS1 skips those) */
    long area = edge(a->x, a->y, b->x, b->y, c->x, c->y);
    if (!area) return;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) {
            long e0 = edge(a->x, a->y, b->x, b->y, x, y), e1 = edge(b->x, b->y, c->x, c->y, x, y),
                 e2 = edge(c->x, c->y, a->x, a->y, x, y);
            if (area > 0 ? (e0 >= 0 && e1 >= 0 && e2 >= 0) : (e0 <= 0 && e1 <= 0 && e2 <= 0)) put(x, y, id);
        }
}
static unsigned note(const Gp0Mode *m, int x, int y, int poly) {
    if (nprims + 1 >= MAXP) return 0;
    unsigned id = ++nprims;
    prims[id].x = (short)x; prims[id].y = (short)y; prims[id].poly = (unsigned char)poly;
    prims[id].opaque = !m->semi;
    return m->semi ? 0 : id;
}
static void occ_poly(const Gp0Mode *m, const Gp0Vtx *v, int n) {
    psx_softgpu.poly(m, v, n);
    unsigned id = note(m, v[0].x, v[0].y, 1);
    if (!id) return;
    drawn = 0;
    tri(&v[0], &v[1], &v[2], id);
    if (n == 4) tri(&v[1], &v[2], &v[3], id);
    prims[id].empty = !drawn;
    geo[id].n = (unsigned char)n;
    for (int k = 0; k < n; ++k) { geo[id].x[k] = (short)v[k].x; geo[id].y[k] = (short)v[k].y; }
}
static void occ_line(const Gp0Mode *m, const Gp0Vtx *a, const Gp0Vtx *b) { psx_softgpu.line(m, a, b); }
static void occ_rect(const Gp0Mode *m, const Gp0Vtx *v, int w, int h) {
    psx_softgpu.rect(m, v, w, h);
    unsigned id = note(m, v->x, v->y, 0);
    if (!id) return;
    /* (FF9_OCC=2: rectangles don't hide what's under them: the battle UI's windows are the DS's 2D layer, the
     * bottom screen's later) */
    if (occ_mode == 2) { prims[id].opaque = 0; return; }
    for (int y = v->y; y < v->y + h; ++y) for (int x = v->x; x < v->x + w; ++x) put(x, y, id);
}
static void occ_fill(int x, int y, int w, int h, unsigned rgb) {
    if (psx_softgpu.fill) psx_softgpu.fill(x, y, w, h, rgb);
    for (int yy = y; yy < y + h; ++yy) for (int xx = x; xx < x + w; ++xx) ids[(yy & (H - 1)) * W + (xx & (W - 1))] = 0;
}
static void report(void) {
    fprintf(stderr, "occlusion: rectangles %llu, hidden %llu (%.1f%%); polygons %llu, no pixel %llu (%.1f%%), hidden by "
            "later ones %llu (%.1f%%)\n", tot[0], hid[0], tot[0] ? 100.0 * hid[0] / tot[0] : 0.0, tot[1], empty[1],
            tot[1] ? 100.0 * empty[1] / tot[1] : 0.0, hid[1], tot[1] ? 100.0 * hid[1] / tot[1] : 0.0);
    fprintf(stderr, "occlusion: pixel cull, tested a pixel wider (nearest first): %llu of %llu polygons (%.1f%%)\n", blk_culled, blk_total,
            blk_total ? 100.0 * blk_culled / blk_total : 0.0);
}
/* the conservative block cull as the DS would do it: per scene, the polygons nearest first (reverse draw order),
 * 8x8 blocks wholly inside an opaque polygon covered, a polygon whose bounding box's blocks are all covered culled */
static int inside_tri(int px, int py, const short *x, const short *y, int a, int b, int c) {
    long e0 = (long)(x[b] - x[a]) * (py - y[a]) - (long)(y[b] - y[a]) * (px - x[a]);
    long e1 = (long)(x[c] - x[b]) * (py - y[b]) - (long)(y[c] - y[b]) * (px - x[b]);
    long e2 = (long)(x[a] - x[c]) * (py - y[c]) - (long)(y[a] - y[c]) * (px - x[c]);
    return (e0 >= 0 && e1 >= 0 && e2 >= 0) || (e0 <= 0 && e1 <= 0 && e2 <= 0);
}
static int inside_poly(int px, int py, unsigned i) {
    if (inside_tri(px, py, geo[i].x, geo[i].y, 0, 1, 2)) return 1;
    return geo[i].n == 4 && inside_tri(px, py, geo[i].x, geo[i].y, 1, 2, 3);
}
/* (per pixel: a polygon's pixels as the PS1 rule fills them, `grow` pixels more around it for the test) */
static unsigned char pcov[512][1024];
static int poly_pixels(unsigned i, const PsxDisplayInfo *d, int grow, int mark) {
    int x0 = geo[i].x[0], x1 = x0, y0 = geo[i].y[0], y1 = y0;
    for (int k = 1; k < geo[i].n; ++k) {
        if (geo[i].x[k] < x0) x0 = geo[i].x[k];
        if (geo[i].x[k] > x1) x1 = geo[i].x[k];
        if (geo[i].y[k] < y0) y0 = geo[i].y[k];
        if (geo[i].y[k] > y1) y1 = geo[i].y[k];
    }
    int any = 0;
    for (int y = y0 - grow; y < y1 + grow; ++y)
        for (int x = x0 - grow; x < x1 + grow; ++x) {
            if (x < d->x || y < d->y || x >= d->x + d->w || y >= d->y + d->h) continue;
            int in = 0;
            for (int dy = -grow; dy <= grow && !in; ++dy)
                for (int dx = -grow; dx <= grow && !in; ++dx) in = inside_poly(x + dx, y + dy, i);
            if (!in) continue;
            any = 1;
            if (mark) pcov[y & 511][x & 1023] = 1;
            else if (!pcov[y & 511][x & 1023]) return 0; /* (a pixel not covered: visible) */
        }
    return mark ? any : 1;
}
static void block_model(const PsxDisplayInfo *d) {
    for (int y = d->y; y < d->y + d->h; ++y) memset(&pcov[y & 511][d->x], 0, (size_t)d->w);
    for (unsigned i = nprims; i >= 1; --i) {
        if (!prims[i].poly || !prims[i].opaque) continue;
        if (prims[i].x < d->x - 64 || prims[i].x >= d->x + d->w + 64 || prims[i].y < d->y - 64 || prims[i].y >= d->y + d->h + 64) continue;
        blk_total++;
        if (poly_pixels(i, d, 1, 0)) { blk_culled++; continue; }
        poly_pixels(i, d, 0, 1);
    }
}
static void occ_frame(void) {
    if (psx_softgpu.frame) psx_softgpu.frame();
    static PsxDisplayInfo last;
    PsxDisplayInfo d;
    psx_gpu_display_info(&d);
    if (d.w > 0 && (d.x != last.x || d.y != last.y)) { /* a buffer newly shown: its primitives since last time */
        memset(owned, 0, nprims + 1);
        for (int y = d.y; y < d.y + d.h; ++y)
            for (int x = d.x; x < d.x + d.w; ++x) owned[ids[(y & (H - 1)) * W + (x & (W - 1))]] = 1;
        for (unsigned i = 1; i <= nprims; ++i) {
            if (!prims[i].opaque) continue;
            if (prims[i].x < d.x - 64 || prims[i].x >= d.x + d.w + 64 || prims[i].y < d.y - 64 || prims[i].y >= d.y + d.h + 64)
                continue; /* (another buffer's) */
            tot[prims[i].poly]++;
            if (prims[i].empty) empty[prims[i].poly]++;
            else if (!owned[i]) hid[prims[i].poly]++;
        }
        block_model(&d);
        nprims = 0;
        memset(ids, 0, (size_t)W * H * sizeof *ids);
    }
    last = d;
    if (++frames % 600 == 0) report();
}
static const Gp0Backend occ_backend = {occ_poly, occ_line, occ_rect, occ_fill, occ_frame, 0};

void occ_stats_install(void) {
    if (!getenv("FF9_OCC")) return;
    occ_mode = atoi(getenv("FF9_OCC"));
    ids = calloc((size_t)W * H, sizeof *ids);
    prims = calloc(MAXP, sizeof *prims);
    owned = calloc(MAXP, 1);
    geo = calloc(MAXP, sizeof *geo);
    if (!ids || !prims || !owned) return;
    gp0_backend = &occ_backend;
    atexit(report);
}
