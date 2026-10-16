#include <stdint.h>
#include <stdio.h>
#include "psx_spu.h"
#include "noise_reference.h"
int main(void) {
    for (unsigned clock = 0; clock < 64; ++clock) {
        PsxSpuNoise noise = {0, 1};
        uint32_t hash = 2166136261u;
        for (unsigned i = 0; i < 20000; ++i) {
            psx_spu_noise_tick(&noise, clock);
            hash = (hash ^ noise.counter) * 16777619u;
            hash = (hash ^ noise.level) * 16777619u;
        }
        if (hash != noise_reference_hashes[clock]) {
            printf("Noise clock %u mismatch\n", clock);
            return 1;
        }
    }
    puts("SPU noise: all 64 clock settings PASS");
    return 0;
}
