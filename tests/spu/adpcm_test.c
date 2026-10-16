#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "psx_spu_adpcm.h"
#include "adpcm_cases.h"
#include "adpcm_reference.h"

int main(void) {
    uint8_t block[16] = {0};
    int16_t output[28];
    PsxSpuAdpcmHistory history = {0};
    /* Low nibble first, signed expansion, and exact 16-bit extrema. */
    for (unsigned i = 0; i < 14; ++i)
        block[i + 2] = ((2 * i + 1) & 15) * 16 + ((2 * i) & 15);
    psx_spu_decode_block(block, &history, output);
    for (unsigned i = 0; i < 28; ++i) {
        int nibble = i & 15;
        if (nibble >= 8) nibble -= 16;
        assert(output[i] == nibble * 4096);
    }
    /* Separate predictor rounding differs from a combined prediction / 64. */
    memset(block, 0, sizeof block);
    block[0] = 0x2c;
    history = (PsxSpuAdpcmHistory){1, -1};
    psx_spu_decode_block(block, &history, output);
    assert(output[0] == 1);
    /* Saturation must happen before predictor history is saved. */
    block[0] = 0x40;
    history = (PsxSpuAdpcmHistory){32767, -32768};
    psx_spu_decode_block(block, &history, output);
    assert(output[0] == 32767 && output[1] == 31742);

    for (unsigned header = 0; header < 256; ++header) {
        uint32_t hash = 2166136261u;
        for (unsigned scenario = 0; scenario < 5; ++scenario) {
            history = (PsxSpuAdpcmHistory){adpcm_initial_history[scenario][0],
                                         adpcm_initial_history[scenario][1]};
            uint32_t random = adpcm_seed(header, scenario);
            for (unsigned b = 0; b < 8; ++b) {
                adpcm_case_block(block, header, &random);
                uint8_t flags = psx_spu_decode_block(block, &history, output);
                for (unsigned i = 0; i < 28; ++i)
                    hash = adpcm_hash(hash, (uint16_t)output[i]);
                hash = adpcm_hash(hash, (uint16_t)history.previous);
                hash = adpcm_hash(hash, (uint16_t)history.older);
                hash = adpcm_hash(hash, flags);
                assert(flags == block[1]);
                assert(history.previous == output[27] && history.older == output[26]);
            }
        }
        if (hash != adpcm_reference_hashes[header]) {
            printf("ADPCM header %02x: %08lx != %08lx\n", header,
                   (unsigned long)hash, (unsigned long)adpcm_reference_hashes[header]);
            return 1;
        }
    }
    printf("SPU ADPCM: 10240 blocks / 286720 samples PASS\n");
    return 0;
}
