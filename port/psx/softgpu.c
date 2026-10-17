/* The software PS1 GPU backend (gp0.h): rasterises decoded primitives into the VRAM shadow.
 *
 * It is the reference renderer: on the PC build it shows what the PS1 would display (frame dumps), and the DS 3D
 * renderer is checked against it. It models the drawing area, texture pages (4/8/15 bit, CLUTs), texture window,
 * the four semi-transparency modes, raw vs modulated textures, gouraud shading, dithering and mask bits. Exact
 * sub-pixel edge rules are not modelled. Edge functions and interpolants step per pixel (no division in the loop).
 */
#include "gp0.h"
#include "profile.h"
#include "psyq_port.h"
#include "psx_raster.h"

#ifdef PORT_RASTER_ITCM
#define SG_HOT __attribute__((section(".itcm"), long_call))
#else
#define SG_HOT
#endif

#define VRAM_W 1024
#define VRAM_H 512

extern u16 *psx_vram;
static struct {
    u16 *pixels;
    u8 *known;
    int x, y, w, h;
    int clip_active, clip_x, clip_y, clip_w, clip_h;
} target;

/* psx_softgpu_set_marks: while set, every pixel a primitive writes gets `bit` in this 1024x512 plane (VRAM
 * coordinates), and drawing takes the generic per-pixel path. The hybrid renderer marks its software-drawn
 * additive glows this way (gx_hybrid.c). */
static u8 *marks;
static u8 marks_bit;
void psx_softgpu_set_marks(u8 *plane, unsigned bit) { marks = plane; marks_bit = (u8)bit; }

static inline u16 *px(int x, int y) {
    return &psx_vram[(y & (VRAM_H - 1)) * VRAM_W + (x & (VRAM_W - 1))];
}

static inline u16 blend(u16 back, u16 front, int abr) {
    int br = back & 31, bg = (back >> 5) & 31, bb = (back >> 10) & 31;
    int fr = front & 31, fg = (front >> 5) & 31, fb = (front >> 10) & 31;
    int r, g, b;
    switch (abr) {
    case 0: r = (br + fr) >> 1; g = (bg + fg) >> 1; b = (bb + fb) >> 1; break;
    case 1: r = br + fr; g = bg + fg; b = bb + fb; break;
    case 2: r = br - fr; g = bg - fg; b = bb - fb; break;
    default: r = br + (fr >> 2); g = bg + (fg >> 2); b = bb + (fb >> 2); break;
    }
    r = r < 0 ? 0 : r > 31 ? 31 : r;
    g = g < 0 ? 0 : g > 31 ? 31 : g;
    b = b < 0 ? 0 : b > 31 ? 31 : b;
    return (u16)(r | g << 5 | b << 10);
}

static u32 shaded_palette[256];
static int palette_ready;

static void prepare_palette(const Gp0Mode *m, const Gp0Vtx *v,
                            int x0, int y0, int x1, int y1) {
    palette_ready = 0;
#ifndef PORT_SOFTGPU_REFERENCE
    if (!m->textured || m->gouraud || m->depth > 1) return;
    int count = m->depth == 0 ? 16 : 256;
    if ((x1-x0)*(y1-y0) < count*4) return;
    /* Palette reads must remain live when drawing can modify the CLUT.
     * Shadow polygons write a separate destination and cannot do so. */
    if (!target.pixels) {
        if (x0 < 0 || y0 < 0 || x1 > VRAM_W || y1 > VRAM_H) return;
        int cy = m->clut_y & (VRAM_H-1);
        if (cy >= y0 && cy < y1) {
            for (int i=0; i<count; ++i) {
                int cx = (m->clut_x+i) & (VRAM_W-1);
                if (cx >= x0 && cx < x1) return;
            }
        }
    }
    int r = v->r < 0 ? 0 : v->r > 255 ? 255 : v->r;
    int g = v->g < 0 ? 0 : v->g > 255 ? 255 : v->g;
    int b = v->b < 0 ? 0 : v->b > 255 ? 255 : v->b;
    for (int i=0; i<count; ++i) {
        u16 t = *px(m->clut_x+i, m->clut_y);
        int tr = (t & 31)*r >> 7, tg = ((t >> 5)&31)*g >> 7, tb = ((t >> 10)&31)*b >> 7;
        u16 c = (tr > 31 ? 31 : tr) | ((tg > 31 ? 31 : tg) << 5)
              | ((tb > 31 ? 31 : tb) << 10) | (t & 0x8000);
        if (m->raw) c = t;
        /* Keep the original texel for transparency and STP, even when colour
         * modulation turns an opaque texel into black. */
        shaded_palette[i] = t | ((u32)c << 16);
    }
    palette_ready = 1;
#endif
}

#ifndef PORT_SOFTGPU_REFERENCE
__attribute__((always_inline))
#endif
static inline u32 texel(const Gp0Mode *m, int u, int v) {
    const Gp0State *s = &gp0_state;
    u = (int)(((unsigned)u & ~s->twin_mask_x) | (s->twin_off_x & s->twin_mask_x)) & 0xFF;
    v = (int)(((unsigned)v & ~s->twin_mask_y) | (s->twin_off_y & s->twin_mask_y)) & 0xFF;
    switch (m->depth) {
    case 0: {
        u16 w = *px(m->tx + (u >> 2), m->ty + v);
        int i = (w >> ((u & 3)*4)) & 15;
        return palette_ready ? shaded_palette[i] : *px(m->clut_x+i, m->clut_y);
    }
    case 1: {
        u16 w = *px(m->tx + (u >> 1), m->ty + v);
        int i = (w >> ((u & 1)*8)) & 255;
        return palette_ready ? shaded_palette[i] : *px(m->clut_x+i, m->clut_y);
    }
    default:
        return *px(m->tx + u, m->ty + v);
    }
}

#ifndef PORT_SOFTGPU_REFERENCE
__attribute__((always_inline))
#endif
static inline u16 dither_colour(int r,int g,int b,int x,int y) {
    static const signed char offsets[16]={-4,0,-3,1,2,-2,3,-1,-3,1,-4,0,3,-1,2,-2};
    int bias=offsets[((y&3)<<2)|(x&3)];
    r+=bias;g+=bias;b+=bias;
    r=r<0?0:r>255?255:r;
    g=g<0?0:g>255?255:g;
    b=b<0?0:b>255?255:b;
    return (u16)((r>>3)|((g>>3)<<5)|((b>>3)<<10));
}

