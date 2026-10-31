/* The battle UI's VBlank list (btlui_submitOt: about 110 primitives, every VBlank) drawn natively.
 *
 * The list's ordering table is walked here instead of through the GP0 decoder and the renderer, and its
 * primitives (sprites from the UI's 4-bit pages, tiles, untextured gradient quads and lines) are rasterised with
 * the software GPU's arithmetic (port/psx/softgpu.c: modulation, blending, dithering, edge rules) into a band of
 * RGB15 pixels, then turned into the canvas's 8-bit palette indices. Over the cleared canvas, not the scene: the
 * game's row cache (the DR_MOVEs to (960, 432) and the sprites drawing them back), which only restores the scene
 * under changing digits and icons, isn't needed and isn't drawn.
 *
 * Each list is compared with the previous one, record by record; only the canvas areas of records that differ are
 * cleared and drawn again (every record over them, in order, clipped to them: exact, blending included). A list
 * that is the same as the last draws nothing.
 *
 * The layout (canvas = screen + a shift per primitive, by where on the PS1's screen it is) places the help window,
 * the party panel and the command/list windows at 1:1 on a 256x192 canvas. */
#include <string.h>
#include "btlui_native.h"
#include "psx_mem.h"
#include "native_mesh.h"

/* battle only: in ovl_0b's dead MIPS code range (no resident memory) */
#define NUI_CODE PORT_RAM_CODE(0b) __attribute__((optimize("Os")))
#define NUI_HOT PORT_RAM_CODE(0b) /* (the per-pixel loops) */
#if defined(__arm__)
/* Thumb: half the size in slot 0 (which ovl_0b sizes, out of the heap); no 64-bit arithmetic here */
#pragma GCC target("thumb")
#endif

extern uint16_t *psx_vram;

typedef uint8_t u8;
typedef uint16_t u16;
typedef int16_t s16;
typedef uint32_t u32;

enum { K_TILE, K_SPRT, K_POLY, K_LINE };
enum { F_SEMI = 1, F_RAW = 2, F_GOURAUD = 4, F_DITHER = 8 };

/* one primitive, as compared between lists (every byte set: memcmp) */
typedef struct {
    u8 kind, flags, abr, n;
    u8 depth, phase, twin_mx, twin_my;  /* phase: the drawing offset's low bits (dithering's VRAM position) */
    u8 twin_ox, twin_oy, canvas, pad;   /* canvas: 0 the UI's, 1 the top screen's (target cursors) */
    s16 dx, dy;                         /* the layout's shift: canvas = screen + (dx, dy) */
    s16 clip[4];                        /* the drawing area, screen x0, y0, x1, y1 inclusive */
    s16 bb[4];                          /* canvas pixels it may cover, inclusive */
    u16 tx, ty, cx, cy;                 /* texture page, CLUT */
    s16 x[4], y[4];                     /* screen vertices; sprites and tiles: x[1], y[1] = width, height */
    u8 r[4], g[4], b[4], u, v;
} NuiPrim;

static NuiCanvas cv;
static NuiPrim *lists[2];               /* [0] the previous list, [1] this one */
static int counts[2], max_prims;
static int redraw_all[2] = {1, 1};
static int top_y;                       /* the top canvas: where the scene's first row is */
static unsigned int src_writes, src_seen;
static int lists_since_frame;
unsigned int nui_stats_drawn, nui_stats_pixels, nui_stats_unknown, nui_stats_max;

/* ---- palette ---------------------------------------------------------------------------------------------- */

#define HASH 512
#define RECTS 24
/* the larger tables, in memory the platform lends (no resident memory) */
typedef struct {
    u16 keys[HASH];                     /* RGB15 colours (0: empty: black is index 0) */
    u8 vals[HASH];
    u16 pal[256];                       /* index -> colour (0: free, but for index 0) */
    u16 stamp[256];                     /* the list that last used the index */
    u8 free_list[256];
    u8 shown[256];
    s16 dirty[2][RECTS][4];
    u16 top_pal[256];                   /* the top canvas's colours (a few: a linear search) */
    u32 lut[256];                       /* a sprite's CLUT, raw low and modulated high */
} Tables;
static Tables *T;
const unsigned int nui_tables_size = sizeof(Tables);
static u16 list_no;
static int free_count, palette_full; /* palette_full: compacted during this list */
static u16 memo_c = 0xFFFF;
static u8 memo_i;

NUI_CODE static void hash_put(u16 c, u8 i) {
    unsigned int h = ((unsigned int)c * 40503u >> 6) & (HASH - 1);
    while (T->keys[h]) h = (h + 1) & (HASH - 1);
    T->keys[h] = c;
    T->vals[h] = i;
}

NUI_CODE static void palette_reset(void) {
    memset(T->keys, 0, sizeof T->keys);
    memset(T->pal, 0, sizeof T->pal);
    free_count = 0;
    for (int i = 255; i >= 1; --i) T->free_list[free_count++] = (u8)i;
    palette_full = 0;
    memo_c = 0xFFFF;
    if (cv.palette) cv.palette(0, 0);
}

/* Full: the entries no canvas pixel shows and this list hasn't used are freed (a gauge's pulse goes through more
 * colours than the palette holds, a few at a time). */
