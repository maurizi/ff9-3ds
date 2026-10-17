/* SPDX-License-Identifier: GPL-2.0-or-later
 * Integer MDEC reconstruction. Dequantization precision, signed-nine-bit
 * clamps and colour rounding are adapted from Mednafen's PSX MDEC research
 * (Copyright (C) 2011-2016 Mednafen Team). See third_party/mednafen/README.md.
 * Matrix passes retain the caller's full signed scale values. */
#include "mdec_decode.h"
#include <stdlib.h>
#include <string.h>

/* Zigzag rank -> row-major frequency coordinate. */
static const uint8_t zigzag[64]={
    0,1,8,16,9,2,3,10,17,24,32,25,18,11,4,5,
    12,19,26,33,40,48,41,34,27,20,13,6,7,14,21,28,
    35,42,49,56,57,50,43,36,29,22,15,23,30,37,44,51,
    58,59,52,45,38,31,39,46,53,60,61,54,47,55,62,63
};
static inline int clamp(int x,int lo,int hi) {return x<lo ? lo : x>hi ? hi : x;}
static int signed10(unsigned x) {return (int)(x&1023)-((x&512) ? 1024 : 0);}
/* floor(x / 2^shift): an arithmetic right shift. */
static inline int floor_shift(int32_t x,unsigned shift) {
    return x>>shift;
}
/* Wrap to signed nine bits, then clamp to a signed byte. The result only depends on x & 511, so it is a table. */
static int8_t clamp9_table[512];
static inline int clamp9(int x) {
    return clamp9_table[(unsigned)x&511];
}

void psx_mdec_env_init(PsxMdecEnv *e,const uint8_t quant[128],const uint8_t scale[128]) {
    for (int i=0;i<512;i++) clamp9_table[i]=(int8_t)clamp(i>=256 ? i-512 : i,-128,127);
    memcpy(e->quant,quant,128);
    for (unsigned i=0;i<64;i++) {
        unsigned value=scale[i*2]|((unsigned)scale[i*2+1]<<8);
        e->scale[i]=(int)(value&32767)-((value&32768) ? 32768 : 0);
    }
    e->scale_max=0;
    for (unsigned i=0;i<64;i++) {
        uint32_t m=(uint32_t)abs(e->scale[i]);
        if (m>e->scale_max) e->scale_max=m;
    }
}

/* Sparse two-pass IDCT, equal to the full matrix products: the vertical pass only visits the nonzero
 * coefficients, and a column without any gives exactly zero after +131072 >> 18, so the horizontal pass only
 * visits the columns that had one (`cols` is their bitmask). Coefficients reach +-16384 and the uploaded scales
 * +-32768, so a sum can exceed 32 bits: each pass uses 32-bit arithmetic only when the block's own bound
 * (sum of |terms| times the largest |scale|) proves it fits, and 64-bit otherwise. Right shifts of negative
 * values are arithmetic, which is floor division. */
#define FITS32(bound) ((bound)<=(INT32_MAX-131072))
static void idct(const PsxMdecEnv *e,int16_t block[64],const uint8_t *nz,unsigned count,unsigned cols) {
    int32_t temp[64];
    unsigned used[8],n=0;
    uint32_t colsum[8]={0};
    for (unsigned i=0;i<count;i++) colsum[nz[i]&7]+=(uint32_t)abs(block[nz[i]]);
    uint64_t bound1=0;
    for (unsigned x=0;x<8;x++) if (cols&(1u<<x)) {
        used[n++]=x;
        if (colsum[x]>bound1) bound1=colsum[x];
    }
    const uint32_t smax=e->scale_max;
    if (FITS32(bound1*smax)) {
        int32_t acc[64];
        for (unsigned k=0;k<n;k++) for (unsigned y=0;y<8;y++) acc[y*8+used[k]]=131072;
        for (unsigned i=0;i<count;i++) {
            unsigned at=nz[i],x=at&7;
            int32_t c=block[at];
            const int16_t *row=&e->scale[(at>>3)*8];
            for (unsigned y=0;y<8;y++) acc[y*8+x]+=c*row[y];
        }
        for (unsigned y=0;y<8;y++) for (unsigned k=0;k<n;k++) temp[y*8+used[k]]=acc[y*8+used[k]]>>18;
    } else {
        int64_t acc[64];
        for (unsigned k=0;k<n;k++) for (unsigned y=0;y<8;y++) acc[y*8+used[k]]=131072;
        for (unsigned i=0;i<count;i++) {
            unsigned at=nz[i],x=at&7;
            int32_t c=block[at];
            const int16_t *row=&e->scale[(at>>3)*8];
            for (unsigned y=0;y<8;y++) acc[y*8+x]+=(int64_t)c*row[y];
        }
        for (unsigned y=0;y<8;y++) for (unsigned k=0;k<n;k++) temp[y*8+used[k]]=(int32_t)(acc[y*8+used[k]]>>18);
    }
    for (unsigned y=0;y<8;y++) {
        const int32_t *t=&temp[y*8];
        uint64_t bound2=0;
        for (unsigned k=0;k<n;k++) bound2+=(uint64_t)(t[used[k]]<0 ? -(int64_t)t[used[k]] : t[used[k]]);
        if (FITS32(bound2*smax)) {
            for (unsigned x=0;x<8;x++) {
                int32_t sum=131072;
                for (unsigned k=0;k<n;k++) sum+=t[used[k]]*e->scale[used[k]*8+x];
                block[y*8+x]=clamp9(sum>>18);
            }
        } else {
            for (unsigned x=0;x<8;x++) {
                int64_t sum=131072;
                for (unsigned k=0;k<n;k++) sum+=(int64_t)t[used[k]]*e->scale[used[k]*8+x];
                block[y*8+x]=clamp9((int)(sum>>18));
            }
        }
    }
}