static inline void plot(const Gp0Mode *m, int x, int y, int r, int g, int b, int u, int v, int dither) {
    /* Fixed-point interpolation can round an edge channel one step below
     * zero. Keep colours in their architectural eight-bit range before packing. */
    r = r < 0 ? 0 : r > 255 ? 255 : r;
    g = g < 0 ? 0 : g > 255 ? 255 : g;
    b = b < 0 ? 0 : b > 255 ? 255 : b;
    int index = 0;
    u16 *d;
    if (target.pixels) {
        if (x < target.x || y < target.y || x >= target.x + target.w || y >= target.y + target.h) return;
        index = (y - target.y)*target.w + x - target.x;
        d = target.pixels + index;
        if (gp0_state.check_mask && !target.known[index]) return;
    } else d = px(x, y);
    if (gp0_state.check_mask && (*d & 0x8000)) {
        return;
    }
    u16 c;
    int semi = m->semi;
    if (m->textured) {
        u32 sample = texel(m, u, v);
        u16 t = sample;
        if (t == 0) {
            return;
        }
        semi = semi && (t & 0x8000);
        if (m->raw) {
            c = t;
        } else if (palette_ready) {
            c = sample >> 16;
        } else if (dither) {
            /* Retain the modulation's low three bits until dithering. Doing
             * the old five-bit multiply first loses the threshold changes. */
            c=dither_colour((t&31)*r>>4,((t>>5)&31)*g>>4,((t>>10)&31)*b>>4,x,y)|(t&0x8000);
        } else {
            int tr = (t & 31) * r >> 7, tg = ((t >> 5) & 31) * g >> 7, tb = ((t >> 10) & 31) * b >> 7;
            c = (u16)((tr > 31 ? 31 : tr) | (tg > 31 ? 31 : tg) << 5 | (tb > 31 ? 31 : tb) << 10 | (t & 0x8000));
        }
    } else {
        c = dither ? dither_colour(r,g,b,x,y) : (u16)((r >> 3) | (g >> 3) << 5 | (b >> 3) << 10);
    }
    if (semi) {
        c = (u16)(blend(*d, c, m->abr) | (c & 0x8000));
    }
    *d = c | (gp0_state.set_mask ? 0x8000 : 0);
    if (target.pixels && !semi) target.known[index] = 1;
    if (marks && !target.pixels) marks[(y & (VRAM_H - 1)) * VRAM_W + (x & (VRAM_W - 1))] |= marks_bit;
}

/* Interpolants outside a thin triangle can exceed signed 16.16 range while
 * traversing its bounding box. Match 32-bit register addition without C signed
 * overflow; only covered pixels are consumed. */
static inline int step_attribute(int value, int delta) {
    return (int)((unsigned int)value + (unsigned int)delta);
}

#ifndef PORT_SOFTGPU_REFERENCE
/* Intersect a row with one biased half-plane. Only a crossing edge needs
 * division; fully accepted/rejected rows use their endpoint signs. */
static inline int clip_span(int edge,int step,int width,int *left,int *right) {
    int end=edge+step*(width-1);
    if(edge>=0 && end>=0) return 1;
    if(edge<0 && end<0) return 0;
    if(step>0) {
        int first=(-edge+step-1)/step;
        if(first>*left) *left=first;
    } else {
        int last=edge/(-step)+1;
        if(last<*right) *right=last;
    }
    return *left<*right;
}
#define RASTER_COVERED 1
#define RASTER_STEP_EDGES() ((void)0)
#else
#define RASTER_COVERED ((w0+bias0)>=0 && (w1+bias1)>=0 && (w2+bias2)>=0)
#define RASTER_STEP_EDGES() do {w0+=A0;w1+=A1;w2+=A2;} while(0)
#endif

/* ---- triangle: edge functions and 12-fractional-bit GPU interpolants -------------------------------- */

#ifndef PORT_SOFTGPU_REFERENCE
/* A textured polygon row on the common path: opaque, no mask test, no dithering, drawn into VRAM, with the
 * colour per texel either from the shaded palette, the texel itself (raw or neutral 128 modulation), or a flat
 * modulation (`mod`, r/g/b fixed for the polygon). Texel and CLUT reads happen per pixel, as plot() does, so a
 * polygon that draws over its own texture or CLUT sees the same values. */
static SG_HOT __attribute__((noinline)) void poly_row_textured(const Gp0Mode *m, u16 *dst, int width, int u, int v,
        int dudx, int dvdx, u16 mask, int mod, int r, int g, int b, int dither, int x, int y) {
    const Gp0State *st = &gp0_state;
    const unsigned keep_x = ~st->twin_mask_x, wx = st->twin_off_x & st->twin_mask_x;
    const unsigned keep_y = ~st->twin_mask_y, wy = st->twin_off_y & st->twin_mask_y;
    const u16 *clut = psx_vram + (m->clut_y & 511) * 1024;
    const unsigned clut_x = m->clut_x, tx = m->tx, ty = m->ty, depth = m->depth;
    for (; width > 0; --width, ++dst, ++x, u = step_attribute(u, dudx), v = step_attribute(v, dvdx)) {
        unsigned uu = (((unsigned)(u >> 16) & keep_x) | wx) & 0xFF;
        unsigned vv = (((unsigned)(v >> 16) & keep_y) | wy) & 0xFF;
        const u16 *row = psx_vram + ((ty + vv) & 511) * 1024;
        u16 t, c;
        if (depth == 0) {
            unsigned i = (row[(tx + (uu >> 2)) & 1023] >> ((uu & 3) * 4)) & 15;
            if (palette_ready) { u32 p = shaded_palette[i]; t = (u16)p; c = p >> 16; }
            else t = c = clut[(clut_x + i) & 1023];
        } else if (depth == 1) {
            unsigned i = (row[(tx + (uu >> 1)) & 1023] >> ((uu & 1) * 8)) & 255;
            if (palette_ready) { u32 p = shaded_palette[i]; t = (u16)p; c = p >> 16; }
            else t = c = clut[(clut_x + i) & 1023];
        } else {
            t = c = row[(tx + uu) & 1023];
        }
        if (!t) continue;
        if (dither) {
            /* plot()'s dithered modulation: eight-bit products, the 4x4 offset, then five bits (palette is off) */
            c = dither_colour((t & 31) * r >> 4, ((t >> 5) & 31) * g >> 4, ((t >> 10) & 31) * b >> 4, x, y) |
                (t & 0x8000);
        } else if (mod && !palette_ready) {
            int tr = (t & 31) * r >> 7, tg = ((t >> 5) & 31) * g >> 7, tb = ((t >> 10) & 31) * b >> 7;
            c = (u16)((tr > 31 ? 31 : tr) | (tg > 31 ? 31 : tg) << 5 | (tb > 31 ? 31 : tb) << 10 | (t & 0x8000));
        }
        *dst = c | mask;
    }
}
#endif

