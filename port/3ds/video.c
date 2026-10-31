/* SOFT=1: the bring-up presenter: the software renderer's PS1 display area copied by the CPU to the top screen, 1:1 and
 * centred (the PS1's 320x224/240 in the 3DS's 400x240). Replaced by the PICA200 renderer (render.c) once it runs. */
#ifdef PORT_SOFT_PRESENT
#include <3ds.h>
#include <string.h>

#include "ctr.h"
#include "psx_display.h"

void psx_softgpu_enable(int on);
extern unsigned short *psx_vram;

void ctr_video_init(void) {
    gfxSetScreenFormat(GFX_TOP, GSP_RGB565_OES);
    gfxSetDoubleBuffering(GFX_TOP, true);
    psx_softgpu_enable(1);
}

static inline u16 bgr555_to_rgb565(u16 c) {
    return (u16)((c & 0x1F) << 11 | (c & 0x3E0) << 1 | (c >> 10 & 0x1F));
}

void ctr_present(long frame) {
    (void)frame;
    if (!psx_vram) return;
    PsxDisplayInfo d;
    psx_gpu_display_info(&d);
    int w = d.w ? d.w : 320, h = d.h ? d.h : 240;
    int step = w > 400 ? 2 : 1; /* 512/640-wide modes: every other pixel */
    int sw = w / step, sh = h > 240 ? 240 : h;
    int ox = (400 - sw) / 2, oy = (240 - sh) / 2;
    u16 *fb = (u16 *)gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL);
    memset(fb, 0, 400 * 240 * 2);
    /* the top framebuffer is 240x400, column-major from the bottom-left: pixel (x, y) at fb[x * 240 + 239 - y] */
    for (int y = 0; y < sh; y++) {
        const u16 *row = psx_vram + ((d.y + y) & 511) * 1024;
        u16 *col = fb + ox * 240 + (239 - (oy + y));
        if (d.rgb24) {
            const u8 *src = (const u8 *)row + d.x * 2;
            for (int x = 0; x < sw; x++, col += 240) {
                const u8 *p = src + x * step * 3;
                *col = (u16)((p[0] >> 3) << 11 | (p[1] >> 2) << 5 | p[2] >> 3);
            }
        } else {
            for (int x = 0; x < sw; x++, col += 240)
                *col = bgr555_to_rgb565(row[(d.x + x * step) & 1023]);
        }
    }
    gfxFlushBuffers();
    gfxSwapBuffers();
}

void ctr_shutdown(void) {
    void ctr_audio_exit(void);
    ctr_audio_exit();
    gfxExit();
}
#endif
