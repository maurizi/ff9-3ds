#include "psx_spu.h"

void psx_spu_noise_tick(PsxSpuNoise *noise, unsigned clock) {
    static const uint8_t fraction[4] = {0, 84, 140, 180};
    clock &= 63;
    unsigned add = fraction[clock & 3];
    uint32_t period = 0x80000000u >> (clock >> 2);
    noise->counter += 0x10000 + add;
    if ((noise->counter & 0xffff) >= 210) noise->counter += 0x10000 - add;
    if (noise->counter >= period) {
        noise->counter &= period - 1;
        unsigned feedback = 1 ^ (noise->level >> 10) ^ (noise->level >> 11)
                              ^ (noise->level >> 12) ^ (noise->level >> 15);
        noise->level = (noise->level << 1) | (feedback & 1);
    }
}
