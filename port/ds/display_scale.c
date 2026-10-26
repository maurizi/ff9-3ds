#include "display_scale.h"
#include "../include/psx_display.h"
#include <string.h>

static inline uint32_t rgb24_to_ds(uint32_t rgb) {
    return 0x8000 | ((rgb >> 3) & 31) | ((rgb >> 6) & 0x3e0) | ((rgb >> 9) & 0x7c00);
}

#if 0 /* (not in ITCM: 24-bit movies play from their transcodes; the MDEC's are the fallback) */
__attribute__((section(".itcm"), long_call))
#endif
void ds_scale_display_rgb24(uint16_t *dst, const uint16_t *vram,
                           int x, int y, int width, int height) {
    if (width <= 0 || height <= 0) return;
    if (width > 320) width = 320;
    if (height > 240) height = 240;
    int out_w = width * 4 / 5, out_h = height * 4 / 5;
    unsigned start = ((unsigned)x & 1022) * 2; /* display X counts halfwords; bit 0 is ignored */
    for (int oy = 0; oy < out_h; oy++) {
        const uint8_t *row = (const uint8_t *)(vram + ((unsigned)(y + oy * 5 / 4) & 511) * 1024);
        uint16_t *out = dst + oy * 256;
        int ox = 0;
        /* Four nearest samples (source pixels 0, 1, 2, 3 of each five) from 15 bytes, unless the row wraps. */
        if (start + (unsigned)width * 3 <= 2048 && !((uintptr_t)out & 3)) {
            const uint8_t *in = row + start;
            uint32_t *pairs = (uint32_t *)out;
            for (; ox + 4 <= out_w; ox += 4, in += 15) {
                uint32_t p0 = rgb24_to_ds(in[0] | (uint32_t)in[1] << 8 | (uint32_t)in[2] << 16);
                uint32_t p1 = rgb24_to_ds(in[3] | (uint32_t)in[4] << 8 | (uint32_t)in[5] << 16);
                uint32_t p2 = rgb24_to_ds(in[6] | (uint32_t)in[7] << 8 | (uint32_t)in[8] << 16);
                uint32_t p3 = rgb24_to_ds(in[9] | (uint32_t)in[10] << 8 | (uint32_t)in[11] << 16);
                pairs[ox / 2] = p0 | p1 << 16;
                pairs[ox / 2 + 1] = p2 | p3 << 16;
            }
        }
        for (; ox < out_w; ox++)
            out[ox] = rgb24_to_ds(psx_display_rgb24(vram, x, y + oy * 5 / 4, ox * 5 / 4));
    }
}

/* Ten bits per channel leave room for the 25-fold coverage sum without
 * carries between RGB channels. Ignore PSX STP and set DS opacity at output. */
static inline uint32_t spread(uint16_t c) {
    return (c & 31) | ((uint32_t)(c & 0x3e0) << 5)
                   | ((uint32_t)(c & 0x7c00) << 10);
}

#ifdef PORT_SCALE_SPARE /* the game's DS build: in PS1 RAM the port keeps spare (psx_spare_alloc), not bss */
void *psx_spare_alloc(unsigned int size);
static uint32_t (*text_mask)[8];
#else
static uint32_t text_mask[192][8];
#endif

