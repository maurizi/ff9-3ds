#include <assert.h>
#include <stdio.h>
#include "gp0.h"
#include "psx_spu.h"

unsigned short psx_spu_regs[0x200];
static unsigned char ram[512 * 1024];
unsigned char *psx_spu_ram = ram;
const Gp0Backend *gp0_backend;
static unsigned samples, events, frames, event_samples[4];
static int stop_in_callback;
void plat_audio_samples(const short *pcm, unsigned count) { (void)pcm; samples += count; }
void plat_wait_frame(void) { ++frames; }
void psx_cd_pump(void) {}
void psx_cd_frame(void) {}
void psx_drawsync_pending(void) {}
void psx_pad_update(void) {}

long SetRCnt(unsigned long, unsigned short, long);
long StartRCnt(unsigned long);
long StopRCnt(unsigned long);
long OpenEvent(unsigned long, long, long, long (*)(void));
long EnableEvent(unsigned long);
void psx_frame(void);

static long tick(void) {
    if (events < 4) event_samples[events] = samples;
    ++events;
    if (stop_in_callback) StopRCnt(0xf2000002ul);
    return 0;
}
int main(void) {
    psx_spu_set_rendering(1);
    psx_spu_reset();
    SetRCnt(0xf2000002ul, 17640, 0x1000);
    StartRCnt(0xf2000002ul);
    long event = OpenEvent(0xf2000002ul, 2, 0x1000, tick);
    assert(event != -1 && EnableEvent(event));
    psx_frame();
    assert(frames == 1 && samples == 735 && events == 4);
    assert(event_samples[0] == 183 && event_samples[1] == 367);
    assert(event_samples[2] == 551 && event_samples[3] == 735);
    for (unsigned i = 1; i < 60; ++i) psx_frame();
    assert(frames == 60 && samples == 44100 && events == 240);
    StopRCnt(0xf2000002ul);
    psx_frame();
    assert(samples == 44835 && events == 240);
    StartRCnt(0xf2000002ul);
    stop_in_callback = 1;
    psx_frame();
    assert(samples == 45570 && events == 241);
    psx_frame();
    assert(samples == 46305 && events == 241);
    SetRCnt(0xf2000002ul, 0, 0x1000);
    StartRCnt(0xf2000002ul);
    psx_frame();
    assert(samples == 47040 && events == 241);
    puts("SPU/AKAO clock: interleaved ticks and independent sample clock PASS");
    return 0;
}