#ifndef PORT_SOFTGPU_REFERENCE
/* The common battle polygon row: indexed texels, no texture window, unwrapped CLUT, dithered flat modulation.
 * dither_colour's arithmetic as a table per 4x4 dither position and channel, rebuilt when the polygon colour
 * changes (rarely: a few hundred times per thousand battle frames): dtab[y & 3][x & 3][ch][t5] is
 * clamp((t5 * colour >> 4) + offset, 0, 255) >> 3, shifted into the channel's place. */
static u16 dtab[4][4][3][32];
static unsigned dtab_key = ~0u;
static void dither_tables(int r, int g, int b) {
    static const signed char offsets[16] = {-4,0,-3,1,2,-2,3,-1,-3,1,-4,0,3,-1,2,-2};
    unsigned key = (unsigned)r | (unsigned)g << 8 | (unsigned)b << 16;
    if (key == dtab_key) return;
    dtab_key = key;
    const int colour[3] = {r, g, b};
    for (int p = 0; p < 16; ++p)
        for (int ch = 0; ch < 3; ++ch)
            for (int t = 0; t < 32; ++t) {
                int v = (t * colour[ch] >> 4) + offsets[p];
                dtab[p >> 2][p & 3][ch][t] = (u16)(((v < 0 ? 0 : v > 255 ? 255 : v) >> 3) << (5 * ch));
            }
}

static SG_HOT __attribute__((noinline)) void poly_row_dither_indexed(const Gp0Mode *m, u16 *dst, int width, int u,
        int v, int dudx, int dvdx, u16 mask, int x, int y) {
    const u16 (*phase)[3][32] = dtab[y & 3];
    const u16 *clut = psx_vram + (m->clut_y & 511) * 1024 + m->clut_x;
    const unsigned tx = m->tx, ty = m->ty;
    if (m->depth) {
        for (; width > 0; --width, ++dst, ++x, u = step_attribute(u, dudx), v = step_attribute(v, dvdx)) {
            unsigned uu = (unsigned)(u >> 16) & 0xFF, vv = (unsigned)(v >> 16) & 0xFF;
            u16 t = clut[(psx_vram[((ty + vv) & 511) * 1024 + ((tx + (uu >> 1)) & 1023)] >> ((uu & 1) * 8)) & 255];
            if (!t) continue;
            const u16 (*d)[32] = phase[x & 3];
            *dst = d[0][t & 31] | d[1][(t >> 5) & 31] | d[2][(t >> 10) & 31] | (t & 0x8000) | mask;
        }
    } else {
        for (; width > 0; --width, ++dst, ++x, u = step_attribute(u, dudx), v = step_attribute(v, dvdx)) {
            unsigned uu = (unsigned)(u >> 16) & 0xFF, vv = (unsigned)(v >> 16) & 0xFF;
            u16 t = clut[(psx_vram[((ty + vv) & 511) * 1024 + ((tx + (uu >> 2)) & 1023)] >> ((uu & 3) * 4)) & 15];
            if (!t) continue;
            const u16 (*d)[32] = phase[x & 3];
            *dst = d[0][t & 31] | d[1][(t >> 5) & 31] | d[2][(t >> 10) & 31] | (t & 0x8000) | mask;
        }
    }
}
#endif

#ifndef PORT_SOFTGPU_REFERENCE
/* plot()'s untextured pixel for a row: colour (interpolated, or the flat colour with zero steps), optional
 * dither, then blend mode `abr` (-1: opaque). No mask test and no shadow target. A row whose colour does not
 * change (flat, or Gouraud with zero x steps: fades) uses its four dithered colours. */
static inline __attribute__((always_inline)) void constant_row(u16 *dst, int width, const u16 *colours, u16 mask,
                                                               int x, const int abr) {
    for (; width > 0; --width, ++dst, ++x) {
        u16 c = colours[x & 3];
        *dst = (abr < 0 ? c : blend(*dst, c, abr)) | mask;
    }
}

static SG_HOT __attribute__((noinline)) void poly_row_untextured(const Gp0Mode *m, u16 *dst, int width, int r, int g,
        int b, int drdx, int dgdx, int dbdx, int dither, u16 mask, int x, int y) {
    const int abr = m->semi ? m->abr : -1;
    if (!drdx && !dgdx && !dbdx) {
        int cr = r >> 16, cg = g >> 16, cb = b >> 16;
        cr = cr < 0 ? 0 : cr > 255 ? 255 : cr;
        cg = cg < 0 ? 0 : cg > 255 ? 255 : cg;
        cb = cb < 0 ? 0 : cb > 255 ? 255 : cb;
        u16 colours[4];
        for (int i = 0; i < 4; ++i)
            colours[(x + i) & 3] = dither ? dither_colour(cr, cg, cb, x + i, y)
                                          : (u16)((cr >> 3) | (cg >> 3) << 5 | (cb >> 3) << 10);
        switch (abr) {
        case -1: constant_row(dst, width, colours, mask, x, -1); break;
        case 0: constant_row(dst, width, colours, mask, x, 0); break;
        case 1: constant_row(dst, width, colours, mask, x, 1); break;
        case 2: constant_row(dst, width, colours, mask, x, 2); break;
        default: constant_row(dst, width, colours, mask, x, 3); break;
        }
        return;
    }
    for (; width > 0; --width, ++dst, ++x, r = step_attribute(r, drdx), g = step_attribute(g, dgdx),
                                           b = step_attribute(b, dbdx)) {
        int cr = r >> 16, cg = g >> 16, cb = b >> 16;
        cr = cr < 0 ? 0 : cr > 255 ? 255 : cr;
        cg = cg < 0 ? 0 : cg > 255 ? 255 : cg;
        cb = cb < 0 ? 0 : cb > 255 ? 255 : cb;
        u16 c = dither ? dither_colour(cr, cg, cb, x, y) : (u16)((cr >> 3) | (cg >> 3) << 5 | (cb >> 3) << 10);
        *dst = (abr < 0 ? c : blend(*dst, c, abr)) | mask;
    }
}