#ifdef PORT_SCALE_ITCM
__attribute__((section(".itcm"), long_call))
#endif
void ds_scale_display_text_pair(uint16_t *dst, const uint16_t *vram,
                          int x, int y, int width, int height,
                          const DsTextRect *rects, unsigned count,
                          const DsTextRect *previous, unsigned previous_count) {
    if (width <= 0 || height <= 0) return;
    if (width > 320) width = 320;
    if (height > 240) height = 240;
    int out_w=width*4/5, out_h=height*4/5;
    /* Scene pixels keep the existing nearest-neighbour appearance. */
    for(int oy=0;oy<out_h;++oy) {
        const uint16_t *src=vram+((y+oy*5/4)&511)*1024;
        uint16_t *out=dst+oy*256;
        unsigned base_x=(unsigned)x&1023;
        int ox=0;
        if(base_x+(unsigned)width<=1024 && !((uintptr_t)out&3)) {
            const uint16_t *in=src+base_x;
            uint32_t *pairs=(uint32_t *)out;
            /* Four nearest samples consume five source pixels. Pair stores
             * reduce bitmap bus writes without changing sampling or opacity. */
            for(;ox+4<=out_w;ox+=4,in+=5) {
                pairs[ox/2]=(in[0]|((uint32_t)in[1]<<16))|0x80008000u;
                pairs[ox/2+1]=(in[2]|((uint32_t)in[3]<<16))|0x80008000u;
            }
        }
        for(;ox<out_w;++ox) out[ox]=src[(base_x+ox*5/4)&1023]|0x8000;
    }
    if (!count && !previous_count) return;
#ifdef PORT_SCALE_SPARE
    if (!text_mask) text_mask = psx_spare_alloc(192 * sizeof *text_mask);
#endif
    /* only the rows and 32-pixel groups this area covers (the hybrid scales 32x32 blocks) */
    const int groups=(out_w+31)>>5;
    for(int oy=0;oy<out_h;++oy) for(int group=0;group<groups;++group) text_mask[oy][group]=0;
    for(unsigned i=0;i<count+previous_count;++i) {
        const DsTextRect *rect=i<count ? rects+i : previous+i-count;
        /* most glyphs are nowhere near this area (the hybrid scales 32x32 blocks): a plain overlap test first,
         * where neither wraps around VRAM's edges */
        if(x+width<=1024 && y+height<=512 && rect->x+rect->w<=1024 && rect->y+rect->h<=512 &&
           (rect->x+rect->w+2<x || rect->x>x+width+2 || rect->y+rect->h+2<y || rect->y>y+height+2)) continue;
        int rx=(rect->x-x)&1023, ry=(rect->y-y)&511;
        if(rx>=512) rx-=1024;
        if(ry>=256) ry-=512;
        /* Include output cells whose two source samples cross the glyph's
         * border. The same cell can cover adjacent glyphs; filter it once. */
        int x0=rx>0 ? (rx-1)*4/5 : 0;
        int y0=ry>0 ? (ry-1)*4/5 : 0;
        int x1=(rx+rect->w)*4/5+1;
        int y1=(ry+rect->h)*4/5+1;
        if(x1>out_w) x1=out_w;
        if(y1>out_h) y1=out_h;
        if(x0>=x1 || y0>=y1) continue;
        unsigned first=(unsigned)x0>>5,last=(unsigned)(x1-1)>>5;
        uint32_t low=~0u<<(x0&31),high=~0u>>(31-((x1-1)&31));
        for(int oy=y0;oy<y1;++oy) {
            uint32_t *row=text_mask[oy];
            if(first==last) row[first]|=low&high;
            else {
                row[first]|=low;
                for(unsigned group=first+1;group<last;++group) row[group]=~0u;
                row[last]|=high;
            }
        }
    }
    for(int oy=0;oy<out_h;++oy) {
        int sy=oy*5/4, fy=oy&3;
        int next_y=sy+1<height ? sy+1 : height-1;
        const uint16_t *a=vram+((y+sy)&511)*1024;
        const uint16_t *b=vram+((y+next_y)&511)*1024;
        for(int group=0;group<groups;++group) {
            uint32_t mask=text_mask[oy][group];
            while(mask) {
                unsigned bit=__builtin_ctz(mask);
                int ox=group*32+bit, sx=ox*5/4, fx=ox&3;
                int next_x=sx+1<width ? sx+1 : width-1;
                unsigned x0=(x+sx)&1023,x1=(x+next_x)&1023;
                /* Four equal colours average to that colour ((25v+12)*1311>>15 == v for every 5-bit v), which
                 * the nearest pass already wrote: skip them. STP is ignored by both. */
                if(!((a[x0]^a[x1])&0x7fff) && !((a[x0]^b[x0])&0x7fff) && !((a[x0]^b[x1])&0x7fff)) {
                    mask&=mask-1;
                    continue;
                }
                uint32_t top=spread(a[x0])*(4-fx)+spread(a[x1])*(fx+1);
                uint32_t bottom=spread(b[x0])*(4-fx)+spread(b[x1])*(fx+1);
                uint32_t c=top*(4-fy)+bottom*(fy+1);
                unsigned r=(((c&1023)+12)*1311)>>15;
                unsigned g=((((c>>10)&1023)+12)*1311)>>15;
                unsigned bl=(((c>>20)+12)*1311)>>15;
                dst[oy*256+ox]=0x8000|r|(g<<5)|(bl<<10);
                mask&=mask-1;
            }
        }
    }
}

void ds_scale_display_text(uint16_t *dst, const uint16_t *vram,
                          int x, int y, int width, int height,
                          const DsTextRect *rects, unsigned count) {
    ds_scale_display_text_pair(dst,vram,x,y,width,height,rects,count,0,0);
}

/* Tests only (the game presents through the text-pair and RGB24 scalers): not in ITCM. */
void ds_scale_display(uint16_t *dst, const uint16_t *vram,
                      int x, int y, int width, int height) {
    if (width <= 0 || height <= 0) return;
    if (width > 320) width = 320;
    if (height > 240) height = 240;
    int out_w = width * 4 / 5, out_h = height * 4 / 5;
    uint32_t rows[5][256];
    for (int block = 0; block < out_h; block += 4) {
        int source_y = block * 5 / 4;
        for (int j = 0; j < 5; ++j) {
            int sy = source_y + j;
            if (sy >= height) sy = height - 1;
            const uint16_t *src = vram + ((y + sy) & 511) * 1024;
            int ox = 0;
            for (; ox + 4 <= out_w; ox += 4) {
                int sx = ox * 5 / 4;
                uint32_t a = spread(src[(x + sx) & 1023]);
                uint32_t b = spread(src[(x + sx + 1) & 1023]);
                uint32_t c = spread(src[(x + sx + 2) & 1023]);
                uint32_t d = spread(src[(x + sx + 3) & 1023]);
                uint32_t e = spread(src[(x + sx + 4) & 1023]);
                rows[j][ox] = 4*a+b;
                rows[j][ox+1] = 3*b+2*c;
                rows[j][ox+2] = 2*c+3*d;
                rows[j][ox+3] = d+4*e;
            }
            for (; ox < out_w; ++ox) {
                int sx = ox * 5 / 4, fraction = ox & 3;
                int next = sx + 1 < width ? sx + 1 : width - 1;
                rows[j][ox] = spread(src[(x + sx) & 1023]) * (4 - fraction)
                             + spread(src[(x + next) & 1023]) * (fraction + 1);
            }
        }
        for (int j = 0; j < 4 && block + j < out_h; ++j) {
            uint16_t *out = dst + (block + j) * 256;
            for (int ox = 0; ox < out_w; ++ox) {
                uint32_t c = rows[j][ox] * (4 - j) + rows[j + 1][ox] * (j + 1);
                /* Exact rounded division by 25 for the channel's 0..775
                 * range, using a small multiply instead of a wide UMULL. */
                unsigned r = (((c & 1023) + 12) * 1311) >> 15;
                unsigned g = ((((c >> 10) & 1023) + 12) * 1311) >> 15;
                unsigned b = (((c >> 20) + 12) * 1311) >> 15;
                out[ox] = 0x8000 | r | (g << 5) | (b << 10);
            }
        }
    }
}
