/* PSX reverb arithmetic and 39-tap resampling. See audio-validation.md for
 * reference scope, channel scheduling, and unresolved hardware verification. */
#include "psx_spu_reverb.h"
#include "spu_hot.h"
#include <string.h>

enum {
    DELAY1, DELAY2, IIR, COMB1, COMB2, COMB3, COMB4, WALL,
    APF1, APF2, SAME_L, SAME_R, COMB1_L, COMB1_R, COMB2_L, COMB2_R,
    DSAME_L, DSAME_R, DIFF_L, DIFF_R, COMB3_L, COMB3_R, COMB4_L, COMB4_R,
    DDIFF_L, DDIFF_R, APF1_L, APF1_R, APF2_L, APF2_R, INPUT_L, INPUT_R
};
/* Nonzero even taps of the documented FIR; its center odd tap is 16384. */
static const int16_t fir[20] = {
    -1, 2, -10, 35, -103, 266, -616, 1332, -2960, 10246,
    10246, -2960, 1332, -616, 266, -103, 35, -10, 2, -1
};

static SPU_HOT int16_t clip(int32_t value) {
    return value > 32767 ? 32767 : value < -32768 ? -32768 : value;
}

static SPU_HOT unsigned address(const PsxSpuReverb *reverb, uint16_t reg, int delta) {
    uint32_t sum = reverb->cursor + (((uint32_t)reg * 4 + delta) & 0x3ffff);
    /* The 18-bit hardware address adder adds ESA on carry; this isn't generic
     * modulo by the work-area size. Preserve that behavior for unusual offsets. */
    if (sum & 0x40000) sum += reverb->base;
    return (sum & 0x3ffff) * 2;
}

/* Addresses are even byte offsets into the little-endian RAM image: halfword accesses. */
static SPU_HOT int16_t load(const PsxSpuReverb *reverb, const uint8_t *ram, uint16_t reg, int delta) {
    return *(const int16_t *)__builtin_assume_aligned(ram + address(reverb, reg, delta), 2);
}

static SPU_HOT void store(const PsxSpuReverb *reverb, uint8_t *ram, uint16_t reg, int16_t value) {
    *(int16_t *)__builtin_assume_aligned(ram + address(reverb, reg, 0), 2) = value;
}

static SPU_HOT int32_t complement_product(int16_t old, int16_t alpha) {
    /* The -32768 IIR coefficient has a hardware special case. Avoid signed
     * overflow for old=-32768, whose normal complement product is 2^31. */
    if (alpha == -32768) return old == -32768 ? 0 : old * -65536;
    return old * (32768 - alpha);
}

static SPU_HOT int32_t negate_coefficient(int16_t value) {
    return value == -32768 ? 32767 : -value;
}

SPU_HOT void psx_spu_reverb_reset(PsxSpuReverb *reverb) { memset(reverb, 0, sizeof *reverb); }
SPU_HOT void psx_spu_reverb_base(PsxSpuReverb *reverb, uint16_t value) {
    reverb->base = reverb->cursor = (uint32_t)value * 4;
}

static SPU_HOT void channel_step(PsxSpuReverb *reverb, uint8_t *ram, unsigned ch,
                          int16_t input, int write_enabled) {
    const uint16_t *reg = reverb->registers;
    if (write_enabled) {
        int32_t incoming = (input * (int16_t)reg[INPUT_L + ch]) >> 14;
        int32_t same = (load(reverb, ram, reg[DSAME_L + ch], 0) * (int16_t)reg[WALL]) >> 14;
        int32_t cross = (load(reverb, ram, reg[DDIFF_L + (ch ^ 1)], 0) * (int16_t)reg[WALL]) >> 14;
        int16_t reflection[2] = {clip((same + incoming) >> 1), clip((cross + incoming) >> 1)};
        int16_t filtered[2];
        for (unsigned i = 0; i < 2; ++i) {
            unsigned dest = (i ? DIFF_L : SAME_L) + ch;
            int16_t old = load(reverb, ram, reg[dest], -1);
            int32_t fresh = (reflection[i] * (int16_t)reg[IIR]) >> 14;
            int32_t retained = complement_product(old, (int16_t)reg[IIR]) >> 14;
            filtered[i] = clip((fresh + retained) >> 1);
        }
        /* Both old reflection samples are read before either destination is
         * written, including when software makes the addresses alias. */
        store(reverb, ram, reg[SAME_L + ch], filtered[0]);
        store(reverb, ram, reg[DIFF_L + ch], filtered[1]);
    }
    int32_t comb = 0;
    static const unsigned comb_address[4] = {COMB1_L, COMB2_L, COMB3_L, COMB4_L};
    for (unsigned i = 0; i < 4; ++i)
        comb += (load(reverb, ram, reg[comb_address[i] + ch], 0) * (int16_t)reg[COMB1 + i]) >> 14;

    int16_t delayed1 = load(reverb, ram, (uint16_t)(reg[APF1_L + ch] - reg[DELAY1]), 0);
    int16_t delayed2 = load(reverb, ram, (uint16_t)(reg[APF2_L + ch] - reg[DELAY2]), 0);
    int16_t stage1 = clip((comb + ((delayed1 * negate_coefficient((int16_t)reg[APF1])) >> 14)) >> 1);
    int32_t feedback1 = (stage1 * (int16_t)reg[APF1]) >> 14;
    int32_t feedback2 = (delayed2 * negate_coefficient((int16_t)reg[APF2])) >> 14;
    int16_t stage2 = clip(delayed1 + ((feedback1 + feedback2) >> 1));
    int16_t wet = clip(delayed2 + ((stage2 * (int16_t)reg[APF2]) >> 15));
    if (write_enabled) {
        store(reverb, ram, reg[APF1_L + ch], stage1);
        store(reverb, ram, reg[APF2_L + ch], stage2);
    }
    unsigned pos = reverb->phase >> 1;
    reverb->up[ch][pos] = reverb->up[ch][pos + 32] = wet;
}

SPU_HOT void psx_spu_reverb_tick(PsxSpuReverb *reverb, uint8_t *ram, int16_t left, int16_t right,
                         int write_enabled, int16_t output[2]) {
    unsigned pos = reverb->phase;
    reverb->down[0][pos] = reverb->down[0][pos + 64] = left;
    reverb->down[1][pos] = reverb->down[1][pos + 64] = right;
    unsigned active = (pos & 1) ^ 1; /* right on even ticks, left on odd ticks */
    const int16_t *down = reverb->down[active] + ((pos - 38) & 63);
    int32_t sum = down[19] * 16384;
    /* fir is symmetric (fir[i] == fir[19 - i]): pair the taps. Same integer sum. */
    for (unsigned i = 0; i < 10; ++i) sum += (down[i * 2] + down[38 - i * 2]) * fir[i];
    channel_step(reverb, ram, active, clip(sum >> 15), write_enabled);
    if (active == 1) {
        reverb->cursor = (reverb->cursor + 1) & 0x3ffff;
        if (!reverb->cursor) reverb->cursor = reverb->base;
    }
    for (unsigned ch = 0; ch < 2; ++ch) {
        const int16_t *up = reverb->up[ch] + (((pos >> 1) - 19) & 31);
        if (ch == active) {
            sum = 0;
            for (unsigned i = 0; i < 10; ++i) sum += (up[i] + up[19 - i]) * fir[i];
            output[ch] = clip(sum >> 14);
        } else output[ch] = up[9];
    }
    reverb->phase = (pos + 1) & 63;
}