NUI_CODE static void palette_compact(void) {
    u8 *shown = T->shown;
    memset(shown, 0, sizeof T->shown);
    for (int y = 0; y < cv.h; ++y) {
        const u8 *row = cv.pixels + y * cv.pitch;
        for (int x = 0; x < cv.w; ++x) shown[row[x]] = 1;
    }
    memset(T->keys, 0, sizeof T->keys);
    for (int i = 1; i < 256; ++i) {
        if (!T->pal[i]) continue;
        if (shown[i] || T->stamp[i] == list_no) hash_put(T->pal[i], (u8)i);
        else { T->pal[i] = 0; T->free_list[free_count++] = (u8)i; }
    }
    memo_c = 0xFFFF;
}

NUI_CODE static u8 nearest(u16 c) {
    int best = 0, best_d = 1 << 30;
    int r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    for (int i = 0; i < 256; ++i) {
        if (i && !T->pal[i]) continue;
        int dr = r - (T->pal[i] & 31), dg = g - ((T->pal[i] >> 5) & 31), db = b - ((T->pal[i] >> 10) & 31);
        int d = dr * dr + dg * dg + db * db;
        if (d < best_d) { best_d = d; best = i; }
    }
    return (u8)best;
}

NUI_HOT static u8 index_of(u16 c) {
    if (c == memo_c) return memo_i;
    memo_c = c;
    if (!c) return memo_i = 0;
    unsigned int h = ((unsigned int)c * 40503u >> 6) & (HASH - 1);
    while (T->keys[h]) {
        if (T->keys[h] == c) { T->stamp[T->vals[h]] = list_no; return memo_i = T->vals[h]; }
        h = (h + 1) & (HASH - 1);
    }
    if (!free_count && !palette_full) { palette_compact(); palette_full = 1; }
    if (!free_count) { memo_c = 0xFFFF; return nearest(c); }
    const u8 i = T->free_list[--free_count];
    memo_c = c; /* (palette_compact cleared it) */
    hash_put(c, i);
    T->pal[i] = c;
    T->stamp[i] = list_no;
    cv.palette(i, c);
    return memo_i = i;
}

static int top_colours, top_full;
static u16 top_memo_c = 0xFFFF;
static u8 top_memo_i;

NUI_CODE static u8 top_index_of(u16 c) {
    if (c == top_memo_c) return top_memo_i;
    top_memo_c = c;
    if (!c) return top_memo_i = 0;
    for (int i = 1; i <= top_colours; ++i)
        if (T->top_pal[i] == c) return top_memo_i = (u8)i;
    if (top_colours == 255) { /* (redrawn with a new palette at the next list) */
        top_full = 1;
        int best = 1, best_d = 1 << 30;
        for (int i = 1; i <= 255; ++i) {
            int dr = (c & 31) - (T->top_pal[i] & 31), dg = ((c >> 5) & 31) - ((T->top_pal[i] >> 5) & 31),
                db = ((c >> 10) & 31) - ((T->top_pal[i] >> 10) & 31), d = dr * dr + dg * dg + db * db;
            if (d < best_d) { best_d = d; best = i; }
        }
        return top_memo_i = (u8)best;
    }
    T->top_pal[++top_colours] = c;
    cv.top_palette(top_colours, c);
    return top_memo_i = (u8)top_colours;
}

/* ---- rasterising (softgpu.c's arithmetic) ------------------------------------------------------------------ */

static u16 *band;                       /* the band's pixels: canvas (band_x, band_y) at band[0], stride cv.w */
static int band_x, band_y;

static inline u32 sat_add15(u32 a, u32 b) {
    u32 sum = a + b, carry = (sum - ((a ^ b) & 0x0421)) & 0x8420;
    return ((sum - carry) | (carry - (carry >> 5))) & 0x7FFF;
}
static inline u16 blend(u16 back, u16 front, int abr) {
    u32 b = back & 0x7FFF, f = front & 0x7FFF;
    switch (abr) {
    case 0: return (u16)((((b & 0x7BDE) + (f & 0x7BDE)) >> 1) + (b & f & 0x0421));
    case 1: return (u16)sat_add15(b, f);
    case 2: return (u16)(sat_add15(b ^ 0x7FFF, f) ^ 0x7FFF);
    default: return (u16)sat_add15(b, (f >> 2) & 0x1CE7);
    }
}
static inline u16 dither_colour(int r, int g, int b, int x, int y) {
    static const signed char offsets[16] = {-4, 0, -3, 1, 2, -2, 3, -1, -3, 1, -4, 0, 3, -1, 2, -2};
    int bias = offsets[((y & 3) << 2) | (x & 3)];
    r += bias; g += bias; b += bias;
    r = r < 0 ? 0 : r > 255 ? 255 : r;
    g = g < 0 ? 0 : g > 255 ? 255 : g;
    b = b < 0 ? 0 : b > 255 ? 255 : b;
    return (u16)((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10));
}

/* the band pixel of screen (x, y) for p */
#define AT(p, x, y) (band[((y) + (p)->dy - band_y) * cv.w + (x) + (p)->dx - band_x])

/* an untextured pixel (plot) */
NUI_CODE static void plot(const NuiPrim *p, int x, int y, int r, int g, int b, int dither) {
    r = r < 0 ? 0 : r > 255 ? 255 : r;
    g = g < 0 ? 0 : g > 255 ? 255 : g;
    b = b < 0 ? 0 : b > 255 ? 255 : b;
    u16 c = dither ? dither_colour(r, g, b, x + (p->phase & 3), y + (p->phase >> 2))
                   : (u16)((r >> 3) | (g >> 3) << 5 | (b >> 3) << 10);
    u16 *d = &AT(p, x, y);
    *d = p->flags & F_SEMI ? blend(*d, c, p->abr) : c;
}