/* plot()'s dithered flat textured pixel with semi-transparency (blend mode abr on texels with bit 15), any
 * texel depth, no texture window, unwrapped CLUT: dtab (dither_tables) gives the modulated colour. */
static SG_HOT __attribute__((noinline)) void poly_row_dither_semi(const Gp0Mode *m, u16 *dst, int width, int u, int v,
        int dudx, int dvdx, u16 mask, int x, int y) {
    const u16 (*phase)[3][32] = dtab[y & 3];
    const u16 *clut = psx_vram + (m->clut_y & 511) * 1024 + m->clut_x;
    const unsigned tx = m->tx, ty = m->ty, depth = m->depth;
    const int abr = m->abr;
    for (; width > 0; --width, ++dst, ++x, u = step_attribute(u, dudx), v = step_attribute(v, dvdx)) {
        unsigned uu = (unsigned)(u >> 16) & 0xFF, vv = (unsigned)(v >> 16) & 0xFF;
        const u16 *row = psx_vram + ((ty + vv) & 511) * 1024;
        u16 t;
        if (depth == 2) t = row[(tx + uu) & 1023];
        else if (depth == 1) t = clut[(row[(tx + (uu >> 1)) & 1023] >> ((uu & 1) * 8)) & 255];
        else t = clut[(row[(tx + (uu >> 2)) & 1023] >> ((uu & 3) * 4)) & 15];
        if (!t) continue;
        const u16 (*d)[32] = phase[x & 3];
        u16 c = d[0][t & 31] | d[1][(t >> 5) & 31] | d[2][(t >> 10) & 31];
        if (t & 0x8000) c = blend(*dst, c, abr) | 0x8000;
        *dst = c | mask;
    }
}
#endif

