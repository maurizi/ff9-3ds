#ifndef PSX_SPU_ADPCM_H
#define PSX_SPU_ADPCM_H

#include <stdint.h>

enum { PSX_SPU_BLOCK_BYTES = 16, PSX_SPU_BLOCK_SAMPLES = 28 };

typedef struct {
    int16_t previous, older;
} PsxSpuAdpcmHistory;

/* Decode one SPU block, preserving predictor history across block/loop boundaries.
 * output must have 28 entries and must not overlap block or history.
 * The returned flag byte belongs to the voice sequencer, not the decoder. */
uint8_t psx_spu_decode_block(const uint8_t block[PSX_SPU_BLOCK_BYTES],
                           PsxSpuAdpcmHistory *history,
                           int16_t output[PSX_SPU_BLOCK_SAMPLES]);

#endif