NUI_HOT static void draw_tile(const NuiPrim *p, int x0, int y0, int x1, int y1) {
    u16 c = (u16)((p->r[0] >> 3) | (p->g[0] >> 3) << 5 | (p->b[0] >> 3) << 10);
    for (int y = y0; y <= y1; ++y) {
        u16 *d = &AT(p, x0, y);
        if (p->flags & F_SEMI) for (int x = x0; x <= x1; ++x, ++d) *d = blend(*d, c, p->abr);
        else for (int x = x0; x <= x1; ++x) *d++ = c;
    }
}

NUI_HOT static void draw_sprt(const NuiPrim *p, int x0, int y0, int x1, int y1) {
    const int px = p->x[0], py = p->y[0];
    const unsigned keep_x = ~((unsigned)p->twin_mx * 8u), keep_y = ~((unsigned)p->twin_my * 8u);
    const unsigned win_x = (p->twin_ox & p->twin_mx) * 8u, win_y = (p->twin_oy & p->twin_my) * 8u;
    const int r = p->r[0], g = p->g[0], b = p->b[0];
    const int plain = (p->flags & F_RAW) || (r == 128 && g == 128 && b == 128);
    const int semi = p->flags & F_SEMI;
    /* the CLUT's texels, raw (transparency and STP) low and modulated high, as softgpu's prepare_palette */
    u32 *lut = T->lut;
    const int entries = p->depth == 0 ? 16 : p->depth == 1 ? 256 : 0;
    for (int i = 0; i < entries; ++i) {
        u16 t = psx_vram[(p->cy & 511) * 1024 + ((p->cx + i) & 1023)], c = t;
        if (!plain) {
            int tr = (t & 31) * r >> 7, tg = ((t >> 5) & 31) * g >> 7, tb = ((t >> 10) & 31) * b >> 7;
            c = (u16)((tr > 31 ? 31 : tr) | (tg > 31 ? 31 : tg) << 5 | (tb > 31 ? 31 : tb) << 10 | (t & 0x8000));
        }
        lut[i] = t | (u32)c << 16;
    }
    for (int y = y0; y <= y1; ++y) {
        unsigned v = ((((unsigned)p->v + (unsigned)(y - py)) & keep_y) | win_y) & 255;
        const u16 *row = psx_vram + ((p->ty + v) & 511) * 1024;
        u16 *d = &AT(p, x0, y);
        for (int x = x0; x <= x1; ++x, ++d) {
            unsigned u = ((((unsigned)p->u + (unsigned)(x - px)) & keep_x) | win_x) & 255;
            u32 s;
            if (p->depth == 0) s = lut[(row[(p->tx + (u >> 2)) & 1023] >> ((u & 3) * 4)) & 15];
            else if (p->depth == 1) s = lut[(row[(p->tx + (u >> 1)) & 1023] >> ((u & 1) * 8)) & 255];
            else {
                u16 t = row[(p->tx + u) & 1023], c = t;
                if (!plain) {
                    int tr = (t & 31) * r >> 7, tg = ((t >> 5) & 31) * g >> 7, tb = ((t >> 10) & 31) * b >> 7;
                    c = (u16)((tr > 31 ? 31 : tr) | (tg > 31 ? 31 : tg) << 5 | (tb > 31 ? 31 : tb) << 10);
                }
                s = t | (u32)c << 16;
            }
            if (!(u16)s) continue;
            u16 c = (u16)(s >> 16);
            *d = semi && (s & 0x8000) ? blend(*d, c, p->abr) : c;
        }
    }
}

typedef struct { int x, y, r, g, b; } V;

