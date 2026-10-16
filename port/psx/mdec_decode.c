/* SPDX-License-Identifier: GPL-2.0-or-later
 * Integer MDEC reconstruction. Dequantization precision, signed-nine-bit
 * clamps and colour rounding are adapted from Mednafen's PSX MDEC research
 * (Copyright (C) 2011-2016 Mednafen Team). See third_party/mednafen/README.md.
 * Matrix passes retain the caller's full signed scale values. */
#include "mdec_decode.h"
#include <string.h>

/* Zigzag rank -> row-major frequency coordinate. */
static const uint8_t zigzag[64]={
    0,1,8,16,9,2,3,10,17,24,32,25,18,11,4,5,
    12,19,26,33,40,48,41,34,27,20,13,6,7,14,21,28,
    35,42,49,56,57,50,43,36,29,22,15,23,30,37,44,51,
    58,59,52,45,38,31,39,46,53,60,61,54,47,55,62,63
};
static int clamp(int x,int lo,int hi) {return x<lo ? lo : x>hi ? hi : x;}
static int signed10(unsigned x) {return (int)(x&1023)-((x&512) ? 1024 : 0);}
static int floor_shift(int64_t x,unsigned shift) {
    return x>=0 ? (int)(x>>shift) : -(int)((-x+(((int64_t)1<<shift)-1))>>shift);
}
static int clamp9(int x) {
    int wrapped=(int)((unsigned)x&511);
    if (wrapped>=256) wrapped-=512;
    return clamp(wrapped,-128,127);
}

void psx_mdec_env_init(PsxMdecEnv *e,const uint8_t quant[128],const uint8_t scale[128]) {
    memcpy(e->quant,quant,128);
    for (unsigned i=0;i<64;i++) {
        unsigned value=scale[i*2]|((unsigned)scale[i*2+1]<<8);
        e->scale[i]=(int)(value&32767)-((value&32768) ? 32768 : 0);
    }
}

static void idct(const PsxMdecEnv *e,int16_t block[64]) {
    int16_t temp[64];
    for (unsigned y=0;y<8;y++) for (unsigned x=0;x<8;x++) {
        int64_t sum=0;
        /* Vertical pass first: integer rounding makes the pass order visible
         * even though the corresponding real-valued transforms commute. */
        for (unsigned v=0;v<8;v++) sum+=(int32_t)block[v*8+x]*e->scale[v*8+y];
        temp[y*8+x]=floor_shift(sum+131072,18);
    }
    for (unsigned y=0;y<8;y++) for (unsigned x=0;x<8;x++) {
        int64_t sum=0;
        for (unsigned u=0;u<8;u++) sum+=(int32_t)temp[y*8+u]*e->scale[u*8+x];
        block[y*8+x]=clamp9(floor_shift(sum+131072,18));
    }
}

static int block(const PsxMdecEnv *e,const uint8_t *qt,const uint16_t *rl,size_t size,
                 size_t *position,int16_t pixels[64]) {
    memset(pixels,0,64*sizeof *pixels);
    unsigned first;
    do {
        if (*position==size) return PSX_MDEC_TRUNCATED;
        first=rl[(*position)++];
    } while (first==0xfe00);
    unsigned q=first>>10,rank=0;
    int level=signed10(first);
    int dc=q ? level*qt[0]*16+(level<0 ? 8 : level>0 ? -8 : 0) : level*32;
    pixels[0]=clamp(dc,-16384,16383);
    while (rank<63) {
        if (*position==size) return PSX_MDEC_TRUNCATED;
        unsigned code=rl[(*position)++];
        rank+=(code>>10)+1;
        if (rank>63) break;
        level=signed10(code);
        unsigned quant=q*qt[rank];
        int coefficient=quant ? floor_shift(level*(int)quant,3)*16+
                               (level<0 ? 8 : level>0 ? -8 : 0) : level*32;
        pixels[zigzag[rank]]=clamp(coefficient,-16384,16383);
    }
    idct(e,pixels);
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
    for (unsigned y=0;y<16;y++) for (unsigned x=0;x<16;x++) {
        unsigned chroma=(y/2)*8+x/2, luma=(y&7)*8+(x&7);
        int cr=samples[0][chroma], cb=samples[1][chroma];
        int lum=samples[2+(y/8)*2+x/8][luma];
        int rgb[3];
        rgb[0]=clamp9(lum+floor_shift(359*cr+128,8))+128;
        int green=floor_shift(-88*cb,5)*32+floor_shift(-183*cr,3)*8;
        rgb[1]=clamp9(lum+floor_shift(green+128,8))+128;
        rgb[2]=clamp9(lum+floor_shift(454*cb+128,8))+128;
        unsigned offset=y*16+x;
        if (depth==2) {
            unsigned flip=(command&0x04000000) ? 128 : 0;
            for (unsigned c=0;c<3;c++) pixels[offset*3+c]=(unsigned)rgb[c]^flip;
        } else {
            unsigned color=(command&0x02000000) ? 0x8000 : 0;
            /* Signed mode selects the byte representation before reducing
             * channels to five bits, including the rounding boundary at 127. */
            for (unsigned c=0;c<3;c++) {
                unsigned value=(unsigned)rgb[c]^((command&0x04000000) ? 128 : 0);
                color|=(unsigned)clamp((value+4)/8,0,31)<<(c*5);
            }
            pixels[offset*2]=color;pixels[offset*2+1]=color>>8;
        }
    }
    *consumed=at;
    return PSX_MDEC_OK;
}
