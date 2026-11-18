/* SPDX-License-Identifier: GPL-2.0-or-later
 * Integer MDEC reconstruction. Dequantization precision, signed-nine-bit
 * clamps and colour rounding are adapted from Mednafen's PSX MDEC research
 * (Copyright (C) 2011-2016 Mednafen Team). See third_party/mednafen/README.md.
 * Matrix passes retain the caller's full signed scale values. */
#include "mdec_decode.h"
#include <stdlib.h>
#include <string.h>

/* Zigzag rank -> row-major frequency coordinate. */
MDEC_DTCM_DATA static const uint8_t zigzag[64]={
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
/* Wrap to signed nine bits, then clamp to a signed byte. The result only depends on x & 511, so it is a table.
 * channel8/channel5 continue to output values: +128, the signed-mode flip (index 1), and for 15-bit output the
 * reduction to five bits. Signed mode selects the byte representation before that reduction, including the
 * rounding boundary at 127. */
MDEC_DTCM_BSS static int8_t clamp9_table[512];
MDEC_DTCM_BSS static uint8_t channel8[2][512],channel5[2][512];
/* the colour offsets of a Cr/Cb pair (psx_mdec_macroblock_with's), by value: the IDCT's results are clamp9's, -128..127.
 * Green's is (green_cb + green_cr + 128) >> 8, the same sum as computed whole */
static int16_t red_cr[256],blue_cb[256];
/* 15-bit output's five-bit channels by the sum itself (luma + offset, -355..353), each already in its place: channel5
 * of (sum & 511), shifted left 0, 5 and 10 (rgb5[flip][channel][sum + 512]). The same values with no mask or shift per
 * pixel: the colour stage was some 29 instructions a pixel (a third of a macroblock's decode) */
static uint16_t rgb5[2][3][1024];
static int32_t green_cb[256],green_cr[256];
static inline int clamp9(int x) {
    return clamp9_table[(unsigned)x&511];
}

void psx_mdec_env_init(PsxMdecEnv *e,const uint8_t quant[128],const uint8_t scale[128]) {
    for (int c=-128;c<128;c++) {
        red_cr[c+128]=(int16_t)floor_shift(359*c+128,8);
        blue_cb[c+128]=(int16_t)floor_shift(454*c+128,8);
        green_cb[c+128]=floor_shift(-88*c,5)*32;
        green_cr[c+128]=floor_shift(-183*c,3)*8;
    }
    for (int i=0;i<512;i++) {
        clamp9_table[i]=(int8_t)clamp(i>=256 ? i-512 : i,-128,127);
        for (unsigned flip=0;flip<2;flip++) {
            unsigned value=(unsigned)(clamp9_table[i]+128)^(flip ? 128 : 0);
            channel8[flip][i]=(uint8_t)value;
            channel5[flip][i]=(uint8_t)clamp((int)(value+4)/8,0,31);
        }
    }
    for (unsigned flip=0;flip<2;flip++)
        for (int v=-512;v<512;v++)
            for (unsigned c=0;c<3;c++) rgb5[flip][c][v+512]=(uint16_t)(channel5[flip][(unsigned)v&511]<<(5*c));
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
    static const unsigned pair_rows[4][2]={{0,2},{4,6},{1,3},{5,7}};
    for (unsigned b=0;b<4;b++) for (unsigned j=0;j<4;j++)
        e->pairs[b][j]=(uint16_t)e->scale[pair_rows[j][0]*8+b]|(uint32_t)(uint16_t)e->scale[pair_rows[j][1]*8+b]<<16;
    /* The standard libpress matrix: even rows symmetric, odd rows mirrored as -s-1 (rounded down). */
    e->mirrored=1;
    for (unsigned u=0;u<8;u++) for (unsigned x=0;x<4;x++) {
        int a=e->scale[u*8+x],b=e->scale[u*8+7-x];
        if (b!=((u&1) ? -a-1 : a)) e->mirrored=0;
    }
}

/* Sparse two-pass IDCT, equal to the full matrix products: the vertical pass only visits the nonzero
 * coefficients, and a column without any gives exactly zero after +131072 >> 18, so the horizontal pass only
 * visits the columns that had one (`cols` is their bitmask). Coefficients reach +-16384 and the uploaded scales
 * +-32768, so a sum can exceed 32 bits: each pass uses 32-bit arithmetic only when the block's own bound
 * (sum of |terms| times the largest |scale|) proves it fits, and 64-bit otherwise. Right shifts of negative
 * values are arithmetic, which is floor division. */
#define FITS32(bound) ((bound)<=(INT32_MAX-131072))
MDEC_ITCM static void idct(const PsxMdecEnv *e,int16_t block[64],const uint8_t *nz,unsigned count,unsigned cols) {
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
    if (e->mirrored) {
        /* Mirrored matrix: row 7-y (and column 7-x below) reuses the even/odd partial sums of y (x):
         * sum over odd rows of c*(-s-1) is -(odd sum) - (sum of those c), exactly. */
        if (!FITS32(bound1*(smax+1))) goto wide;
        int32_t ev[32],od[32],todd[8];
        for (unsigned k=0;k<n;k++) {
            unsigned x=used[k];
            todd[x]=0;
            for (unsigned y=0;y<4;y++) ev[y*8+x]=131072,od[y*8+x]=0;
        }
        for (unsigned i=0;i<count;i++) {
            unsigned at=nz[i],x=at&7,v=at>>3;
            int32_t c=block[at];
            const int16_t *row=&e->scale[v*8];
            if (v&1) {
                todd[x]+=c;
                for (unsigned y=0;y<4;y++) od[y*8+x]+=c*row[y];
            } else {
                for (unsigned y=0;y<4;y++) ev[y*8+x]+=c*row[y];
            }
        }
        for (unsigned y=0;y<4;y++) for (unsigned k=0;k<n;k++) {
            unsigned x=used[k];
            int32_t E=ev[y*8+x],O=od[y*8+x];
            temp[y*8+x]=(E+O)>>18;
            temp[(7-y)*8+x]=(E-O-todd[x])>>18;
        }
        unsigned even[8],odd[8],ne=0,no=0;
        for (unsigned k=0;k<n;k++) {
            if (used[k]&1) odd[no++]=used[k];
            else even[ne++]=used[k];
        }
        for (unsigned y=0;y<8;y++) {
            const int32_t *t=&temp[y*8];
            uint32_t bound2=0;
            int32_t to=0;
            for (unsigned k=0;k<n;k++) bound2+=(uint32_t)abs(t[used[k]]);
            if (!FITS32((uint64_t)bound2*(smax+1))) goto wide_row;
            for (unsigned k=0;k<no;k++) to+=t[odd[k]];
            for (unsigned x=0;x<4;x++) {
                int32_t E=131072,O=0;
                for (unsigned k=0;k<ne;k++) E+=t[even[k]]*e->scale[even[k]*8+x];
                for (unsigned k=0;k<no;k++) O+=t[odd[k]]*e->scale[odd[k]*8+x];
                block[y*8+x]=clamp9((E+O)>>18);
                block[y*8+7-x]=clamp9((E-O-to)>>18);
            }
            continue;
        wide_row:
            for (unsigned x=0;x<8;x++) {
                int64_t sum=131072;
                for (unsigned k=0;k<n;k++) sum+=(int64_t)t[used[k]]*e->scale[used[k]*8+x];
                block[y*8+x]=clamp9((int)(sum>>18));
            }
        }
        return;
    }
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
    wide:;
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

/* The same IDCT for the standard (mirrored) matrix, dense over the block's used rows and columns: a column or row with
 * no coefficients adds exactly zero to every sum, so taking every column below the highest used one (and every row
 * below the highest used one) gives the same integers, in straight-line code for each width instead of loops over
 * lists. Two passes as idct's mirrored one: the same even/odd sums, the same +131072 >> 18 and clamps. Only when one
 * bound for the whole block (its coefficients' total, its first pass's largest result) proves every 32-bit sum fits;
 * else 0, the block untouched, for idct. (An Old 3DS's movies: the IDCT was 6 of a VBlank's 22 ms) */
#if defined(__ARM_FEATURE_SIMD32)
#include <arm_acle.h>
/* two values as halfwords, the second 0 past n (an absent row or column: its product 0) */
#define PK(v,i,j,n) ((uint32_t)(uint16_t)(v)[i]|((j)<(n) ? (uint32_t)(uint16_t)(v)[j]<<16 : 0u))
/* The same sums by SMLAD (two 16x16 products added at once, exactly): the values as halfwords (the coefficients always
 * fit, the first pass's results only where the block's largest does: idct_dense checks), the matrix's pairs packed by
 * psx_mdec_env_init */
static inline __attribute__((always_inline)) void dense_cols(const PsxMdecEnv *e,const int16_t *c,int32_t *temp,
                                                             unsigned w,const unsigned h,uint32_t *tmax) {
    uint32_t m=0;
    for (unsigned x=0;x<w;x++) {
        int16_t cv[8];
#pragma GCC unroll 8
        for (unsigned v=0;v<h;v++) cv[v]=c[v*8+x];
        const uint32_t e0=PK(cv,0,2,h),e1=h>4 ? PK(cv,4,6,h) : 0,o0=h>1 ? PK(cv,1,3,h) : 0,o1=h>5 ? PK(cv,5,7,h) : 0;
        int32_t todd=0;
#pragma GCC unroll 4
        for (unsigned v=1;v<h;v+=2) todd+=cv[v];
#pragma GCC unroll 4
        for (unsigned y=0;y<4;y++) {
            const uint32_t *p=e->pairs[y];
            int32_t E=__smlad(e0,p[0],131072),O=h>1 ? __smlad(o0,p[2],0) : 0;
            if (h>4) E=__smlad(e1,p[1],E);
            if (h>5) O=__smlad(o1,p[3],O);
            const int32_t a=(E+O)>>18,b=(E-O-todd)>>18;
            temp[y*8+x]=a;
            temp[(7-y)*8+x]=b;
            const uint32_t ma=(uint32_t)abs(a),mb=(uint32_t)abs(b);
            if (ma>m) m=ma;
            if (mb>m) m=mb;
        }
    }
    *tmax=m;
}
/* (sat: every result proved within -256..255, where clamp9's wrap does nothing: its clamp is SSAT's to 8 bits) */
static inline __attribute__((always_inline)) void dense_rows(const PsxMdecEnv *e,const int32_t *temp,int16_t *block,
                                                             const unsigned w,const int sat) {
    for (unsigned y=0;y<8;y++) {
        const int32_t *t=&temp[y*8];
        const uint32_t e0=PK(t,0,2,w),e1=w>4 ? PK(t,4,6,w) : 0,o0=w>1 ? PK(t,1,3,w) : 0,o1=w>5 ? PK(t,5,7,w) : 0;
        int32_t to=0;
#pragma GCC unroll 4
        for (unsigned k=1;k<w;k+=2) to+=t[k];
#pragma GCC unroll 4
        for (unsigned x=0;x<4;x++) {
            const uint32_t *p=e->pairs[x];
            int32_t E=__smlad(e0,p[0],131072),O=w>1 ? __smlad(o0,p[2],0) : 0;
            if (w>4) E=__smlad(e1,p[1],E);
            if (w>5) O=__smlad(o1,p[3],O);
            if (sat) {
                block[y*8+x]=(int16_t)__ssat((E+O)>>18,8);
                block[y*8+7-x]=(int16_t)__ssat((E-O-to)>>18,8);
            } else {
                block[y*8+x]=(int16_t)clamp9((E+O)>>18);
                block[y*8+7-x]=(int16_t)clamp9((E-O-to)>>18);
            }
        }
    }
}
#define DENSE_ENV(e) (e)
#define DENSE_T16 1 /* (the first pass's results must fit halfwords) */
#define DENSE_SAT 1
#else
#define DENSE_ENV(e) ((e)->scale)
#define DENSE_T16 0
#define DENSE_SAT 0
static inline __attribute__((always_inline)) void dense_cols(const int16_t *S,const int16_t *c,int32_t *temp,
                                                             unsigned w,const unsigned h,uint32_t *tmax) {
    uint32_t m=0;
    for (unsigned x=0;x<w;x++) {
        int32_t cv[8],todd=0;
#pragma GCC unroll 8
        for (unsigned v=0;v<h;v++) cv[v]=c[v*8+x];
#pragma GCC unroll 4
        for (unsigned v=1;v<h;v+=2) todd+=cv[v];
#pragma GCC unroll 4
        for (unsigned y=0;y<4;y++) {
            int32_t E=131072,O=0;
#pragma GCC unroll 4
            for (unsigned v=0;v<h;v+=2) E+=cv[v]*S[v*8+y];
#pragma GCC unroll 4
            for (unsigned v=1;v<h;v+=2) O+=cv[v]*S[v*8+y];
            const int32_t a=(E+O)>>18,b=(E-O-todd)>>18;
            temp[y*8+x]=a;
            temp[(7-y)*8+x]=b;
            const uint32_t ma=(uint32_t)abs(a),mb=(uint32_t)abs(b);
            if (ma>m) m=ma;
            if (mb>m) m=mb;
        }
    }
    *tmax=m;
}
static inline __attribute__((always_inline)) void dense_rows(const int16_t *S,const int32_t *temp,int16_t *block,
                                                             const unsigned w,const int sat) {
    (void)sat;
    for (unsigned y=0;y<8;y++) {
        const int32_t *t=&temp[y*8];
        int32_t to=0;
#pragma GCC unroll 4
        for (unsigned k=1;k<w;k+=2) to+=t[k];
#pragma GCC unroll 4
        for (unsigned x=0;x<4;x++) {
            int32_t E=131072,O=0;
#pragma GCC unroll 4
            for (unsigned k=0;k<w;k+=2) E+=t[k]*S[k*8+x];
#pragma GCC unroll 4
            for (unsigned k=1;k<w;k+=2) O+=t[k]*S[k*8+x];
            block[y*8+x]=(int16_t)clamp9((E+O)>>18);
            block[y*8+7-x]=(int16_t)clamp9((E-O-to)>>18);
        }
    }
}
#endif
MDEC_ITCM static int idct_dense(const PsxMdecEnv *e,int16_t block[64],unsigned w,unsigned h,uint32_t total) {
    const uint64_t smax=(uint64_t)e->scale_max+1;
    if (!FITS32(total*smax)) return 0;
    int32_t temp[64];
    uint32_t tmax; /* (the first pass's largest |result|, as dense_cols makes them) */
    switch (h) {
    case 1: dense_cols(DENSE_ENV(e),block,temp,w,1,&tmax); break;
    case 2: dense_cols(DENSE_ENV(e),block,temp,w,2,&tmax); break;
    case 3: dense_cols(DENSE_ENV(e),block,temp,w,3,&tmax); break;
    case 4: dense_cols(DENSE_ENV(e),block,temp,w,4,&tmax); break;
    case 5: dense_cols(DENSE_ENV(e),block,temp,w,5,&tmax); break;
    case 6: dense_cols(DENSE_ENV(e),block,temp,w,6,&tmax); break;
    case 7: dense_cols(DENSE_ENV(e),block,temp,w,7,&tmax); break;
    default: dense_cols(DENSE_ENV(e),block,temp,w,8,&tmax); break;
    }
    if (!FITS32((uint64_t)tmax*w*smax) || (DENSE_T16 && tmax>32767)) return 0;
    /* (each second-pass sum is 131072 plus at most w terms of |t| <= tmax times |s| <= smax - 1, less the odd ones'
     * total (at most w tmax): within 2^18 * 256 - 1 of zero, every result >> 18 is in -256..255) */
    if (DENSE_SAT && 131072+(uint64_t)tmax*w*smax<(uint64_t)256<<18) {
        switch (w) {
        case 1: dense_rows(DENSE_ENV(e),temp,block,1,1); break;
        case 2: dense_rows(DENSE_ENV(e),temp,block,2,1); break;
        case 3: dense_rows(DENSE_ENV(e),temp,block,3,1); break;
        case 4: dense_rows(DENSE_ENV(e),temp,block,4,1); break;
        case 5: dense_rows(DENSE_ENV(e),temp,block,5,1); break;
        case 6: dense_rows(DENSE_ENV(e),temp,block,6,1); break;
        case 7: dense_rows(DENSE_ENV(e),temp,block,7,1); break;
        default: dense_rows(DENSE_ENV(e),temp,block,8,1); break;
        }
        return 1;
    }
    switch (w) {
    case 1: dense_rows(DENSE_ENV(e),temp,block,1,0); break;
    case 2: dense_rows(DENSE_ENV(e),temp,block,2,0); break;
    case 3: dense_rows(DENSE_ENV(e),temp,block,3,0); break;
    case 4: dense_rows(DENSE_ENV(e),temp,block,4,0); break;
    case 5: dense_rows(DENSE_ENV(e),temp,block,5,0); break;
    case 6: dense_rows(DENSE_ENV(e),temp,block,6,0); break;
    case 7: dense_rows(DENSE_ENV(e),temp,block,7,0); break;
    default: dense_rows(DENSE_ENV(e),temp,block,8,0); break;
    }
    return 1;
}

MDEC_ITCM static int block(const PsxMdecEnv *e,const uint8_t *qt,const uint16_t *rl,size_t size,
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
    unsigned rows=0;
    uint32_t total=(uint32_t)abs(pixels[0]);
    if (pixels[0]) count++,cols|=1,rows|=1;
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
        if (value) count++,cols|=1u<<(at&7),rows|=1u<<(at>>3),total+=(uint32_t)abs(value); /* ranks only
                                                                    * increase: each position is written once */
        pixels[at]=value;
    }
    if (!count) return PSX_MDEC_OK; /* (idct's result for none: every value clamp9(131072 >> 18), 0, as it is) */
    if (e->mirrored && idct_dense(e,pixels,32-__builtin_clz(cols),32-__builtin_clz(rows),total)) return PSX_MDEC_OK;
    /* (the nonzero positions for idct, only now: the dense pass takes nearly every block. In row-major order, not the
     * codes': its sums are exact (32-bit only where a bound proves they fit), so the order doesn't change them) */
    count=0;
    for (unsigned at=0;at<64;at++) if (pixels[at]) nz[count++]=(uint8_t)at;
    idct(e,pixels,nz,count,cols);
    return PSX_MDEC_OK;
}

MDEC_ITCM int psx_mdec_macroblock(const PsxMdecEnv *e,const uint16_t *rl,size_t halfwords,
                       uint32_t command,uint8_t *out,size_t capacity,size_t *consumed) {
    /* Pixels are assembled here and copied out once: one bulk copy costs far less than 512 or 768 scattered
     * byte stores. Static rather than on the stack, which on the DS is the small DTCM. Not reentrant. */
    static uint8_t pixels[768] __attribute__((aligned(4)));
    return psx_mdec_macroblock_with(e,rl,halfwords,command,out,capacity,consumed,pixels);
}

/* The halfwords psx_mdec_macroblock would consume for its macroblock, and the result it would return, without
 * decoding: the same steps through the run-length codes (block's), for finding where each macroblock starts */
int psx_mdec_macroblock_extent(const uint16_t *rl,size_t halfwords,uint32_t command,size_t capacity,size_t *consumed) {
    *consumed=0;
    unsigned depth=(command>>27)&3;
    if ((command>>29)!=1 || depth<2) return PSX_MDEC_MODE;
    if (capacity<(depth==2 ? 768u : 512u)) return PSX_MDEC_CAPACITY;
    size_t at=0;
    for (unsigned i=0;i<6;i++) {
        unsigned first;
        do {
            if (at==halfwords) return PSX_MDEC_TRUNCATED;
            first=rl[at++];
        } while (first==0xfe00);
        unsigned rank=0;
        while (rank<63) {
            if (at==halfwords) return PSX_MDEC_TRUNCATED;
            rank+=(rl[at++]>>10)+1;
        }
    }
    *consumed=at;
    return PSX_MDEC_OK;
}

MDEC_ITCM int psx_mdec_macroblock_with(const PsxMdecEnv *e,const uint16_t *rl,size_t halfwords,
                       uint32_t command,uint8_t *out,size_t capacity,size_t *consumed,uint8_t pixels[768]) {
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
    /* Each Cr/Cb pair colours a 2x2 pixel group: compute its three offsets once, per 8x8 luma quadrant. The
     * nine-bit clamp, the signed-mode flip and (for 15-bit) the rounding to five bits are table lookups. */
    unsigned signed_mode=(command&0x04000000) ? 1 : 0;
    const uint8_t *c8=channel8[signed_mode],*c5=channel5[signed_mode];
    uint16_t stp=(command&0x02000000) ? 0x8000 : 0;
    /* (written straight into `out`: nothing fails after the blocks have decoded. The pixels went through `pixels` and a
     * copy for the DS, whose main RAM takes scattered byte stores slowly) */
    (void)pixels;
    if (depth!=2) { /* (15-bit: rgb5's tables, two pixels to a store) */
        const uint16_t *R=rgb5[signed_mode][0]+512,*G=rgb5[signed_mode][1]+512,*B=rgb5[signed_mode][2]+512;
        const uint32_t stp2=stp*0x10001u;
        for (unsigned q=0;q<4;q++) {
            const int16_t *luma=samples[2+q];
            unsigned qx=(q&1)*8,qy=(q>>1)*8;
            for (unsigned cy=0;cy<4;cy++)
#pragma GCC unroll 4
                for (unsigned cx=0;cx<4;cx++) {
                    unsigned ci=(qy/2+cy)*8+qx/2+cx;
                    const unsigned cr=(unsigned)(samples[0][ci]+128),cb=(unsigned)(samples[1][ci]+128);
                    const int dr=red_cr[cr],dg=(green_cb[cb]+green_cr[cr]+128)>>8,db=blue_cb[cb];
                    for (unsigned dy=0;dy<2;dy++) {
                        const unsigned ly=cy*2+dy,offset=(qy+ly)*16+qx+cx*2;
                        const int l0=luma[ly*8+cx*2],l1=luma[ly*8+cx*2+1];
                        const uint32_t c=stp2|R[l0+dr]|G[l0+dg]|B[l0+db]|(uint32_t)(R[l1+dr]|G[l1+dg]|B[l1+db])<<16;
                        memcpy(out+offset*2,&c,4);
                    }
                }
        }
        *consumed=at;
        return PSX_MDEC_OK;
    }
    for (unsigned q=0;q<4;q++) {
        const int16_t *luma=samples[2+q];
        unsigned qx=(q&1)*8,qy=(q>>1)*8;
        for (unsigned cy=0;cy<4;cy++)
#pragma GCC unroll 4
            for (unsigned cx=0;cx<4;cx++) {
                unsigned ci=(qy/2+cy)*8+qx/2+cx;
                const unsigned cr=(unsigned)(samples[0][ci]+128),cb=(unsigned)(samples[1][ci]+128);
                const int dr=red_cr[cr],dg=(green_cb[cb]+green_cr[cr]+128)>>8,db=blue_cb[cb];
                for (unsigned dy=0;dy<2;dy++) {
                    const unsigned ly=cy*2+dy,offset=(qy+ly)*16+qx+cx*2;
                    const int l0=luma[ly*8+cx*2],l1=luma[ly*8+cx*2+1];
                    if (depth==2) {
                        uint8_t *o=out+offset*3;
                        o[0]=c8[(unsigned)(l0+dr)&511];o[1]=c8[(unsigned)(l0+dg)&511];o[2]=c8[(unsigned)(l0+db)&511];
                        o[3]=c8[(unsigned)(l1+dr)&511];o[4]=c8[(unsigned)(l1+dg)&511];o[5]=c8[(unsigned)(l1+db)&511];
                    } else {
                        const uint16_t c0=stp|c5[(unsigned)(l0+dr)&511]|(unsigned)c5[(unsigned)(l0+dg)&511]<<5|
                                          (unsigned)c5[(unsigned)(l0+db)&511]<<10;
                        const uint16_t c1=stp|c5[(unsigned)(l1+dr)&511]|(unsigned)c5[(unsigned)(l1+dg)&511]<<5|
                                          (unsigned)c5[(unsigned)(l1+db)&511]<<10;
                        memcpy(out+offset*2,&c0,2);
                        memcpy(out+offset*2+2,&c1,2);
                    }
                }
            }
    }
    *consumed=at;
    return PSX_MDEC_OK;
}