/* softgpu.c triangle()'s untextured path (its reference arithmetic), clipped to x0..x1, y0..y1 inclusive */
NUI_CODE static void triangle(const NuiPrim *p, const V *a, const V *b, const V *c, int x0, int y0, int x1, int y1) {
    const V *anchor = b->x <= a->x ? b : a;
    if (c->x < anchor->x || (anchor == b && c->x == anchor->x)) anchor = c;
    int area = (b->x - a->x) * (c->y - a->y) - (b->y - a->y) * (c->x - a->x);
    if (area == 0) return;
    if (area < 0) { const V *t = b; b = c; c = t; area = -area; }
    int minx = a->x, maxx = a->x, miny = a->y, maxy = a->y;
    const V *vs[3] = {a, b, c};
    for (int i = 1; i < 3; i++) {
        if (vs[i]->x < minx) minx = vs[i]->x;
        if (vs[i]->x > maxx) maxx = vs[i]->x;
        if (vs[i]->y < miny) miny = vs[i]->y;
        if (vs[i]->y > maxy) maxy = vs[i]->y;
    }
    if (maxx - minx >= 1024 || maxy - miny >= 512) return;
    if (minx < x0) minx = x0;
    if (miny < y0) miny = y0;
    if (maxx > x1 + 1) maxx = x1 + 1;
    if (maxy > y1 + 1) maxy = y1 + 1;
    if (minx >= maxx || miny >= maxy) return;
    const int gouraud = p->flags & F_GOURAUD, dither = p->flags & F_DITHER;
    int A0 = b->y - c->y, B0 = c->x - b->x, A1 = c->y - a->y, B1 = a->x - c->x, A2 = a->y - b->y, B2 = b->x - a->x;
    int w0row = A0 * (minx - b->x) + B0 * (miny - b->y);
    int w1row = A1 * (minx - c->x) + B1 * (miny - c->y);
    int w2row = A2 * (minx - a->x) + B2 * (miny - a->y);
    int bias0 = (A0 > 0 || (A0 == 0 && B0 > 0)) ? 0 : -1;
    int bias1 = (A1 > 0 || (A1 == 0 && B1 > 0)) ? 0 : -1;
    int bias2 = (A2 > 0 || (A2 == 0 && B2 > 0)) ? 0 : -1;
#define GRAD(fa, fb, fc) \
    (int)((unsigned)((((fa) * A0 + (fb) * A1 + (fc) * A2) * 4096) / area) * 16u), \
    (int)((unsigned)((((fa) * B0 + (fb) * B1 + (fc) * B2) * 4096) / area) * 16u)
    int drdx = 0, drdy = 0, dgdx = 0, dgdy = 0, dbdx = 0, dbdy = 0;
    if (gouraud) {
        int tr[2] = {GRAD(a->r, b->r, c->r)}, tg[2] = {GRAD(a->g, b->g, c->g)}, tb[2] = {GRAD(a->b, b->b, c->b)};
        drdx = tr[0]; drdy = tr[1]; dgdx = tg[0]; dgdy = tg[1]; dbdx = tb[0]; dbdy = tb[1];
    }
#undef GRAD
#define SEED(value, dx, dy) \
    (int)(((unsigned)(value) << 16) + 0x8000u + (unsigned)(dx) * (unsigned)(minx - anchor->x) + \
          (unsigned)(dy) * (unsigned)(miny - anchor->y))
    int rrow = SEED(anchor->r, drdx, drdy), grow = SEED(anchor->g, dgdx, dgdy), brow = SEED(anchor->b, dbdx, dbdy);
#undef SEED
    for (int y = miny; y < maxy; y++) {
        int w0 = w0row, w1 = w1row, w2 = w2row, r = rrow, g = grow, bl = brow;
        for (int x = minx; x < maxx; x++) {
            if (w0 + bias0 >= 0 && w1 + bias1 >= 0 && w2 + bias2 >= 0) {
                if (gouraud) plot(p, x, y, r >> 16, g >> 16, bl >> 16, dither);
                else plot(p, x, y, a->r, a->g, a->b, dither);
            }
            w0 += A0; w1 += A1; w2 += A2;
            r = (int)((unsigned)r + (unsigned)drdx); g = (int)((unsigned)g + (unsigned)dgdx);
            bl = (int)((unsigned)bl + (unsigned)dbdx);
        }
        w0row += B0; w1row += B1; w2row += B2;
        rrow = (int)((unsigned)rrow + (unsigned)drdy); grow = (int)((unsigned)grow + (unsigned)dgdy);
        brow = (int)((unsigned)brow + (unsigned)dbdy);
    }
}

/* A quad that is a rectangle (v0 top left, v1 top right, v2 bottom left, v3 bottom right) shaded left to right only:
 * both triangles' arithmetic comes down to the same ramp along each row (the slope's quotient divides by the height
 * exactly, the vertical slopes are 0, and the top-left rule gives the shared diagonal's pixels to one of them). */
NUI_HOT static int rect_ramp(const NuiPrim *p, int x0, int y0, int x1, int y1) {
    const int lx = p->x[0], rx = p->x[1], ty = p->y[0], by = p->y[2];
    if (p->n != 4 || p->x[2] != lx || p->x[3] != rx || p->y[1] != ty || p->y[3] != by || rx <= lx || by <= ty ||
        rx - lx >= 1024 || by - ty >= 512)
        return 0;
    const int gouraud = p->flags & F_GOURAUD;
    if (gouraud && (p->r[0] != p->r[2] || p->g[0] != p->g[2] || p->b[0] != p->b[2] || p->r[1] != p->r[3] ||
                    p->g[1] != p->g[3] || p->b[1] != p->b[3]))
        return 0;
    if (x0 < lx) x0 = lx;
    if (y0 < ty) y0 = ty;
    if (x1 > rx - 1) x1 = rx - 1;
    if (y1 > by - 1) y1 = by - 1;
    if (x1 < x0 || y1 < y0) return 1;
    const int w = rx - lx, dither = p->flags & F_DITHER, semi = p->flags & F_SEMI;
    int drdx = 0, dgdx = 0, dbdx = 0;
    if (gouraud) {
        drdx = (int)((unsigned)((p->r[1] - p->r[0]) * 4096 / w) * 16u);
        dgdx = (int)((unsigned)((p->g[1] - p->g[0]) * 4096 / w) * 16u);
        dbdx = (int)((unsigned)((p->b[1] - p->b[0]) * 4096 / w) * 16u);
    }
    const int skip = x0 - lx;
    const int r0 = (int)(((unsigned)p->r[0] << 16) + 0x8000u + (unsigned)drdx * (unsigned)skip);
    const int g0 = (int)(((unsigned)p->g[0] << 16) + 0x8000u + (unsigned)dgdx * (unsigned)skip);
    const int b0 = (int)(((unsigned)p->b[0] << 16) + 0x8000u + (unsigned)dbdx * (unsigned)skip);
    for (int y = y0; y <= y1; ++y) {
        u16 *d = &AT(p, x0, y);
        int r = r0, g = g0, b = b0;
        for (int x = x0; x <= x1; ++x, ++d) {
            int cr = r >> 16, cg = g >> 16, cb = b >> 16;
            cr = cr < 0 ? 0 : cr > 255 ? 255 : cr;
            cg = cg < 0 ? 0 : cg > 255 ? 255 : cg;
            cb = cb < 0 ? 0 : cb > 255 ? 255 : cb;
            u16 c = dither ? dither_colour(cr, cg, cb, x + (p->phase & 3), y + (p->phase >> 2))
                           : (u16)((cr >> 3) | (cg >> 3) << 5 | (cb >> 3) << 10);
            *d = semi ? blend(*d, c, p->abr) : c;
            r = (int)((unsigned)r + (unsigned)drdx); g = (int)((unsigned)g + (unsigned)dgdx);
            b = (int)((unsigned)b + (unsigned)dbdx);
        }
    }
    return 1;
}

