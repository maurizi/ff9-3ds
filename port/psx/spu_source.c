#include "psx_spu_source.h"
#include "spu_hot.h"
#include "psx_spu_gauss.h"
#include <string.h>

SPU_HOT void psx_spu_source_key_on(PsxSpuSource *source, uint16_t start_address) {
    memset(source, 0, sizeof *source);
    source->address = start_address & 0xfffeu;
    source->first_block = 1;
}

SPU_HOT void psx_spu_source_set_loop(PsxSpuSource *source, uint16_t *loop_address,
                             uint16_t value, int voice_on) {
    *loop_address = value;
    /* An explicit software loop overrides future ADPCM loop-start flags after
     * the first block, or while off. Key-on opens that initial window again. */
    if (!voice_on || !source->first_block) source->ignore_loop_start = 1;
}

SPU_HOT void psx_spu_source_fetch(PsxSpuSource *source, const uint8_t *ram,
                          uint16_t *loop_address) {
    if (source->fetched) return;
    for (unsigned i = 0; i < 3; ++i)
        source->samples[i] = source->samples[PSX_SPU_BLOCK_SAMPLES + i];
    /* Key-on, increment, and loop redirects all align to 16 bytes. The last
     * block starts at 0x7fff0 and fits entirely inside the RAM image. */
    unsigned byte_address = (unsigned)source->address * 8;
    source->flags = psx_spu_decode_block(ram + byte_address, &source->history,
                                         source->samples + 3);
    source->fetched = 1;
    if ((source->flags & 4) && !source->ignore_loop_start)
        *loop_address = source->address;
}

SPU_HOT int16_t psx_spu_source_interpolate(const PsxSpuSource *source) {
    unsigned phase = (source->counter >> 4) & 255;
    const int16_t *samples = source->samples + (source->counter >> 12);
    /* Accumulate before rounding, as in both reference emulators. The largest
     * absolute coefficient sum is below 32768, so int32 cannot overflow even
     * with four full-scale input samples. See audio-validation.md. */
    int32_t total = samples[0] * psx_spu_gauss[255 - phase];
    total += samples[1] * psx_spu_gauss[511 - phase];
    total += samples[2] * psx_spu_gauss[256 + phase];
    total += samples[3] * psx_spu_gauss[phase];
    return (int16_t)(total >> 15);
}

SPU_HOT unsigned psx_spu_source_advance(PsxSpuSource *source, uint16_t pitch,
                                uint16_t loop_address, int noise_enabled) {
    if (pitch > 0x3fff) pitch = 0x3fff;
    source->counter += pitch;
    if (source->counter < (PSX_SPU_BLOCK_SAMPLES << 12)) return 0;

    source->counter -= PSX_SPU_BLOCK_SAMPLES << 12;
    source->address = (uint16_t)(source->address + 2);
    source->fetched = 0;
    source->first_block = 0;
    if (!(source->flags & 1)) return 0;
    source->address = loop_address & 0xfffeu;
    if (!(source->flags & 2) && !noise_enabled)
        return PSX_SPU_SOURCE_END | PSX_SPU_SOURCE_MUTE;
    return PSX_SPU_SOURCE_END;
}
