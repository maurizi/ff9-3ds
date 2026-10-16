#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "mdec_decode.h"
static uint8_t quant[128],scale[128],pixels[769];
static uint16_t rl[32];
int main(void) {
    memset(quant,2,sizeof quant);
    for (unsigned i=0;i<8;i++) {scale[i*2]=0x82;scale[i*2+1]=0x5a;}
    PsxMdecEnv env;psx_mdec_env_init(&env,quant,scale);
    const int dc[6]={0,0,32,64,-32,-64};
    for (unsigned i=0;i<6;i++) {rl[i*2]=0x400|((unsigned)dc[i]&1023);rl[i*2+1]=0xfe00;}
    size_t used;
    memset(pixels,0xa5,sizeof pixels);
    assert(psx_mdec_macroblock(&env,rl,12,0x30000006,pixels,768,&used)==0 && used==12);
    const unsigned brightness[4]={136,144,120,112};
    for (unsigned y=0;y<16;y++) for (unsigned x=0;x<16;x++)
        for (unsigned c=0;c<3;c++) assert(pixels[(y*16+x)*3+c]==brightness[(y/8)*2+x/8]);
    assert(pixels[768]==0xa5);
    assert(psx_mdec_macroblock(&env,rl,12,0x34000006,pixels,768,&used)==0);
    for (unsigned y=0;y<16;y++) for (unsigned x=0;x<16;x++)
        for (unsigned c=0;c<3;c++) assert(pixels[(y*16+x)*3+c]==(brightness[(y/8)*2+x/8]^128));
    for (unsigned mode=0;mode<4;mode++) {
        memset(pixels,0xa5,sizeof pixels);
        uint32_t command=0x38000006|((mode&1)<<26)|((mode>>1)<<25);
        assert(psx_mdec_macroblock(&env,rl,12,command,pixels,512,&used)==0);
        for (unsigned y=0;y<16;y++) for (unsigned x=0;x<16;x++) {
            unsigned component=brightness[(y/8)*2+x/8]^((mode&1) ? 128 : 0);
            unsigned v=(component+4)/8;
            if (v>31) v=31;
            unsigned expected=v|(v<<5)|(v<<10)|((mode&2) ? 0x8000 : 0);
            unsigned offset=(y*16+x)*2;
            assert((pixels[offset]|(pixels[offset+1]<<8))==expected);
        }
        assert(pixels[512]==0xa5);
    }
    memset(pixels,0xa5,sizeof pixels);
    assert(psx_mdec_macroblock(&env,rl,11,0x30000006,pixels,768,&used)==PSX_MDEC_TRUNCATED && !used);
    for (unsigned i=0;i<sizeof pixels;i++) assert(pixels[i]==0xa5);
    assert(psx_mdec_macroblock(&env,rl,12,0x30000006,pixels,767,&used)==PSX_MDEC_CAPACITY);
    assert(psx_mdec_macroblock(&env,rl,12,0x20000006,pixels,768,&used)==PSX_MDEC_MODE);
    /* Leading padding and implicit block end at coefficient 63. */
    for (unsigned i=0;i<6;i++) {rl[i*3]=0xfe00;rl[i*3+1]=0x400;rl[i*3+2]=0xf801;}
    assert(psx_mdec_macroblock(&env,rl,18,0x30000009,pixels,768,&used)==0 && used==18);
    /* PSX DMA goldens catch signed RGB555 rounding at byte 127 and IDCT
     * pass-order rounding for an off-diagonal chroma impulse. */
    for (unsigned i=0;i<6;i++) {rl[i*2]=(62<<10)|512;rl[i*2+1]=0xfe00;}
    assert(psx_mdec_macroblock(&env,rl,12,0x3c000006,pixels,512,&used)==0);
    assert(pixels[0]==0x30 && pixels[1]==0x40);
    const int16_t row1[8]={32138,27245,18204,6392,-6393,-18205,-27246,-32139};
    for (unsigned i=0;i<8;i++) {
        unsigned value=(uint16_t)row1[i];scale[16+i*2]=value;scale[17+i*2]=value>>8;
    }
    psx_mdec_env_init(&env,quant,scale);
    const uint16_t impulse[13]={0,3103,0xfe00,17,0xfe00,34,0xfe00,51,0xfe00,68,0xfe00,85,0xfe00};
    assert(psx_mdec_macroblock(&env,impulse,13,0x30000007,pixels,768,&used)==0);
    assert(pixels[(2*16+6)*3]==140 && pixels[(2*16+6)*3+1]==132);
    assert(pixels[(6*16+2)*3]==139 && pixels[(6*16+2)*3+1]==133);
    puts("MDEC macroblocks: DC quadrants, signed/STP modes, padding and bounds pass");
    return 0;
}