NUI_CODE static void draw_poly(const NuiPrim *p, int x0, int y0, int x1, int y1) {
    if (rect_ramp(p, x0, y0, x1, y1)) return;
    V v[4];
    for (int i = 0; i < p->n; ++i) v[i] = (V){p->x[i], p->y[i], p->r[i], p->g[i], p->b[i]};
    /* (flat ones: the first vertex's colour) */
    triangle(p, &v[0], &v[1], &v[2], x0, y0, x1, y1);
    if (p->n == 4) triangle(p, &v[2], &v[1], &v[3], x0, y0, x1, y1);
}

/* softgpu.c sg_line()'s arithmetic */
NUI_CODE static void draw_line(const NuiPrim *p, int x0, int y0, int x1, int y1) {
    int dx = p->x[1] - p->x[0], dy = p->y[1] - p->y[0];
    int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    int n = adx > ady ? adx : ady;
    if (n >= 1024) return;
    const int dither = p->flags & F_DITHER, gouraud = p->flags & F_GOURAUD;
    for (int i = 0; i <= n; i++) {
        int x = n ? p->x[0] + dx * i / n : p->x[0], y = n ? p->y[0] + dy * i / n : p->y[0];
        if (x < x0 || x > x1 || y < y0 || y > y1) continue;
        int r = p->r[0], g = p->g[0], b = p->b[0];
        if (gouraud && n) {
            r += (p->r[1] - p->r[0]) * i / n;
            g += (p->g[1] - p->g[0]) * i / n;
            b += (p->b[1] - p->b[0]) * i / n;
        }
        plot(p, x, y, r, g, b, dither);
    }
}

/* p over canvas x0..x1, y0..y1 (inclusive, within the band) */
NUI_CODE static void draw(const NuiPrim *p, int x0, int y0, int x1, int y1) {
    if (x0 < p->bb[0]) x0 = p->bb[0];
    if (y0 < p->bb[1]) y0 = p->bb[1];
    if (x1 > p->bb[2]) x1 = p->bb[2];
    if (y1 > p->bb[3]) y1 = p->bb[3];
    if (x1 < x0 || y1 < y0) return;
    /* to screen coordinates, within the drawing area */
    x0 -= p->dx; x1 -= p->dx; y0 -= p->dy; y1 -= p->dy;
    if (x0 < p->clip[0]) x0 = p->clip[0];
    if (y0 < p->clip[1]) y0 = p->clip[1];
    if (x1 > p->clip[2]) x1 = p->clip[2];
    if (y1 > p->clip[3]) y1 = p->clip[3];
    if (x1 < x0 || y1 < y0) return;
    switch (p->kind) {
    case K_TILE: draw_tile(p, x0, y0, x1, y1); break;
    case K_SPRT: draw_sprt(p, x0, y0, x1, y1); break;
    case K_POLY: draw_poly(p, x0, y0, x1, y1); break;
    default: draw_line(p, x0, y0, x1, y1); break;
    }
}

/* ---- the list --------------------------------------------------------------------------------------------- */

static struct {
    u32 texpage;
    int mx, my, ox, oy;                 /* texture window, in 8-texel units */
    int ax0, ay0, ax1, ay1;             /* drawing area (VRAM) */
    int ofs_x, ofs_y;
    int slot;                           /* the list's OT entry being walked (g_ot[slot]) */
} st;

/* which canvas a primitive covering screen x0..x1, y0..y1 goes to (0, 1; -1: none), and where */
NUI_CODE static int layout(int x0, int y0, int x1, int y1, int *dx, int *dy) {
    *dx = *dy = 0;
    if (cv.identity) return 0;
    const int cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
    if (st.slot == 17) {
        /* the target cursors (btlui_drawTargetCursors' OT entry), pointing into the scene: on the top screen, where
         * the scene (scaled to 4/5) has that point, at 1:1 */
        if (!cv.top_pixels) return -1;
        *dx = cx * 4 / 5 - cx;
        *dy = cy * 4 / 5 + top_y - cy;
        return 1;
    }
    if (cy < 40) { *dx = -32; return 0; }
    /* the command and list windows below the party panel */
    if (cx < 142) { *dy = -26; return 0; }
    *dx = -57; *dy = -104; /* the party panel or the help window (to 312: the canvas's right edge) */
    return 0;
}


static inline int sext11(u32 v) { return (int)((v & 0x7FF) ^ 0x400) - 0x400; }

