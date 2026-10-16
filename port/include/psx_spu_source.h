#ifndef PSX_SPU_SOURCE_H
#define PSX_SPU_SOURCE_H

#include "psx_spu_adpcm.h"

enum {
    PSX_SPU_RAM_BYTES = 512 * 1024,
    PSX_SPU_SOURCE_END = 1,
    PSX_SPU_SOURCE_MUTE = 2
};

/* ADPCM fetch/pitch state only. Envelope, key-event timing, ENDX, noise sample
 * generation, RAM IRQs and mixing belong to the enclosing SPU core. Keeping
 * source state separate lets silent/released/noise voices continue fetching. */
typedef struct {
    PsxSpuAdpcmHistory history;
    int16_t samples[3 + PSX_SPU_BLOCK_SAMPLES];
    uint32_t counter; /* source index in 4.12 units, always below 28 << 12 */
    uint16_t address; /* current block in 8-byte SPU register units */
    uint8_t flags, fetched, first_block, ignore_loop_start;
} PsxSpuSource;

/* Start a voice using SSA; do not change the programmed SSA or loop register.
 * Neither key-off nor looping should call this (both preserve source history). */
void psx_spu_source_key_on(PsxSpuSource *source, uint16_t start_address);

/* Observe an explicit LSAX write, including writes that repeat the same value.
 * voice_on describes the envelope state, not whether source fetching is enabled. */
void psx_spu_source_set_loop(PsxSpuSource *source, uint16_t *loop_address,
                             uint16_t value, int voice_on);

/* Fetch one block when needed. May update the visible LSAX register on bit 2.
 * ram must provide the full 512 KiB SPU RAM image. */
void psx_spu_source_fetch(PsxSpuSource *source, const uint8_t *ram,
                          uint16_t *loop_address);

/* Four-tap Gaussian sample at the current 4.12 source position. Requires fetch
 * first. Counter bits 0..3 are ignored; bits 4..11 select the coefficient phase. */
int16_t psx_spu_source_interpolate(const PsxSpuSource *source);

/* Advance AFTER sampling the current position. pitch is already modulated;
 * hardware limits it to 0x3fff. Returns ENDX and envelope-mute actions for the
 * enclosing core. End+mute is ignored for noise voices, but ENDX/looping aren't.
 * The enclosing core calls fetch before sampling again, even when muted. */
unsigned psx_spu_source_advance(PsxSpuSource *source, uint16_t pitch,
                                uint16_t loop_address, int noise_enabled);

#endif