static SG_HOT void triangle(const Gp0Mode *m, const Gp0Vtx *a, const Gp0Vtx *b, const Gp0Vtx *c) {
    /* The GPU seeds attributes at the leftmost input vertex. Preserve input
     * order for tied X before changing winding for edge coverage below. */
    const Gp0Vtx *anchor = b->x <= a->x ? b : a;
    if (c->x < anchor->x || (anchor == b && c->x == anchor->x)) anchor = c;
    int area = (b->x - a->x) * (c->y - a->y) - (b->y - a->y) * (c->x - a->x);
    if (area == 0) {
        return;
    }
    if (area < 0) {
        const Gp0Vtx *t = b;
        b = c;
        c = t;
        area = -area;
    }
    int minx = a->x, maxx = a->x, miny = a->y, maxy = a->y;
    const Gp0Vtx *vs[3] = {a, b, c};
    for (int i = 1; i < 3; i++) {
        if (vs[i]->x < minx) minx = vs[i]->x;
        if (vs[i]->x > maxx) maxx = vs[i]->x;
        if (vs[i]->y < miny) miny = vs[i]->y;
        if (vs[i]->y > maxy) maxy = vs[i]->y;
    }
    if (maxx - minx >= 1024 || maxy - miny >= 512) {
        return; /* the GPU skips polygons this large */
    }
    if (minx < gp0_state.area_x0) minx = gp0_state.area_x0;
    if (miny < gp0_state.area_y0) miny = gp0_state.area_y0;
    if (maxx > gp0_state.area_x1 + 1) maxx = gp0_state.area_x1 + 1;
    if (maxy > gp0_state.area_y1 + 1) maxy = gp0_state.area_y1 + 1;
    if (minx >= maxx || miny >= maxy) {
        return;
    }
    int dither=(gp0_state.texpage&0x200) && (m->textured?!m->raw:m->gouraud);
    if(dither) palette_ready=0;
    else prepare_palette(m, a, minx, miny, maxx, maxy);
    /* edge function e_i(x, y) = A_i x + B_i y + C_i, for the edge opposite vertex i */
    int A0 = b->y - c->y, B0 = c->x - b->x, A1 = c->y - a->y, B1 = a->x - c->x, A2 = a->y - b->y, B2 = b->x - a->x;
    int w0row = A0 * (minx - b->x) + B0 * (miny - b->y);
    int w1row = A1 * (minx - c->x) + B1 * (miny - c->y);
    int w2row = A2 * (minx - a->x) + B2 * (miny - a->y);
    /* top-left rule: on a right/bottom edge the pixel belongs to the neighbour (bias the edge by -1) */
    int bias0 = (A0 > 0 || (A0 == 0 && B0 > 0)) ? 0 : -1;
    int bias1 = (A1 > 0 || (A1 == 0 && B1 > 0)) ? 0 : -1;
    int bias2 = (A2 > 0 || (A2 == 0 && B2 > 0)) ? 0 : -1;
    /* Quantize slopes to twelve fractional bits, truncating toward zero.
     * Keep the existing 16.16 stepping representation (four low bits zero).
     * A half-unit seed rounds RGB/UV at pixels instead of flooring them.
     * Decoded attributes are bytes. After the extent rejection above, each
     * determinant is bounded by 255*1023, so scaling by 4096 fits signed 32
     * bits. Avoid a software 64-bit divide for every attribute slope. */
#define GRAD(fa, fb, fc) \
    (int)((unsigned)((((fa) * A0 + (fb) * A1 + (fc) * A2) * 4096) / area) * 16u), \
    (int)((unsigned)((((fa) * B0 + (fb) * B1 + (fc) * B2) * 4096) / area) * 16u)
    /* Flat primitives use the first vertex colour directly. Do not compute
     * or step RGB interpolants that cannot affect their pixels. */
#ifdef PORT_SOFTGPU_REFERENCE
    const int interpolate_rgb = 1;
    const int interpolate_uv = 1;
#else
    const int interpolate_rgb = m->gouraud;
    const int interpolate_uv = m->textured;
#endif
    int drdx = 0, drdy = 0, dgdx = 0, dgdy = 0, dbdx = 0, dbdy = 0;
    int dudx = 0, dudy = 0, dvdx = 0, dvdy = 0;
    if (interpolate_rgb) {
        int tr[2] = {GRAD(a->r, b->r, c->r)};
        int tg[2] = {GRAD(a->g, b->g, c->g)};
        int tb[2] = {GRAD(a->b, b->b, c->b)};
        drdx = tr[0]; drdy = tr[1];
        dgdx = tg[0]; dgdy = tg[1];
        dbdx = tb[0]; dbdy = tb[1];
    }
    if (interpolate_uv) {
        int t[2] = {GRAD(a->u, b->u, c->u)}; dudx = t[0]; dudy = t[1];
    }
    if (interpolate_uv) {
        int t[2] = {GRAD(a->v, b->v, c->v)}; dvdx = t[0]; dvdy = t[1];
    }
#undef GRAD
    /* Evaluate from the same vertex using the quantized slopes. Computing
     * an independent barycentric value here loses the GPU's accumulated
     * rounding error when clipping or stepping from that vertex. */
#define AT(value, dx, dy) \
    (int)(((unsigned)(value) << 16) + 0x8000u + \
          (unsigned)(dx) * (unsigned)(minx - anchor->x) + \
          (unsigned)(dy) * (unsigned)(miny - anchor->y))
    int rrow = 0, grow = 0, brow = 0;
    if (interpolate_rgb) {
        rrow = AT(anchor->r, drdx, drdy);
        grow = AT(anchor->g, dgdx, dgdy);
        brow = AT(anchor->b, dbdx, dbdy);
    }
    int urow = 0, vrow = 0;
    if (interpolate_uv) {
        urow = AT(anchor->u, dudx, dudy);
        vrow = AT(anchor->v, dvdx, dvdy);
    }
#undef AT
    if (target.clip_active) {
        int skip_x = target.clip_x > minx ? target.clip_x - minx : 0;
        int skip_y = target.clip_y > miny ? target.clip_y - miny : 0;
        if (maxx > target.clip_x + target.clip_w) maxx = target.clip_x + target.clip_w;
        if (maxy > target.clip_y + target.clip_h) maxy = target.clip_y + target.clip_h;
        minx += skip_x; miny += skip_y;
        if (minx >= maxx || miny >= maxy) return;
        w0row += A0 * skip_x + B0 * skip_y;
        w1row += A1 * skip_x + B1 * skip_y;
        w2row += A2 * skip_x + B2 * skip_y;
#define ADVANCE(value, dx, dy) ((int)((unsigned)(value) + (unsigned)(dx) * (unsigned)skip_x + (unsigned)(dy) * (unsigned)skip_y))
        rrow = ADVANCE(rrow, drdx, drdy); grow = ADVANCE(grow, dgdx, dgdy); brow = ADVANCE(brow, dbdx, dbdy);
        urow = ADVANCE(urow, dudx, dudy); vrow = ADVANCE(vrow, dvdx, dvdy);
#undef ADVANCE
    }
#ifndef PORT_SOFTGPU_REFERENCE
    /* plot()'s colour for these pixels, decided once per polygon: see poly_row_textured. Gouraud texturing,
     * blending, mask tests, dithering and shadow targets keep the general path. */
    const int neutral = m->raw || (a->r == 128 && a->g == 128 && a->b == 128);
    const int fast_textured = m->textured && !m->gouraud && !m->semi && !gp0_state.check_mask &&
                              !target.pixels && !marks;
    const int modulate = !neutral;
    /* dither implies a modulated (non-raw) texture and no palette cache: see poly_row_dither_indexed */
    const int dither_indexed = fast_textured && dither && m->depth < 2 && !gp0_state.twin_mask_x &&
                               !gp0_state.twin_mask_y && m->clut_x + (m->depth ? 256 : 16) <= VRAM_W;
    const int flat_r = a->r < 0 ? 0 : a->r > 255 ? 255 : a->r;
    const int flat_g = a->g < 0 ? 0 : a->g > 255 ? 255 : a->g;
    const int flat_b = a->b < 0 ? 0 : a->b > 255 ? 255 : a->b;
    /* the same for semi-transparent polygons (any depth), and untextured polygons */
    const int plain_target = !gp0_state.check_mask && !target.pixels && !marks;
    const int dither_semi = m->textured && !m->gouraud && m->semi && dither && plain_target &&
                            !gp0_state.twin_mask_x && !gp0_state.twin_mask_y &&
                            (m->depth == 2 || m->clut_x + (m->depth ? 256 : 16) <= VRAM_W);
    const int untextured = !m->textured && plain_target;
    if (dither_indexed || dither_semi) dither_tables(flat_r, flat_g, flat_b);
#endif
    for (int y = miny; y < maxy; y++) {
        int w0 = w0row, w1 = w1row, w2 = w2row;
        int r = rrow, g = grow, bl = brow, u = urow, v = vrow;
        int left=minx,right=maxx;
#ifndef PORT_SOFTGPU_REFERENCE
        int first=0,last=maxx-minx,width=last;
        if(!clip_span(w0+bias0,A0,width,&first,&last) ||
           !clip_span(w1+bias1,A1,width,&first,&last) ||
           !clip_span(w2+bias2,A2,width,&first,&last)) last=first;
        left+=first;right=minx+last;
        /* Skipping uncovered pixels must reproduce repeated 32-bit adds,
         * including thin-triangle interpolants that wrap outside coverage. */
        if(interpolate_rgb) {
            r=(int)((unsigned)r+(unsigned)drdx*(unsigned)first);
            g=(int)((unsigned)g+(unsigned)dgdx*(unsigned)first);
            bl=(int)((unsigned)bl+(unsigned)dbdx*(unsigned)first);
        }
        if(interpolate_uv) {
            u=(int)((unsigned)u+(unsigned)dudx*(unsigned)first);
            v=(int)((unsigned)v+(unsigned)dvdx*(unsigned)first);
        }
#endif
#ifndef PORT_SOFTGPU_REFERENCE
        if (dither_indexed && right <= VRAM_W) {
            if (right > left) {
                poly_row_dither_indexed(m, psx_vram + (y & 511) * 1024 + left, right - left, u, v, dudx, dvdx,
                                        gp0_state.set_mask ? 0x8000 : 0, left, y);
            }
        } else if (dither_semi && right <= VRAM_W) {
            if (right > left)
                poly_row_dither_semi(m, psx_vram + (y & 511) * 1024 + left, right - left, u, v, dudx, dvdx,
                                     gp0_state.set_mask ? 0x8000 : 0, left, y);
        } else if (untextured && right <= VRAM_W) {
            if (right > left) {
                if (m->gouraud)
                    poly_row_untextured(m, psx_vram + (y & 511) * 1024 + left, right - left, r, g, bl, drdx, dgdx,
                                        dbdx, dither, gp0_state.set_mask ? 0x8000 : 0, left, y);
                else
                    poly_row_untextured(m, psx_vram + (y & 511) * 1024 + left, right - left, a->r << 16, a->g << 16,
                                        a->b << 16, 0, 0, 0, dither, gp0_state.set_mask ? 0x8000 : 0, left, y);
            }
        } else if (fast_textured && right <= VRAM_W) { /* rows past x 1023 wrap per pixel: general path */
            if (right > left)
                poly_row_textured(m, psx_vram + (y & 511) * 1024 + left, right - left, u, v, dudx, dvdx,
                                  gp0_state.set_mask ? 0x8000 : 0, modulate, flat_r, flat_g, flat_b, dither, left, y);
        } else if (!m->gouraud) {
            for (int x = left; x < right; x++) {
                if (RASTER_COVERED)
                    plot(m, x, y, a->r, a->g, a->b, u >> 16, v >> 16, dither);
                RASTER_STEP_EDGES();
                u = step_attribute(u, dudx); v = step_attribute(v, dvdx);
            }
        } else if (!m->textured) {
            /* Battle lighting overlays interpolate RGB but never consume UV.
             * Avoid both UV setup and the two per-pixel attribute steps. */
            for (int x = left; x < right; x++) {
                if (RASTER_COVERED)
                    plot(m, x, y, r >> 16, g >> 16, bl >> 16, 0, 0, dither);
                RASTER_STEP_EDGES();
                r = step_attribute(r, drdx); g = step_attribute(g, dgdx);
                bl = step_attribute(bl, dbdx);
            }
        } else
#endif
        for (int x = left; x < right; x++) {
            if (RASTER_COVERED) {
                if (m->gouraud) {
                    plot(m, x, y, r >> 16, g >> 16, bl >> 16, u >> 16, v >> 16, dither);
                } else {
                    plot(m, x, y, a->r, a->g, a->b, u >> 16, v >> 16, dither);
                }
            }
            RASTER_STEP_EDGES();
            r = step_attribute(r, drdx); g = step_attribute(g, dgdx);
            bl = step_attribute(bl, dbdx); u = step_attribute(u, dudx); v = step_attribute(v, dvdx);
        }
        w0row += B0;
        w1row += B1;
        w2row += B2;
        rrow = step_attribute(rrow, drdy); grow = step_attribute(grow, dgdy);
        brow = step_attribute(brow, dbdy); urow = step_attribute(urow, dudy); vrow = step_attribute(vrow, dvdy);
    }
}

