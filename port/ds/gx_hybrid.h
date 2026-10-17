#ifndef GX_HYBRID_H
#define GX_HYBRID_H
#include "psx_display.h"
/* Hybrid renderer (gx_hybrid.c, docs/design.md §5.0). */
void gxh_init(void);
void gxh_enable(int on);   /* switch VRAM and the GP0 backend between hybrid and all-software */
int gxh_active(void);
/* The displayed area and its scaled image (256-wide): submits that buffer's 3D scene, rebuilds the 2D layer. */
void gxh_present(const PsxDisplayInfo *d, void (*scale)(unsigned short *dst, int x, int y, int w, int h));
extern int gxh_stats_polys, gxh_stats_dropped, gxh_stats_texture_fail, gxh_stats_blocks;
#endif
