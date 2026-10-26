/* FastVideoDS motion compensation on the CPU (port/ds/fv_mc.c). */
#ifndef FV_MC_H
#define FV_MC_H
#include <stdint.h>

#define FV_MC_WIDTH 256
#define FV_MC_STRIDE 256 /* pixels per row of a frame buffer, as in a VRAM bank */

/* The prediction of a P-frame of `h` rows from `ref` into `dst` (RGB555, bit 15 set), from the texcoords the vector
 * decoder wrote: `list_a` and `list_b` at the first block's texcoord in each list (5 words a block, 3 more between
 * the halves). */
void fv_mc_predict(const uint16_t *ref, uint16_t *dst, const uint32_t *list_a, const uint32_t *list_b, int h);
#endif
