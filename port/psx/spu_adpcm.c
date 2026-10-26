/* PSX SPU ADPCM arithmetic. See docs/audio-validation.md for reference scope. */
#include "psx_spu_adpcm.h"
#include "spu_hot.h"

SPU_HW_HOT uint8_t psx_spu_decode_block(const uint8_t block[PSX_SPU_BLOCK_BYTES],
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
    if (!c1 && !c2) {
        /* No prediction and all-zero nibbles decode to silence whatever the shift: FF9 parks idle voices on
         * the zeroed CD input buffer, which they keep fetching. Blocks start 16-byte aligned. */
        typedef uint32_t __attribute__((may_alias)) word;
        const word *w = (const word *)__builtin_assume_aligned(block, 4);
        if (!((w[0] >> 16) | w[1] | w[2] | w[3])) {
            for (unsigned i = 0; i < PSX_SPU_BLOCK_SAMPLES; ++i) output[i] = 0;
            history->previous = history->older = 0;
            return block[1];
        }
    }
    /* The nibbles a word at a time (blocks start 16-byte aligned): each one moved to the top bits and shifted down
     * arithmetically, (n << 28) >> (16 + shift), which is n * 4096 >> shift exactly for the shifts up to 12.
     * Round BOTH predictor terms down before summing: no XA-style +32. (The supported ARM/x86 targets implement
     * arithmetic signed right shift.) */
    typedef uint32_t __attribute__((may_alias)) word;
    const word *w = (const word *)__builtin_assume_aligned(block, 4);
    const unsigned down = 16 + shift;
    int16_t *out = output;
    for (unsigned k = 0; k < 4; ++k) {
        uint32_t bits = k ? w[k] : w[0] >> 16;
        for (unsigned j = k ? 8 : 4; j; --j, bits >>= 4) {
            int sample = (int32_t)(bits << 28) >> down;
            sample += (previous * c1) >> 6;
            sample += (older * c2) >> 6;
            if ((unsigned)(sample + 32768) > 65535u) sample = (sample >> 31) ^ 32767;
            *out++ = (int16_t)sample;
            older = previous;
            previous = sample;
        }
    }
    history->previous = (int16_t)previous;
    history->older = (int16_t)older;
    return block[1];
}