NUI_CODE static NuiPrim *add(int kind, int flags) {
    if (counts[1] == max_prims) { nui_stats_unknown++; return NULL; }
    NuiPrim *p = &lists[1][counts[1]];
    memset(p, 0, sizeof *p);
    p->kind = (u8)kind;
    p->flags = (u8)flags;
    p->abr = (u8)((st.texpage >> 5) & 3);
    p->phase = (u8)((st.ofs_x & 3) | (st.ofs_y & 3) << 2);
    p->clip[0] = (s16)(st.ax0 - st.ofs_x); p->clip[1] = (s16)(st.ay0 - st.ofs_y);
    p->clip[2] = (s16)(st.ax1 - st.ofs_x); p->clip[3] = (s16)(st.ay1 - st.ofs_y);
    return p;
}

/* p's extent on the canvas, and its layout; 0: drop it */
NUI_CODE static int place(NuiPrim *p, int x0, int y0, int x1, int y1) {
    if (x0 < p->clip[0]) x0 = p->clip[0];
    if (y0 < p->clip[1]) y0 = p->clip[1];
    if (x1 > p->clip[2]) x1 = p->clip[2];
    if (y1 > p->clip[3]) y1 = p->clip[3];
    if (x1 < x0 || y1 < y0) return 0;
    int dx, dy, canvas = layout(x0, y0, x1, y1, &dx, &dy);
    if (canvas < 0) return 0;
    p->canvas = (u8)canvas;
    p->dx = (s16)dx; p->dy = (s16)dy;
    x0 += dx; x1 += dx; y0 += dy; y1 += dy;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > cv.w - 1) x1 = cv.w - 1;
    if (y1 > cv.h - 1) y1 = cv.h - 1;
    if (x1 < x0 || y1 < y0) return 0;
    p->bb[0] = (s16)x0; p->bb[1] = (s16)y0; p->bb[2] = (s16)x1; p->bb[3] = (s16)y1;
    counts[1]++;
    return 1;
}

NUI_CODE static void command(const u32 *w, unsigned int len) {
    const u32 cmd = w[0] >> 24;
    switch (cmd) {
    case 0xE1: st.texpage = w[0] & 0xFFFFFF; return;
    case 0xE2:
        st.mx = w[0] & 31; st.my = (w[0] >> 5) & 31; st.ox = (w[0] >> 10) & 31; st.oy = (w[0] >> 15) & 31;
        return;
    case 0xE3: st.ax0 = w[0] & 0x3FF; st.ay0 = (w[0] >> 10) & 0x1FF; return;
    case 0xE4: st.ax1 = w[0] & 0x3FF; st.ay1 = (w[0] >> 10) & 0x1FF; return;
    case 0xE5: st.ofs_x = sext11(w[0]); st.ofs_y = sext11(w[0] >> 11); return;
    case 0xE6: case 0x00: case 0x01: return;
    }
    if (cmd >= 0x80 && cmd < 0xA0) return; /* VRAM copies: the row cache's */
    if (cmd >= 0x60 && cmd < 0x80) {
        const int textured = cmd & 4, size = (cmd >> 3) & 3;
        const unsigned int k = textured ? 3 : 2;
        if (len < k + (size == 0)) { nui_stats_unknown++; return; }
        int x = sext11(w[1]), y = sext11(w[1] >> 16), wd, ht;
        if (size == 0) { wd = w[k] & 0x3FF; ht = (w[k] >> 16) & 0x1FF; }
        else wd = ht = size == 1 ? 1 : size == 2 ? 8 : 16;
        NuiPrim *p = add(textured ? K_SPRT : K_TILE, (cmd & 2 ? F_SEMI : 0) | (textured && (cmd & 1) ? F_RAW : 0));
        if (!p) return;
        p->x[0] = (s16)x; p->y[0] = (s16)y; p->x[1] = (s16)wd; p->y[1] = (s16)ht;
        p->r[0] = (u8)w[0]; p->g[0] = (u8)(w[0] >> 8); p->b[0] = (u8)(w[0] >> 16);
        if (textured) {
            const u32 tp = st.texpage;
            p->u = (u8)w[2]; p->v = (u8)(w[2] >> 8);
            p->cx = (u16)(((w[2] >> 16) & 0x3F) * 16); p->cy = (u16)((w[2] >> 22) & 0x1FF);
            p->tx = (u16)((tp & 15) * 64); p->ty = (u16)(((tp >> 4) & 1) * 256);
            p->depth = (u8)((tp >> 7) & 3) > 2 ? 2 : (u8)((tp >> 7) & 3);
            p->twin_mx = (u8)st.mx; p->twin_my = (u8)st.my; p->twin_ox = (u8)st.ox; p->twin_oy = (u8)st.oy;
            /* the row cache's sprites: the scene under the panel's digits and icons, which the canvas doesn't have */
            if (p->depth == 2 && p->tx + p->u >= 960 && p->ty + p->v >= 432) { memset(p, 0, sizeof *p); return; }
        }
        if (!place(p, x, y, x + wd - 1, y + ht - 1)) memset(p, 0, sizeof *p);
        return;
    }
    if (cmd >= 0x20 && cmd < 0x40 && !(cmd & 4)) { /* untextured polygons */
        const int n = cmd & 8 ? 4 : 3, gouraud = cmd & 0x10;
        NuiPrim *p = add(K_POLY, (cmd & 2 ? F_SEMI : 0) | (gouraud ? F_GOURAUD : 0) |
                                     (gouraud && (st.texpage & 0x200) ? F_DITHER : 0));
        if (!p) return;
        p->n = (u8)n;
        if (len < (unsigned int)(1 + n + (gouraud ? n - 1 : 0))) { memset(p, 0, sizeof *p); nui_stats_unknown++; return; }
        unsigned int k = 1;
        int x0 = 4096, y0 = 4096, x1 = -4096, y1 = -4096;
        for (int i = 0; i < n; ++i) {
            u32 c = gouraud && i ? w[k++] : w[0];
            p->r[i] = (u8)c; p->g[i] = (u8)(c >> 8); p->b[i] = (u8)(c >> 16);
            p->x[i] = (s16)sext11(w[k]); p->y[i] = (s16)sext11(w[k] >> 16);
            ++k;
            if (p->x[i] < x0) x0 = p->x[i];
            if (p->x[i] > x1) x1 = p->x[i];
            if (p->y[i] < y0) y0 = p->y[i];
            if (p->y[i] > y1) y1 = p->y[i];
        }
        if (!place(p, x0, y0, x1, y1)) memset(p, 0, sizeof *p);
        return;
    }
    if (cmd >= 0x40 && cmd < 0x60 && !(cmd & 8)) { /* lines */
        const int gouraud = cmd & 0x10;
        NuiPrim *p = add(K_LINE, (cmd & 2 ? F_SEMI : 0) | (gouraud ? F_GOURAUD : 0) | (st.texpage & 0x200 ? F_DITHER : 0));
        if (!p) return;
        p->n = 2;
        u32 c1 = gouraud ? w[2] : w[0];
        p->r[0] = (u8)w[0]; p->g[0] = (u8)(w[0] >> 8); p->b[0] = (u8)(w[0] >> 16);
        p->r[1] = (u8)c1; p->g[1] = (u8)(c1 >> 8); p->b[1] = (u8)(c1 >> 16);
        p->x[0] = (s16)sext11(w[1]); p->y[0] = (s16)sext11(w[1] >> 16);
        p->x[1] = (s16)sext11(w[gouraud ? 3 : 2]); p->y[1] = (s16)sext11(w[gouraud ? 3 : 2] >> 16);
        int x0 = p->x[0] < p->x[1] ? p->x[0] : p->x[1], x1 = p->x[0] ^ p->x[1] ^ x0;
        int y0 = p->y[0] < p->y[1] ? p->y[0] : p->y[1], y1 = p->y[0] ^ p->y[1] ^ y0;
        if (!place(p, x0, y0, x1, y1)) memset(p, 0, sizeof *p);
        return;
    }
    nui_stats_unknown++;
}

