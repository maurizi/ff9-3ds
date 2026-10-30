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

/* As psx_spu_decode_block, into `output` (4-byte aligned, 28 samples) negated if `negate`, with the samples in pairs
 * and copied out in bursts: for the ARM7, whose stores to main RAM (the hardware channels' rings) are slow one by
 * one. */
SPU_HW_ONLY_HOT uint8_t psx_spu_decode_block_to(const uint8_t block[PSX_SPU_BLOCK_BYTES], PsxSpuAdpcmHistory *history,
                                           int16_t *output, int negate) {
    static const int8_t coefficients[5][2] = {
        {0, 0}, {60, 0}, {115, -52}, {98, -55}, {122, -60}
    };
    unsigned filter = block[0] >> 4;
    unsigned shift = block[0] & 15;
    int previous = history->previous, older = history->older;
    const int c1 = filter < 5 ? coefficients[filter][0] : 0;
    const int c2 = filter < 5 ? coefficients[filter][1] : 0;
    if (shift > 12) shift = 9;
    typedef uint32_t __attribute__((may_alias)) word;
    const word *w = (const word *)__builtin_assume_aligned(block, 4);
    word *out = (word *)__builtin_assume_aligned(output, 4);
    if (!c1 && !c2 && !((w[0] >> 16) | w[1] | w[2] | w[3])) {
        for (unsigned i = 0; i < PSX_SPU_BLOCK_SAMPLES / 2; ++i) out[i] = 0;
        history->previous = history->older = 0;
        return block[1];
    }
    const unsigned down = 16 + shift;
    const int sign = negate ? -1 : 1;
    uint32_t pairs[PSX_SPU_BLOCK_SAMPLES / 2];
    unsigned at = 0;
    for (unsigned k = 0; k < 4; ++k) {
        uint32_t bits = k ? w[k] : w[0] >> 16;
        for (unsigned j = k ? 4 : 2; j; --j, bits >>= 8) {
            int a = (int32_t)(bits << 28) >> down;
            a += (previous * c1) >> 6;
            a += (older * c2) >> 6;
            if ((unsigned)(a + 32768) > 65535u) a = (a >> 31) ^ 32767;
            int b = (int32_t)((bits << 24) & 0xF0000000u) >> down;
            b += (a * c1) >> 6;
            b += (previous * c2) >> 6;
            if ((unsigned)(b + 32768) > 65535u) b = (b >> 31) ^ 32767;
            older = a;
            previous = b;
            pairs[at++] = (uint16_t)(a * sign) | (uint32_t)(b * sign) << 16;
        }
    }
    history->previous = (int16_t)previous;
    history->older = (int16_t)older;
#if defined(__arm__) && !defined(__thumb__)
    const uint32_t *from = pairs;
    __asm__ volatile("ldmia %1!, {r2-r8}\n\tstmia %0!, {r2-r8}\n\tldmia %1!, {r2-r8}\n\tstmia %0!, {r2-r8}"
                     : "+r"(out), "+r"(from) : : "r2", "r3", "r4", "r5", "r6", "r7", "r8", "memory");
#else
    for (unsigned i = 0; i < PSX_SPU_BLOCK_SAMPLES / 2; ++i) out[i] = pairs[i];
#endif
    return block[1];
}
