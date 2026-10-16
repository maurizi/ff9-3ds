#ifndef PSX_SPU_REVERB_H
#define PSX_SPU_REVERB_H
#include <stdint.h>

typedef struct {
    uint16_t registers[32];
    uint32_t base, cursor; /* halfword addresses in 512 KiB SPU RAM */
    int16_t down[2][128], up[2][64]; /* duplicated FIR ring histories */
    unsigned phase;
} PsxSpuReverb;

void psx_spu_reverb_reset(PsxSpuReverb *reverb);
/* ESA writes reset the RAM cursor, but do not discard the FIR histories. */
void psx_spu_reverb_base(PsxSpuReverb *reverb, uint16_t base_register);
/* One 44.1 kHz tick. Input is already clamped to signed PCM16. Outputs precede
 * EVOLL/EVOLR. Write-disable preserves RAM, but reads/output/history continue.
 * Channels alternate each tick; the RAM cursor advances after the right step. */
void psx_spu_reverb_tick(PsxSpuReverb *reverb, uint8_t *ram, int16_t left, int16_t right,
                         int write_enabled, int16_t output[2]);
#endif