unsigned int psx_gp0_len(const u32 *w, unsigned int avail);

NUI_CODE static void walk(const u32 *p) {
    const u32 window = (u32)(uintptr_t)psx_ram & 0xFF000000;
    const u32 *const ot = p - 0x17; /* (the list is g_ot[0x17] down to g_ot[0]: btlui_submitOt) */
    unsigned int guard = 0;
    while (((u32)(uintptr_t)p & 0xFFFFFF) != 0xFFFFFF) {
        if (p >= ot && p <= ot + 0x17) st.slot = (int)(p - ot);
        const unsigned int len = p[0] >> 24;
        for (unsigned int i = 0; i < len;) {
            const unsigned int n = psx_gp0_len(p + 1 + i, len - i);
            command(p + 1 + i, n);
            i += n ? n : 1;
        }
        const u32 addr = p[0] & 0xFFFFFF;
        if (addr == 0xFFFFFF) break;
        p = (const u32 *)(uintptr_t)(window | addr);
        if ((const u8 *)p < psx_ram || (const u8 *)p >= psx_ram + PSX_RAM_SIZE || ++guard > 100000) break;
    }
}

/* ---- redrawing -------------------------------------------------------------------------------------------- */

static int ndirty[2];

NUI_CODE static void dirty_add(int canvas, const s16 *r) {
    if (r[2] < r[0] || r[3] < r[1] || redraw_all[canvas]) return;
    for (int i = 0; i < ndirty[canvas]; ++i) {
        s16 *d = T->dirty[canvas][i];
        if (r[0] <= d[2] + 1 && d[0] <= r[2] + 1 && r[1] <= d[3] + 1 && d[1] <= r[3] + 1) {
            if (r[0] < d[0]) d[0] = r[0];
            if (r[1] < d[1]) d[1] = r[1];
            if (r[2] > d[2]) d[2] = r[2];
            if (r[3] > d[3]) d[3] = r[3];
            return;
        }
    }
    if (ndirty[canvas] == RECTS) { redraw_all[canvas] = 1; return; }
    memcpy(T->dirty[canvas][ndirty[canvas]++], r, sizeof T->dirty[0][0]);
}

