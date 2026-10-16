#ifndef MDEC_DECODE_H
#define MDEC_DECODE_H
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t quant[2][64]; /* luminance, chroma; stream/zigzag order */
    int16_t scale[64];   /* original uploaded matrix, frequency-major */
} PsxMdecEnv;
enum { PSX_MDEC_OK=0, PSX_MDEC_TRUNCATED=-1, PSX_MDEC_CAPACITY=-2, PSX_MDEC_MODE=-3 };
void psx_mdec_env_init(PsxMdecEnv *env, const uint8_t quant[128], const uint8_t scale[128]);
/* Decode six RLE blocks (Cr,Cb,Y1..Y4) to one row-major 16x16 macroblock.
 * command uses the original MDEC(1) depth/signed/STP bits: RGB24 or RGB555.
 * capacity is bytes (768 or 512 needed); consumed counts input halfwords.
 * No allocation or global state. Does not implement MMIO/DMA/timing. */
int psx_mdec_macroblock(const PsxMdecEnv *env, const uint16_t *rl, size_t halfwords,
                       uint32_t command, uint8_t *pixels, size_t capacity, size_t *consumed);
#endif