static int block(const PsxMdecEnv *e,const uint8_t *qt,const uint16_t *rl,size_t size,
                 size_t *position,int16_t pixels[64]) {
    memset(pixels,0,64*sizeof *pixels);
    uint8_t nz[64];
    unsigned count=0,cols=0;
    unsigned first;
    do {
        if (*position==size) return PSX_MDEC_TRUNCATED;
        first=rl[(*position)++];
    } while (first==0xfe00);
    unsigned q=first>>10,rank=0;
    int level=signed10(first);
    int dc=q ? level*qt[0]*16+(level<0 ? 8 : level>0 ? -8 : 0) : level*32;
    pixels[0]=clamp(dc,-16384,16383);
    if (pixels[0]) nz[count++]=0,cols|=1;
    while (rank<63) {
        if (*position==size) return PSX_MDEC_TRUNCATED;
        unsigned code=rl[(*position)++];
        rank+=(code>>10)+1;
        if (rank>63) break;
        level=signed10(code);
        unsigned quant=q*qt[rank];
        int coefficient=quant ? floor_shift(level*(int)quant,3)*16+
                               (level<0 ? 8 : level>0 ? -8 : 0) : level*32;
        unsigned at=zigzag[rank];
        int value=clamp(coefficient,-16384,16383);
        if (value) nz[count++]=at,cols|=1u<<(at&7); /* ranks only increase: each position is written once */
        pixels[at]=value;
    }
    idct(e,pixels,nz,count,cols);
    return PSX_MDEC_OK;
}

int psx_mdec_macroblock(const PsxMdecEnv *e,const uint16_t *rl,size_t halfwords,
                       uint32_t command,uint8_t *pixels,size_t capacity,size_t *consumed) {
    *consumed=0;
    unsigned depth=(command>>27)&3;
    if ((command>>29)!=1 || depth<2) return PSX_MDEC_MODE;
    unsigned bytes=depth==2 ? 768 : 512;
    if (capacity<bytes) return PSX_MDEC_CAPACITY;
    int16_t samples[6][64];
    size_t at=0;
    for (unsigned i=0;i<6;i++) {
        int result=block(e,e->quant[i<2],rl,halfwords,&at,samples[i]);
        if (result) return result;
    }
    /* Each Cr/Cb pair colours a 2x2 pixel group: compute its three offsets once. */
    unsigned flip=(command&0x04000000) ? 128 : 0;
    unsigned stp=(command&0x02000000) ? 0x8000 : 0;
    for (unsigned cy=0;cy<8;cy++) for (unsigned cx=0;cx<8;cx++) {
        int cr=samples[0][cy*8+cx], cb=samples[1][cy*8+cx];
        int dr=floor_shift(359*cr+128,8);
        int dg=floor_shift(floor_shift(-88*cb,5)*32+floor_shift(-183*cr,3)*8+128,8);
        int db=floor_shift(454*cb+128,8);
        for (unsigned dy=0;dy<2;dy++) for (unsigned dx=0;dx<2;dx++) {
            unsigned y=cy*2+dy, x=cx*2+dx, offset=y*16+x;
            int lum=samples[2+(y/8)*2+x/8][(y&7)*8+(x&7)];
            unsigned r=(unsigned)(clamp9(lum+dr)+128)^flip;
            unsigned g=(unsigned)(clamp9(lum+dg)+128)^flip;
            unsigned b=(unsigned)(clamp9(lum+db)+128)^flip;
            if (depth==2) {
                pixels[offset*3]=r;pixels[offset*3+1]=g;pixels[offset*3+2]=b;
            } else {
                /* Signed mode selects the byte representation before reducing
                 * channels to five bits, including the rounding boundary at 127. */
                unsigned color=stp|(r+4>=256 ? 31u : (r+4)>>3)|(g+4>=256 ? 31u : (g+4)>>3)<<5|
                               (b+4>=256 ? 31u : (b+4)>>3)<<10;
                pixels[offset*2]=color;pixels[offset*2+1]=color>>8;
            }
        }
    }
    *consumed=at;
    return PSX_MDEC_OK;
}
