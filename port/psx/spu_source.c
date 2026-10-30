#include "psx_spu_source.h"
#include "spu_hot.h"
#include "spu_inline.h"
#include <string.h>

/* spu_layout.c: how the shadow keeps SPU RAM (weak: the SPU unit tests have one plain SPU RAM) */
extern unsigned psx_spu_ram_size __attribute__((weak)), psx_spu_ram_split[2] __attribute__((weak));
extern uint8_t *psx_spu_ram_mid __attribute__((weak)), *psx_spu_ram_hi __attribute__((weak));

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

/* Key-on, increment, and loop redirects all align to 16 bytes. The last
 * block starts at 0x7fff0 and fits entirely inside the RAM image. */
static inline const uint8_t *source_block(const PsxSpuSource *source, const uint8_t *ram) {
    unsigned byte_address = (unsigned)source->address * 8;
    static const uint8_t zeros[16] __attribute__((aligned(4)));
    return !&psx_spu_ram_size || byte_address < psx_spu_ram_split[0] ? ram + byte_address
         : byte_address < psx_spu_ram_split[1] ? psx_spu_ram_mid + byte_address
         : byte_address < psx_spu_ram_size ? psx_spu_ram_hi + byte_address : zeros;
}

SPU_HW_HOT void psx_spu_source_fetch(PsxSpuSource *source, const uint8_t *ram,
                          uint16_t *loop_address) {
    if (source->fetched) return;
    for (unsigned i = 0; i < 3; ++i)
        source->samples[i] = source->samples[PSX_SPU_BLOCK_SAMPLES + i];
    source->flags = psx_spu_decode_block(source_block(source, ram), &source->history,
                                         source->samples + 3);
    source->fetched = 1;
    if ((source->flags & 4) && !source->ignore_loop_start)
        *loop_address = source->address;
}

/* As psx_spu_source_fetch, its samples decoded into `to` instead (psx_spu_decode_block_to), not into the source's:
 * for the hardware path, which doesn't interpolate them. `to` 1: not decoded, its flags only. */
SPU_HW_ONLY_HOT void psx_spu_source_fetch_to(PsxSpuSource *source, const uint8_t *ram, uint16_t *loop_address,
                                        int16_t *to, int negate) {
    if (source->fetched) return;
    source->flags = to == (int16_t *)1 ? source_block(source, ram)[1] /* (PSX_SPU_UNHEARD: its flags only) */
                  : psx_spu_decode_block_to(source_block(source, ram), &source->history, to, negate);
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
