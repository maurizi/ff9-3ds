#include <assert.h>
#include <stdio.h>
#include "psx_spu_reverb.h"
#include "reverb_cases.h"
#include "reverb_reference.h"

static uint8_t ram[512 * 1024];
int main(void) {
    for (unsigned scenario = 0; scenario < REVERB_CASES; ++scenario) {
        PsxSpuReverb reverb;
        psx_spu_reverb_reset(&reverb);
        reverb_case_init(scenario, reverb.registers, ram);
        psx_spu_reverb_base(&reverb, reverb_case_base(scenario));
        uint32_t hash = 2166136261u;
        for (unsigned frame = 0; frame < REVERB_FRAMES; ++frame) {
            uint16_t base;
            if (reverb_case_base_event(scenario, frame, &base)) psx_spu_reverb_base(&reverb, base);
            int16_t input[2], output[2];
            reverb_case_input(scenario, frame, input);
            psx_spu_reverb_tick(&reverb, ram, input[0], input[1],
                                 reverb_case_enable(scenario, frame), output);
            hash = reverb_case_hash(hash, (uint16_t)output[0]);
            hash = reverb_case_hash(hash, (uint16_t)output[1]);
            hash = reverb_case_hash(hash, reverb.cursor);
            assert(reverb.cursor < 0x40000 && reverb.phase == ((frame + 1) & 63));
        }
        for (unsigned i = 0; i < sizeof ram; ++i) hash = reverb_case_hash(hash, ram[i]);
        if (hash != reverb_reference_hashes[scenario]) {
            printf("Reverb scenario %u: %08lx != %08lx\n", scenario,
                   (unsigned long)hash, (unsigned long)reverb_reference_hashes[scenario]);
            return 1;
        }
    }
    puts("SPU reverb: 32 RAM/FIR/stereo/overflow scenarios PASS");
    return 0;
}
