#ifndef PSX_DISPLAY_H
#define PSX_DISPLAY_H
#include <stdint.h>

/* X is a VRAM halfword coordinate; W is visible pixels even in RGB24 mode. */
typedef struct { int x, y, w, h, rgb24; } PsxDisplayInfo;
void psx_gpu_display_info(PsxDisplayInfo *info);

/* Packed RGB bytes in a 2048-byte VRAM row. Display-start X ignores bit 0.
 * Wrap within the same row rather than carrying into the next scanline. */
static inline uint32_t psx_display_rgb24(const uint16_t *vram, int x, int y, unsigned pixel) {
    const uint8_t *row = (const uint8_t *)(vram + ((unsigned)y & 511) * 1024);
    unsigned byte = (((unsigned)x & 1022) * 2 + pixel * 3) & 2047;
    return row[byte] | ((uint32_t)row[(byte + 1) & 2047] << 8)
                     | ((uint32_t)row[(byte + 2) & 2047] << 16);
}
#endif
