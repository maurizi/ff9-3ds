#include "psx_spu_source.h"
#include "spu_hot.h"
#include "spu_inline.h"
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
    return spu_source_interpolate(source);
}

SPU_HOT unsigned psx_spu_source_advance(PsxSpuSource *source, uint16_t pitch,
                                uint16_t loop_address, int noise_enabled) {
    return spu_source_advance(source, pitch, loop_address, noise_enabled);
}
