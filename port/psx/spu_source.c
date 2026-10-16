#include "psx_spu_source.h"
#include <string.h>

void psx_spu_source_key_on(PsxSpuSource *source, uint16_t start_address) {
    memset(source, 0, sizeof *source);
    source->address = start_address & 0xfffeu;
    source->first_block = 1;
}

void psx_spu_source_set_loop(PsxSpuSource *source, uint16_t *loop_address,
                             uint16_t value, int voice_on) {
    *loop_address = value;
    /* An explicit software loop overrides future ADPCM loop-start flags after
     * the first block, or while off. Key-on opens that initial window again. */
    if (!voice_on || !source->first_block) source->ignore_loop_start = 1;
}

void psx_spu_source_fetch(PsxSpuSource *source, const uint8_t *ram,
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

unsigned psx_spu_source_advance(PsxSpuSource *source, uint16_t pitch,
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
