#ifndef MDEC_DECODE_H
#define MDEC_DECODE_H
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t quant[2][64]; /* luminance, chroma; stream/zigzag order */
    int16_t scale[64];   /* original uploaded matrix, frequency-major */
    uint32_t scale_max;  /* largest |scale|, for the IDCT's overflow bounds */
    int mirrored;        /* scale has the standard row symmetry (see psx_mdec_env_init) */
} PsxMdecEnv;
/* DS: code and tables in main RAM. ITCM goes to the renderer and SPU, which run every frame; DTCM holds the ARM9
 * stack, which deep game call chains (battles) need nearly all of. MDEC_ITCM=1 builds (benchmarks) can opt in. */
#define MDEC_DTCM_BSS
#define MDEC_DTCM_DATA
#if defined(__arm__) && defined(PORT_MDEC_ITCM)
#define MDEC_ITCM __attribute__((section(".itcm"), long_call))
#else
#define MDEC_ITCM
#endif
enum { PSX_MDEC_OK=0, PSX_MDEC_TRUNCATED=-1, PSX_MDEC_CAPACITY=-2, PSX_MDEC_MODE=-3 };
void psx_mdec_env_init(PsxMdecEnv *env, const uint8_t quant[128], const uint8_t scale[128]);
/* Decode six RLE blocks (Cr,Cb,Y1..Y4) to one row-major 16x16 macroblock.
 * command uses the original MDEC(1) depth/signed/STP bits: RGB24 or RGB555.
 * capacity is bytes (768 or 512 needed); consumed counts input halfwords.
 * No allocation or global state. Does not implement MMIO/DMA/timing. */
int psx_mdec_macroblock(const PsxMdecEnv *env, const uint16_t *rl, size_t halfwords,
                       uint32_t command, uint8_t *pixels, size_t capacity, size_t *consumed);
#endif
