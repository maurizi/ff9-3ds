#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "psx_spu.h"

unsigned short psx_spu_regs[0x200];
static unsigned char ram[512 * 1024];
unsigned char *psx_spu_ram = ram;
static unsigned sample_count;
static short last[2];
void plat_audio_samples(const short *samples, unsigned count) {
    assert(count > 0 && count <= 256);
    sample_count += count;
    last[0] = samples[count * 2 - 2]; last[1] = samples[count * 2 - 1];
}

int main(void) {
    psx_spu_set_rendering(1);
    psx_spu_reset();
    assert(psx_spu_read(0x1aa) == 0xc000);
    psx_spu_advance(95);
    assert(sample_count == 0);
    psx_spu_advance(1);
    assert(sample_count == 1 && last[0] == 0 && last[1] == 0);
    psx_spu_advance(96 * 1000 + 95);
    assert(sample_count == 1001);
    psx_spu_advance(1);
    assert(sample_count == 1002);
    assert((psx_spu_read(0x1ae) & 0x800) == 0x800);

    /* 24 loud voices require clipping after summation, before main volume.
     * Each voice loops one constant block; after startup set held sustain to
     * full scale and check positive/negative stereo gains independently. */
    psx_spu_reset();
    ram[0x1000] = 0;
    ram[0x1001] = 7;
    memset(ram + 0x1002, 0x77, 14);
    for (unsigned v = 0; v < 24; ++v) {
        psx_spu_voice_register(v, 0, 0x3fff);
        psx_spu_voice_register(v, 2, 0x4000);
        psx_spu_voice_register(v, 4, 0x1000);
        psx_spu_voice_register(v, 6, 0x200);
        psx_spu_voice_register(v, 8, 15);
        psx_spu_voice_register(v, 10, 0x1fc0);
    }
    psx_spu_write(0x180, 0x2000);
    psx_spu_write(0x182, 0x2000);
    psx_spu_write(0x188, 0xffff);
    psx_spu_write(0x18a, 0xffff); /* only the low 8 high-half bits are voices */
    psx_spu_advance(96);
    assert(psx_spu_read(12) == 0 && psx_spu_read(23 * 16 + 12) == 0);
    assert(last[0] == 0 && last[1] == 0);
    psx_spu_advance(96 * 10);
    for (unsigned v = 0; v < 24; ++v) psx_spu_voice_register(v, 12, 0x7fff);
    psx_spu_advance(96 * 50);
    assert(last[0] == 16383 && last[1] == -16384);
    assert(psx_spu_read(0x19c) == 0xffff && psx_spu_read(0x19e) == 255);
    assert(psx_spu_regs[6] == 0x7fff && psx_spu_regs[7] == 0x200);
    assert(psx_spu_read(0x200) == 0x7ffe && psx_spu_read(0x202) == 0x8000);

    /* Repeated KON is a retrigger, including simultaneous KOF. The current
     * output frame is generated before the event takes effect. */
    psx_spu_write(0x188, 1);
    psx_spu_write(0x18c, 1);
    psx_spu_advance(96);
    assert(last[0] == 16383 && psx_spu_read(12) == 0);
    assert(!(psx_spu_read(0x19c) & 1));
    psx_spu_advance(96);
    assert(psx_spu_read(12) == 14336);
    psx_spu_write(0x188, 0);
    psx_spu_write(0x1aa, 0x8000); /* mute output while voices still advance */
    psx_spu_advance(96 * 4);
    assert(last[0] == 0 && last[1] == 0 && psx_spu_read(12) != 0);
    psx_spu_write(0x1aa, 0); /* falling enable immediately forces active envelopes off */
    assert(psx_spu_read(12) == 0);

    /* Pending halfword writes replace their half; writing zero cancels it. */
    psx_spu_reset();
    psx_spu_write(0x188, 1);
    psx_spu_write(0x188, 0);
    psx_spu_advance(96 * 3);
    assert(psx_spu_read(12) == 0);
    psx_spu_voice_register(24, 0, 0xffff);
    psx_spu_write(0x401, 0xffff);
    assert(psx_spu_read(0x401) == 0xffff);
    puts("SPU integration: sample clock, keys, clipping, registers PASS");
    return 0;
}