static void sg_poly(const Gp0Mode *m, const Gp0Vtx *v, int n) {
    if (!psx_vram) {
        return;
    }
    unsigned int profile_start=port_profile_begin();
    triangle(m, &v[0], &v[1], &v[2]);
    if (n == 4) {
        /* Strip order matters for the attribute seed when X coordinates tie,
         * even though reversing these first two vertices covers the same area. */
        triangle(m, &v[2], &v[1], &v[3]);
    }
    port_profile_draw_end(0,profile_start);
}

void psx_softgpu_shadow_poly(const Gp0Mode *m, const Gp0Vtx *v, int n,
                            u16 *pixels, u8 *known, int x, int y, int w, int h) {
    target.pixels = pixels; target.known = known;
    target.x = x; target.y = y; target.w = w; target.h = h;
    Gp0State saved = gp0_state;
    if (gp0_state.area_x0 < x) gp0_state.area_x0 = x;
    if (gp0_state.area_y0 < y) gp0_state.area_y0 = y;
    if (gp0_state.area_x1 >= x+w) gp0_state.area_x1 = x+w-1;
    if (gp0_state.area_y1 >= y+h) gp0_state.area_y1 = y+h-1;
    sg_poly(m, v, n);
    gp0_state = saved;
    target.pixels = NULL;
}

void psx_softgpu_shadow_poly_region(const Gp0Mode *m, const Gp0Vtx *v, int n,
                                  u16 *pixels, u8 *known, int x, int y, int w, int h,
                                  int clip_x, int clip_y, int clip_w, int clip_h) {
    target.clip_active = 1;
    target.clip_x = clip_x; target.clip_y = clip_y; target.clip_w = clip_w; target.clip_h = clip_h;
    psx_softgpu_shadow_poly(m, v, n, pixels, known, x, y, w, h);
    target.clip_active = 0;
}

static SG_HOT void sg_line(const Gp0Mode *m, const Gp0Vtx *a, const Gp0Vtx *b) {
    palette_ready = 0;
    if (!psx_vram) {
        return;
    }
    int dx = b->x - a->x, dy = b->y - a->y;
    int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    int n = adx > ady ? adx : ady;
    if (n >= 1024) {
        return;
    }
    unsigned int profile_start=port_profile_begin();
    for (int i = 0; i <= n; i++) {
        int x = n ? a->x + dx * i / n : a->x, y = n ? a->y + dy * i / n : a->y;
        if (x < gp0_state.area_x0 || x > gp0_state.area_x1 || y < gp0_state.area_y0 || y > gp0_state.area_y1) {
            continue;
        }
        int r = a->r, g = a->g, bl = a->b;
        if (m->gouraud && n) {
            r += (b->r - a->r) * i / n;
            g += (b->g - a->g) * i / n;
            bl += (b->b - a->b) * i / n;
        }
        plot(m, x, y, r, g, bl, 0, 0, gp0_state.texpage&0x200);
    }
    port_profile_draw_end(2,profile_start);
}

#ifndef PORT_SOFTGPU_REFERENCE
/* effects is constant at each call site, retaining the opaque pixel loop
 * without mask/blend branches. The slow variants preserve original STP even
 * when modulation turns a nonzero source colour into black. */
