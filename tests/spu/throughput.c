/* Disc-free complete-SPU throughput; the PCM sink only hashes, never waits. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "psx_spu.h"
#include "psx_spu_presets.h"
#ifdef ARM9
#include <nds.h>
#else
#include <time.h>
#endif
unsigned short psx_spu_regs[0x200];
static unsigned char ram[512*1024];
unsigned char *psx_spu_ram=ram;
static uint32_t hash,frames;
volatile uint32_t spu_throughput_stats[4][4];
void plat_audio_samples(const short *pcm,unsigned count) {
    frames+=count;
    for(unsigned i=0;i<count*2;++i)hash=(hash^(uint16_t)pcm[i])*16777619u;
}
int main(void) {
#ifdef ARM9
    consoleDemoInit();if(isDSiMode())setCpuClock(true);
#endif
    puts("SPU throughput; 4410 frames/run");
    for(unsigned scenario=0;scenario<4;++scenario) {
        unsigned voices=scenario<2?4:24,wet=scenario&1;
        memset(ram,0,sizeof ram);psx_spu_set_rendering(1);psx_spu_reset();
        ram[0x1001]=7;for(unsigned i=2;i<16;++i)ram[0x1000+i]=i*13;
        for(unsigned v=0;v<voices;++v) {
            psx_spu_voice_register(v,0,0x1800+v*23);psx_spu_voice_register(v,2,0x1700-v*17);
            psx_spu_voice_register(v,4,0xe00+v*51);psx_spu_voice_register(v,6,0x200);
            psx_spu_voice_register(v,8,15);psx_spu_voice_register(v,10,0x1fc0);
        }
        for(unsigned i=0;i<32;++i)psx_spu_write(0x1c0+i*2,psx_spu_presets[4].registers[i]);
        psx_spu_write(0x1a2,(0x80000-psx_spu_presets[4].bytes)/8);
        psx_spu_write(0x184,0x2800);psx_spu_write(0x186,0x2800);
        psx_spu_write(0x180,0x3fff);psx_spu_write(0x182,0x3fff);
        unsigned mask=(1u<<voices)-1;
        psx_spu_write(0x198,mask);psx_spu_write(0x19a,mask>>16);
        psx_spu_write(0x188,mask);psx_spu_write(0x18a,mask>>16);
        psx_spu_write(0x1aa,wet?0xc080:0xc000);
        frames=0;hash=2166136261u;
#ifdef ARM9
        cpuStartTiming(0);
#else
        clock_t start=clock();
#endif
        psx_spu_advance(96*4410);
#ifdef ARM9
        unsigned ticks=cpuEndTiming(),hz=BUS_CLOCK;
#else
        unsigned ticks=clock()-start,hz=CLOCKS_PER_SEC;
#endif
        assert(frames==4410);
        spu_throughput_stats[scenario][0]=voices;spu_throughput_stats[scenario][1]=wet;
        spu_throughput_stats[scenario][2]=ticks;spu_throughput_stats[scenario][3]=hash;
        unsigned us=(uint64_t)ticks*1000000/(hz*6u);
        printf("%u voices wet%u: %u us/frame\n%08lx\n",voices,wet,us,(unsigned long)hash);
    }
#ifdef ARM9
    puts("Throughput complete");while(1)swiWaitForVBlank();
#else
    return 0;
#endif
}
