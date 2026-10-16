#include <nds.h>
#include <assert.h>
#include <stdio.h>
#include "audio.h"
#include "platform.h"
extern int pcm_ring_tests(void);
static int16_t samples[735*2];
static uint32_t emitted;
volatile uint32_t port_audio_test_stats[8];
void psx_spu_set_rendering(int enabled) { (void)enabled; } /* sink-only fixture */
static void feed(void) {
    for(unsigned i=0;i<735;++i) {
        unsigned l=emitted%100,r=emitted%150;
        samples[i*2]=(int16_t)((l<50?l:100-l)*480-12000);
        samples[i*2+1]=(int16_t)((r<75?r:150-r)*320-12000);
        ++emitted;
    }
    plat_audio_samples(samples,735);
}
int main(void) {
    defaultExceptionHandler();consoleDemoInit();if(isDSiMode())setCpuClock(true);
    puts("DS stereo PCM stream\n");pcm_ring_tests();
    assert(port_audio_init());
    uint32_t stall_silence=0;
    for(unsigned frame=0;frame<240;++frame) {
        feed();
        if(frame==90) {
            unsigned before=port_audio_stats[4];
            for(unsigned j=0;j<30;++j)swiWaitForVBlank();
            stall_silence=port_audio_stats[4]-before;
        }
        if(frame==150)for(unsigned j=0;j<20;++j)feed(); /* force backpressure */
        swiWaitForVBlank();
    }
    while(port_audio_queued())swiWaitForVBlank();
    for(unsigned i=0;i<6;++i)swiWaitForVBlank();
    port_audio_test_stats[1]=emitted;port_audio_test_stats[2]=port_audio_stats[3];
    port_audio_test_stats[3]=stall_silence;port_audio_test_stats[4]=port_audio_stats[7];
    port_audio_test_stats[5]=TIMER_CR(0);port_audio_test_stats[6]=port_audio_stats[9];
    assert(port_audio_stats[2]==emitted&&port_audio_stats[3]==emitted);
    assert(stall_silence&&port_audio_stats[7]&&port_audio_stats[9]==0);
    port_audio_stop();
    port_audio_test_stats[0]=0x50415353;
    printf("frames %lu / %lu\n",(unsigned long)emitted,(unsigned long)port_audio_stats[3]);
    printf("stall zeros %lu\n",(unsigned long)stall_silence);
    printf("waits %lu format errors %lu\n",(unsigned long)port_audio_stats[7],(unsigned long)port_audio_stats[9]);
    puts("PCM stream checks PASS");while(1)swiWaitForVBlank();
}