static inline __attribute__((always_inline)) void sprite_pixel(
        const Gp0Mode *m, u16 *dst, u16 original, u16 colour, u16 mask, int effects) {
    if (!original) return;
    if (effects) {
        if (gp0_state.check_mask && (*dst & 0x8000)) return;
        if (m->semi && (original & 0x8000))
            colour = blend(*dst, colour, m->abr) | (colour & 0x8000);
    }
    *dst = colour | mask;
}

/* The row loops of fast_indexed_row, out of line so their few values stay in registers. src is the first
 * texel's halfword, lead the texel's position inside it; width >= 1. */
static SG_HOT __attribute__((noinline)) void row_direct8(const u16 *src, const u16 *cl, u16 *dst, unsigned lead,
                                                         unsigned width, u16 mask) {
    if (lead) { u16 c = cl[*src++ >> 8]; if (c) *dst = c | mask; ++dst; --width; }
    for (; width >= 2; width -= 2, dst += 2) {
        u16 w = *src++;
        u16 a = cl[w & 255], b = cl[w >> 8];
        if (a) dst[0] = a | mask;
        if (b) dst[1] = b | mask;
    }
    if (width) { u16 c = cl[*src & 255]; if (c) *dst = c | mask; }
}

static SG_HOT __attribute__((noinline)) void row_palette8(const u16 *src, const u32 *pal, u16 *dst, unsigned lead,
                                                          unsigned width, u16 mask) {
    if (lead) { u32 p = pal[*src++ >> 8]; if ((u16)p) *dst = (u16)(p >> 16) | mask; ++dst; --width; }
    for (; width >= 2; width -= 2, dst += 2) {
        u16 w = *src++;
        u32 a = pal[w & 255], b = pal[w >> 8];
        if ((u16)a) dst[0] = (u16)(a >> 16) | mask;
        if ((u16)b) dst[1] = (u16)(b >> 16) | mask;
    }
    if (width) { u32 p = pal[*src & 255]; if ((u16)p) *dst = (u16)(p >> 16) | mask; }
}

static SG_HOT __attribute__((noinline)) void row_direct4(const u16 *src, const u16 *cl, u16 *dst, unsigned lead,
                                                         unsigned width, u16 mask) {
    for (unsigned w = *src++ >> (lead * 4), k = lead; width; --width, ++dst) {
        u16 c = cl[w & 15];
        if (c) *dst = c | mask;
        if (++k == 4 && width > 1) { k = 0; w = *src++; } else w >>= 4; /* no read past the last texel */
    }
}

static SG_HOT __attribute__((noinline)) void row_palette4(const u16 *src, const u32 *pal, u16 *dst, unsigned lead,
                                                          unsigned width, u16 mask) {
    for (unsigned w = *src++ >> (lead * 4), k = lead; width; --width, ++dst) {
        u32 p = pal[w & 15];
        if ((u16)p) *dst = (u16)(p >> 16) | mask;
        if (++k == 4 && width > 1) { k = 0; w = *src++; } else w >>= 4; /* no read past the last texel */
    }
}

/* The common sprite row: indexed texels, no texture window, no wrap in u, texture X or CLUT, opaque, and a
 * destination that neither this row's texels nor its CLUT entries overlap (so reading each source halfword
 * once for its 2 or 4 pixels sees the same values as per-pixel reads). Returns 0 for the general loop. */
static inline __attribute__((always_inline)) int fast_indexed_row(
        const Gp0Mode *m, const u16 *row, unsigned row_y, u16 *dst, int y, int x0, int x1, unsigned cursor,
        unsigned keep_x, unsigned window_x, u16 mask, int direct) {
    if (keep_x != ~0u || window_x) return 0;
    /* A rectangle clipped to nothing can have x1 < x0: nothing to draw, but the unsigned width would wrap the
     * u range check below and run the row loop over ~4 GiB (seen on DS as stray pixels in DTCM). */
    if (x1 <= x0) return 1;
    unsigned u = cursor & 255, width = (unsigned)(x1 - x0);
    if (width > 256 || u + width > 256) return 0;
    unsigned shift = m->depth ? 1 : 2; /* texels per halfword: 2 or 4 */
    unsigned tx0 = m->tx + (u >> shift), tx1 = m->tx + ((u + width - 1) >> shift);
    if (tx1 >= 1024) return 0;
    unsigned entries = m->depth ? 256 : 16;
    const u16 *clut = psx_vram + (m->clut_y & 511) * 1024;
    if (direct && m->clut_x + entries > 1024) return 0;
    if (row_y == (unsigned)y && tx0 < (unsigned)x1 && tx1 >= (unsigned)x0) return 0;
    if (direct && (m->clut_y & 511) == (unsigned)y && (unsigned)m->clut_x < (unsigned)x1 &&
        m->clut_x + entries > (unsigned)x0) return 0;
    const u16 *src = row + tx0;
    if (direct) {
        if (m->depth) row_direct8(src, clut + m->clut_x, dst, u & 1, width, mask);
        else row_direct4(src, clut + m->clut_x, dst, u & 3, width, mask);
    } else {
        if (m->depth) row_palette8(src, shaded_palette, dst, u & 1, width, mask);
        else row_palette4(src, shaded_palette, dst, u & 3, width, mask);
    }
    return 1;
}

