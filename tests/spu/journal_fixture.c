/* Synthetic bus events for the production trace writer; no disc assets. */
#include <assert.h>
#include <stdio.h>
#undef NULL
#include "psyq_port.h"
#include "psx_spu.h"
extern u8 *psx_spu_ram;
u32 func_8005709C(u8 *, u32);
int plat_spu_trace_open(const char *path);
unsigned short psx_spu_regs[0x200];
static u8 ram[512*1024];
static u32 hash=2166136261u;
static unsigned frames,nonzero;
void plat_audio_samples(const short *pcm,unsigned count) {
    frames+=count;
    for(unsigned i=0;i<count*2;++i){hash=(hash^(u16)pcm[i])*16777619u;nonzero+=pcm[i]!=0;}
}
int main(int argc,char **argv) {
    if(argc!=2||!plat_spu_trace_open(argv[1]))return 1;
    psx_spu_ram=ram;SpuInit();
    u8 block[16]={0,7};for(unsigned i=2;i<16;++i)block[i]=0x32+i;
    SpuSetTransferStartAddr(0x1000);assert(func_8005709C(block,sizeof block)==sizeof block);
    assert(SpuSetReverbModeType(0x104)==0);SpuSetReverbModeDepth(0x2800,0x2800);SpuSetReverb(1);
    psx_spu_write(0x180,0x3fff);psx_spu_write(0x182,0x3fff);
    psx_spu_voice_register(0,0,0x2fff);psx_spu_voice_register(0,2,0x2000);
    psx_spu_voice_register(0,4,0x1000);psx_spu_voice_register(0,6,0x200);
    psx_spu_voice_register(0,8,15);psx_spu_voice_register(0,10,0x1fc0);
    psx_spu_write(0x198,1);psx_spu_write(0x188,1);psx_spu_advance(96*5000);
    /* Equal-value writes remain distinct events; mutate a playing RAM block. */
    psx_spu_voice_register(0,2,0x2000);psx_spu_voice_register(0,2,0x2000);
    block[4]=0x8f;assert(func_8005709C(block,sizeof block)==sizeof block);
    psx_spu_advance(96*1000);psx_spu_write(0x18c,1);psx_spu_advance(96*1000);
    assert(frames==7000&&nonzero);
    printf("%u %u %08x\n",frames,nonzero,hash);return 0;
}