NUI_HOT static void redraw(int canvas, const s16 *r) {
    const NuiPrim *cur = lists[1];
    u8 *const pixels = canvas ? cv.top_pixels : cv.pixels;
    const int pitch = canvas ? cv.top_pitch : cv.pitch;
    const int n = counts[1];
    int x0 = r[0] & ~1, x1 = r[2] | 1;
    if (x1 > cv.w - 1) x1 = cv.w - 1;
    for (int y0 = r[1]; y0 <= r[3]; y0 += cv.scratch_rows) {
        int y1 = y0 + cv.scratch_rows - 1;
        if (y1 > r[3]) y1 = r[3];
        band = cv.scratch; band_x = x0; band_y = y0;
        for (int y = 0; y <= y1 - y0; ++y) memset(band + y * cv.w, 0, (size_t)(x1 - x0 + 1) * 2);
        for (int i = 0; i < n; ++i) {
            const NuiPrim *p = &cur[i];
            if (p->canvas != canvas || p->bb[0] > x1 || p->bb[2] < x0 || p->bb[1] > y1 || p->bb[3] < y0) continue;
            draw(p, x0, y0, x1, y1);
            nui_stats_drawn++;
        }
        /* (halfword stores: the DS's VRAM ignores byte writes) */
        for (int y = 0; y <= y1 - y0; ++y) {
            const u16 *s = band + y * cv.w;
            u16 *o = (u16 *)(pixels + (y0 + y) * pitch + x0);
            if (canvas)
                for (int x = 0; x < x1 - x0; x += 2)
                    *o++ = (u16)(top_index_of(s[x] & 0x7FFF) | top_index_of(s[x + 1] & 0x7FFF) << 8);
            else
                for (int x = 0; x < x1 - x0; x += 2)
                    *o++ = (u16)(index_of(s[x] & 0x7FFF) | index_of(s[x + 1] & 0x7FFF) << 8);
        }
        nui_stats_pixels += (unsigned int)((x1 - x0 + 1) * (y1 - y0 + 1));
    }
}

NUI_CODE static void finish(void) {
    NuiPrim *prev = lists[0], *cur = lists[1];
    const int n = counts[1], m = counts[0];
    ++list_no;
    palette_full = 0; /* (a full palette compacts again at most once a list) */
    const int textures = src_writes != src_seen;
    src_seen = src_writes;
    if (top_full) { top_full = 0; top_colours = 0; top_memo_c = 0xFFFF; redraw_all[1] = 1; }
    ndirty[0] = ndirty[1] = 0;
    for (int i = 0; i < n || i < m; ++i) {
        if (i < n && i < m && !memcmp(&cur[i], &prev[i], sizeof *cur) && !(textures && cur[i].kind == K_SPRT))
            continue;
        if (i < n) dirty_add(cur[i].canvas, cur[i].bb);
        if (i < m) dirty_add(prev[i].canvas, prev[i].bb);
    }
    for (int k = 0; k < 2; ++k) {
        if (k && !cv.top_pixels) break;
        if (redraw_all[k]) {
            const s16 all[4] = {0, 0, (s16)(cv.w - 1), (s16)(cv.h - 1)};
            redraw(k, all);
            redraw_all[k] = 0;
        } else
            for (int i = 0; i < ndirty[k]; ++i) redraw(k, T->dirty[k][i]);
    }
    lists[0] = cur; lists[1] = prev;
    counts[0] = n; counts[1] = 0;
}

/* ---- interface -------------------------------------------------------------------------------------------- */

NUI_CODE void nui_init(const NuiCanvas *canvas) {
    cv = *canvas;
    T = (Tables *)cv.tables;
    max_prims = (int)(cv.store_size / 2 / sizeof(NuiPrim));
    lists[0] = (NuiPrim *)cv.store;
    lists[1] = lists[0] + max_prims;
    counts[0] = counts[1] = 0;
    palette_reset();
    top_colours = 0; top_memo_c = 0xFFFF;
    redraw_all[0] = redraw_all[1] = 1;
}

NUI_CODE int nui_list(const uint32_t *head) {
    if (!T) return 0;
    memset(&st, 0, sizeof st);
    st.ax1 = 1023; st.ay1 = 511;
    counts[1] = 0;
    walk(head);
    if ((unsigned int)counts[1] > nui_stats_max) nui_stats_max = (unsigned int)counts[1];
    finish();
    lists_since_frame++;
    return 1;
}

NUI_CODE void nui_frame(void) {
    if (!T) return;
    if (!lists_since_frame) { counts[1] = 0; finish(); } /* no UI this frame */
    lists_since_frame = 0;
}

void nui_vram_written(int x, int y, int w, int h) {
    /* the UI's pages and CLUTs (x >= 512, y >= 224), but for the row cache the list's own copies write */
    if (x + w <= 512 || y + h <= 224) return;
    if (x >= 960 && y >= 432) return;
    src_writes++;
}

void nui_invalidate(void) {
    redraw_all[0] = redraw_all[1] = 1;
}

void nui_set_top(int y) {
    top_y = y;
}

#ifdef NUI_DEBUG
#include <stdio.h>
void nui_debug_pixel(int x, int y) {
    const NuiPrim *cur = lists[0];
    for (int i = 0; i < counts[0]; ++i) {
        const NuiPrim *p = &cur[i];
        if (x < p->bb[0] || x > p->bb[2] || y < p->bb[1] || y > p->bb[3]) continue;
        fprintf(stderr, "  #%d kind %d flags %x abr %d n %d depth %d phase %x clip %d,%d-%d,%d xy", i, p->kind, p->flags,
                p->abr, p->n, p->depth, p->phase, p->clip[0], p->clip[1], p->clip[2], p->clip[3]);
        for (int k = 0; k < 4; ++k) fprintf(stderr, " (%d,%d %02x%02x%02x)", p->x[k], p->y[k], p->r[k], p->g[k], p->b[k]);
        fprintf(stderr, "\n");
    }
}
#endif