static inline __attribute__((always_inline)) void sprite_rows(
        const Gp0Mode *m, int u0, int v0, int x0, int y0, int x1, int y1, int effects, int direct) {
    unsigned keep_x=~gp0_state.twin_mask_x,keep_y=~gp0_state.twin_mask_y;
    unsigned window_x=gp0_state.twin_off_x&gp0_state.twin_mask_x;
    unsigned window_y=gp0_state.twin_off_y&gp0_state.twin_mask_y;
    u16 mask=gp0_state.set_mask ? 0x8000 : 0;
    for(int y=y0;y<y1;y++) {
        unsigned tv=((((unsigned)v0+(unsigned)y)&keep_y)|window_y)&255;
        const u16 *row=psx_vram+(((m->ty+tv)&511)*1024);
        u16 *dst=psx_vram+y*1024+x0;
        unsigned cursor=(unsigned)u0+(unsigned)x0;
        if(m->depth<2 && !effects && fast_indexed_row(m, row, (unsigned)((m->ty+tv)&511), dst, y, x0, x1,
                                                      cursor, keep_x, window_x, mask, direct))
            continue;
        if(direct) {
            /* Unmodulated indexed texels: the colour is the CLUT entry itself, read live from VRAM as plot()
             * does, so a sprite that overwrites its own CLUT still sees each entry as it is at that pixel. */
            const u16 *clut=psx_vram+(m->clut_y&511)*1024;
            unsigned clut_x=m->clut_x;
            if(m->depth==0) for(int x=x0;x<x1;x++,cursor++,dst++) {
                unsigned u=((cursor&keep_x)|window_x)&255;
                u16 packed=row[(m->tx+(u>>2))&1023];
                u16 sample=clut[(clut_x+((packed>>((u&3)*4))&15))&1023];
                sprite_pixel(m, dst, sample, sample, mask, effects);
            }
            else for(int x=x0;x<x1;x++,cursor++,dst++) {
                unsigned u=((cursor&keep_x)|window_x)&255;
                u16 packed=row[(m->tx+(u>>1))&1023];
                u16 sample=clut[(clut_x+((packed>>((u&1)*8))&255))&1023];
                sprite_pixel(m, dst, sample, sample, mask, effects);
            }
        }
        else if(m->depth==0) for(int x=x0;x<x1;x++,cursor++,dst++) {
            unsigned u=((cursor&keep_x)|window_x)&255;
            u16 packed=row[(m->tx+(u>>2))&1023];
            u32 sample=shaded_palette[(packed>>((u&3)*4))&15];
            sprite_pixel(m, dst, (u16)sample, sample >> 16, mask, effects);
        }
        else if(m->depth==1) for(int x=x0;x<x1;x++,cursor++,dst++) {
            unsigned u=((cursor&keep_x)|window_x)&255;
            u16 packed=row[(m->tx+(u>>1))&1023];
            u32 sample=shaded_palette[(packed>>((u&1)*8))&255];
            sprite_pixel(m, dst, (u16)sample, sample >> 16, mask, effects);
        }
        else for(int x=x0;x<x1;x++,cursor++,dst++) {
            unsigned u=((cursor&keep_x)|window_x)&255;
            u16 sample=row[(m->tx+u)&1023];
            sprite_pixel(m, dst, sample, sample, mask, effects);
        }
    }
}
#endif

static SG_HOT void sg_rect(const Gp0Mode *m, const Gp0Vtx *v, int w, int h) {
    if (!psx_vram) {
        return;
    }
    unsigned int profile_start=port_profile_begin();
    int x0 = v->x, y0 = v->y, x1 = v->x + w, y1 = v->y + h;
    int u0 = v->u - x0, v0 = v->v - y0;
    if (x0 < gp0_state.area_x0) x0 = gp0_state.area_x0;
    if (y0 < gp0_state.area_y0) y0 = gp0_state.area_y0;
    if (x1 > gp0_state.area_x1 + 1) x1 = gp0_state.area_x1 + 1;
    if (y1 > gp0_state.area_y1 + 1) y1 = gp0_state.area_y1 + 1;
#ifndef PORT_SOFTGPU_REFERENCE
    if (!m->textured && !m->semi && !gp0_state.check_mask && !target.pixels
        && x0 >= 0 && y0 >= 0 && x1 <= VRAM_W && y1 <= VRAM_H) {
        int r = v->r < 0 ? 0 : v->r > 255 ? 255 : v->r;
        int g = v->g < 0 ? 0 : v->g > 255 ? 255 : v->g;
        int b = v->b < 0 ? 0 : v->b > 255 ? 255 : v->b;
        u16 c = (r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10)
              | (gp0_state.set_mask ? 0x8000 : 0);
        for (int y=y0; y<y1; ++y) {
            u16 *row = px(x0,y);
            for (int x=x0; x<x1; ++x) *row++ = c;
        }
        port_profile_draw_end(1,profile_start);
        return;
    }
#endif
    prepare_palette(m, v, x0, y0, x1, y1);
#ifndef PORT_SOFTGPU_REFERENCE
    const int unmodulated = m->raw || (v->r==128 && v->g==128 && v->b==128);
    if(m->textured && !target.pixels &&
       x0>=0 && y0>=0 && x1<=VRAM_W && y1<=VRAM_H &&
       (palette_ready || unmodulated)) {
        /* Cached-indexed or unmodulated sprites avoid per-pixel colour setup. Keep texture and CLUT reads live
         * for overlapping source/dest. prepare_palette rejects CLUT feedback; shadows use generic plot.
         * Neutral 128 modulation reproduces each texel exactly, as raw does. */
        const int direct = !palette_ready && m->depth < 2;
        if (m->semi || gp0_state.check_mask) {
            if (direct) sprite_rows(m, u0, v0, x0, y0, x1, y1, 1, 1);
            else sprite_rows(m, u0, v0, x0, y0, x1, y1, 1, 0);
        } else {
            if (direct) sprite_rows(m, u0, v0, x0, y0, x1, y1, 0, 1);
            else sprite_rows(m, u0, v0, x0, y0, x1, y1, 0, 0);
        }
        port_profile_draw_end(1,profile_start);
        return;
    }
#endif
    for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) {
            plot(m, x, y, v->r, v->g, v->b, u0 + x, v0 + y, 0);
        }
    }
    port_profile_draw_end(1,profile_start);
}

void psx_softgpu_shadow_rect(const Gp0Mode *m, const Gp0Vtx *v, int width, int height,
                             u16 *pixels, u8 *known, int x, int y, int w, int h) {
    target.pixels = pixels; target.known = known;
    target.x = x; target.y = y; target.w = w; target.h = h;
    Gp0State saved = gp0_state;
    if (gp0_state.area_x0 < x) gp0_state.area_x0 = x;
    if (gp0_state.area_y0 < y) gp0_state.area_y0 = y;
    if (gp0_state.area_x1 >= x + w) gp0_state.area_x1 = x + w - 1;
    if (gp0_state.area_y1 >= y + h) gp0_state.area_y1 = y + h - 1;
    sg_rect(m, v, width, height);
    gp0_state = saved;
    target.pixels = NULL;
}

const Gp0Backend psx_softgpu = {sg_poly, sg_line, sg_rect, 0, 0};

/* PC --render / DS debug: rasterise into the VRAM shadow */
void psx_softgpu_enable(int on) {
    gp0_backend = on ? &psx_softgpu : 0;
}
