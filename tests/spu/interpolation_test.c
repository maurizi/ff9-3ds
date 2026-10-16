#include <assert.h>
#include <stdio.h>
#include "psx_spu_source.h"
#include "interpolation_cases.h"
#include "interpolation_reference.h"

int main(void) {
    for (unsigned phase = 0; phase < 256; ++phase) {
        for (unsigned position = 0; position < 28; ++position) {
            uint32_t hash = 2166136261u;
            for (unsigned scenario = 0; scenario < 32; ++scenario) {
                PsxSpuSource source = {0};
                for (unsigned i = 0; i < 31; ++i) source.samples[i] = -12345;
                interpolation_case(scenario, source.samples + position);
                source.counter = (position << 12) | (phase << 4);
                int16_t sample = psx_spu_source_interpolate(&source);
                hash = interpolation_hash(hash, sample);
                for (unsigned fraction = 1; fraction < 16; ++fraction) {
                    source.counter = (source.counter & ~15u) | fraction;
                    assert(psx_spu_source_interpolate(&source) == sample);
                }
            }
            if (hash != interpolation_reference_hashes[phase]) {
                printf("Interpolation phase %02x position %u mismatch\n", phase, position);
                return 1;
            }
        }
    }
    puts("SPU Gaussian: all phases, positions, low pitch bits PASS");
    return 0;
}
