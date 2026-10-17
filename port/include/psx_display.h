#ifndef PSX_DISPLAY_H
#define PSX_DISPLAY_H
#include <stdint.h>

/* X is a VRAM halfword coordinate; W is visible pixels even in RGB24 mode. */
typedef struct { int x, y, w, h, rgb24; } PsxDisplayInfo;
void psx_gpu_display_info(PsxDisplayInfo *info);
/* Counts VRAM writes (draw commands, fills, copies, uploads): a presenter can skip an unchanged frame. */
extern unsigned int psx_vram_generation;
/* Counts the VRAM writes that may touch psx_display_watch (x, y, w, h in halfwords; w 0: all of VRAM), the
 * rectangle a presenter last showed. Draws into a back buffer or texture pages elsewhere leave it alone. */
extern unsigned int psx_display_generation;
extern int psx_display_watch[4];
static inline void psx_display_written(int x, int y, int w, int h) {
    const int *r = psx_display_watch;
    if (!r[2] || x < 0 || y < 0 || x + w > 1024 || y + h > 512 ||
        (x < r[0] + r[2] && r[0] < x + w && y < r[1] + r[3] && r[1] < y + h))
        psx_display_generation++;
}

/* Packed RGB bytes in a 2048-byte VRAM row. Display-start X ignores bit 0.
 * Wrap within the same row rather than carrying into the next scanline. */
static inline uint32_t psx_display_rgb24(const uint16_t *vram, int x, int y, unsigned pixel) {
    const uint8_t *row = (const uint8_t *)(vram + ((unsigned)y & 511) * 1024);
    unsigned byte = (((unsigned)x & 1022) * 2 + pixel * 3) & 2047;
    return row[byte] | ((uint32_t)row[(byte + 1) & 2047] << 8)
                     | ((uint32_t)row[(byte + 2) & 2047] << 16);
}
#endif
