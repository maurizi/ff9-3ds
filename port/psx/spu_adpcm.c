/* PSX SPU ADPCM arithmetic. See docs/audio-validation.md for reference scope. */
#include "psx_spu_adpcm.h"

uint8_t psx_spu_decode_block(const uint8_t block[PSX_SPU_BLOCK_BYTES],
                           PsxSpuAdpcmHistory *history,
                           int16_t output[PSX_SPU_BLOCK_SAMPLES]) {
    static const int8_t coefficients[5][2] = {
        {0, 0}, {60, 0}, {115, -52}, {98, -55}, {122, -60}
    };
    unsigned filter = block[0] >> 4;
    unsigned shift = block[0] & 15;
    int previous = history->previous, older = history->older;
    int c1 = filter < 5 ? coefficients[filter][0] : 0;
    int c2 = filter < 5 ? coefficients[filter][1] : 0;
    /* Reserved shifts follow psx-spx/DuckStation; hardware confirmation pending. */
    if (shift > 12) shift = 9;
    for (unsigned i = 0; i < PSX_SPU_BLOCK_SAMPLES; ++i) {
        int nibble = (block[2 + i / 2] >> ((i & 1) * 4)) & 15;
        if (nibble & 8) nibble -= 16;
        /* Multiplication avoids left-shifting a negative signed value. The
         * supported ARM/x86 targets implement arithmetic signed right shift.
         * Round BOTH predictor terms down before summing: no XA-style +32. */
        int sample = (nibble * 4096) >> shift;
        sample += (previous * c1) >> 6;
        sample += (older * c2) >> 6;
        if (sample > 32767) sample = 32767;
        if (sample < -32768) sample = -32768;
        output[i] = (int16_t)sample;
        older = previous;
        previous = sample;
    }
    history->previous = (int16_t)previous;
    history->older = (int16_t)older;
    return block[1];
}
