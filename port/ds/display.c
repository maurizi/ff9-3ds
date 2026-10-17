/* Top-screen software display: shrink the PSX framebuffer 4/5 into a bitmap.
 * Font sprite regions use area coverage to retain thin strokes; scene pixels
 * keep nearest-neighbour sampling. PSX VRAM and game coordinates stay intact.
 */
#include <nds.h>
#include <string.h>

#include "display.h"
#include "display_scale.h"
#include "psx_mem.h"
#include "profile.h"
#include "psx_display.h"
#ifdef PORT_GX_HYBRID
#include "gx_hybrid.h"
extern unsigned char *g_gameState;
#endif

extern unsigned short *psx_vram;
void psx_softgpu_enable(int on);

static int bg = -1;
static int back_buffer=1;
static DsTextRect text_rects[1024];
static DsTextRect previous_rects[1024];
static unsigned text_count;
static unsigned previous_count;
static int drawing;
unsigned int psx_display_font_calls, psx_display_font_presented;

void psx_display_drawing(void) {
#ifdef PORT_SOFT_DISPLAY
    if (!drawing) {
        memcpy(previous_rects,text_rects,text_count*sizeof *text_rects);
        previous_count=text_count;
        text_count=0;
        drawing=1;
    }
#endif
}

void psx_display_glyph(int x, int y, int w, int h) {
#ifdef PORT_SOFT_DISPLAY
    psx_display_font_calls++;
    if (text_count < sizeof text_rects / sizeof text_rects[0])
        text_rects[text_count++] = (DsTextRect){x,y,w,h};
#endif
}

void render_gx_init(void);
static void vblank_irq(void);
static volatile unsigned int real_vblanks;
static unsigned int virtual_frames;

void display_init(void) {
    if (!psx_vram) {
        return; /* 4 MB DS: no VRAM shadow, nothing to show */
    }
#ifndef PORT_SOFT_DISPLAY /* `make ds SOFTDISPLAY=1`: the software reference renderer on the DS, for comparison */
    render_gx_init();
    return;
#endif
#ifdef PORT_GX_HYBRID
    gxh_init();
#endif
    videoSetMode(MODE_5_2D);
    vramSetBankA(VRAM_A_MAIN_BG_0x06000000);
    vramSetBankB(VRAM_B_MAIN_BG_0x06020000);
    bg = bgInit(3, BgType_Bmp16, BgSize_B16_256x256, 0, 0);
    memset(BG_BMP_RAM_MAIN(0),0,0x40000);
    bgWrapOff(bg);
    /* The bitmap is already scaled; use the 2D engine at native DS size. */
    bgSetRotateScale(bg, 0, 256, 256);
    bgSetScroll(bg, 0, -6);
    bgUpdate();
    irqSet(IRQ_VBLANK, vblank_irq);
    irqEnable(IRQ_VBLANK);
    virtual_frames = real_vblanks;
    psx_softgpu_enable(1);
}

/* Copy the displayed area into the bitmap (bit 15 set: opaque). */
/* Virtual frames (psx_frame) are paced against real VBlanks: a frame waits only while it is ahead of real time,
 * as the PS1's VSync(n) counts the VBlanks that passed while the game was working. A frame that falls more than
 * MAX_LAG behind gives up the debt (the PS1 slows down too). The bitmap swap happens in the VBlank interrupt,
 * so presenting never waits for the beam. */
#define MAX_LAG 8
static volatile int queued_buffer = -1, queued_scroll;

static void vblank_irq(void) {
    real_vblanks++;
    if (queued_buffer >= 0) {
        bgSetMapBase(bg, queued_buffer * 8);
        bgSetScroll(bg, 0, queued_scroll);
        bgUpdate();
        queued_buffer = -1;
    }
}

static void pace(void) {
    unsigned int profile_wait = port_profile_begin();
    ++virtual_frames;
    if ((int)(real_vblanks - virtual_frames) > MAX_LAG) virtual_frames = real_vblanks;
    while ((int)(real_vblanks - virtual_frames) < 0) swiWaitForVBlank();
    port_profile_end(PORT_PROFILE_WAIT, profile_wait);
}

#ifdef PORT_GX_HYBRID
/* The hybrid renderer scales its 2D layer a block at a time (dst stride 256). */
static void scale_region(u16 *dst, int x, int y, int w, int h) {
    ds_scale_display_text_pair(dst, psx_vram, x, y, w, h, text_rects, text_count, previous_rects, previous_count);
}
#endif

void display_present(void) {
#ifndef PORT_SOFT_DISPLAY
    return;
#endif
    if (bg < 0) {
        swiWaitForVBlank();
        return;
    }
    PsxDisplayInfo info;
    psx_gpu_display_info(&info);
    const PsxDisplayInfo *d = &info;
    static PsxDisplayInfo shown;
    static unsigned int shown_generation;
    static int shown_valid;
#ifdef PORT_GX_HYBRID
    /* Battle (mode 2) uses the hybrid renderer; its 3D scenes and VRAM layout replace the software bitmaps. */
    int hybrid = g_gameState && g_gameState[8] == 2;
    if (hybrid != gxh_active()) {
        gxh_enable(hybrid);
        if (!hybrid) {
            videoSetMode(MODE_5_2D);
            vramSetBankA(VRAM_A_MAIN_BG_0x06000000);
            vramSetBankB(VRAM_B_MAIN_BG_0x06020000);
            bgShow(bg);
            bgUpdate();
        }
        shown_valid = 0;
    }
#endif
    /* Nothing written over the shown area and the same display area: the bitmap already shows this frame. Keep
     * it rather than scaling an identical copy (battle scenes change every fourth VSync, movies too). */
    if (shown_valid && shown_generation == psx_display_generation && !memcmp(&shown, d, sizeof shown)) {
        drawing = 0;
        pace();
        return;
    }
    shown = info;
    shown_valid = 1;
    int w = d->w ? d->w : 320, h = d->h ? d->h : 224;
    if (w > 320) {
        w = 320;
    }
    if (h > 240) {
        h = 240;
    }
    /* Watch the VRAM this frame reads (RGB24 spans 1.5 halfwords a pixel); a wrapping area watches everything. */
    int watch_w = d->rgb24 ? (w * 3 + 1) / 2 + 1 : w;
    if (d->x + watch_w > 1024 || d->y + h > 512) watch_w = 0;
    psx_display_watch[0] = d->x; psx_display_watch[1] = d->y; psx_display_watch[2] = watch_w; psx_display_watch[3] = h;
    shown_generation = psx_display_generation;
    unsigned int profile_scale=port_profile_begin();
    /* Scale into cached main RAM, then DMA to the VRAM back buffer: the ARM9's own stores to VRAM cross the
     * slow external bus and cost most of the scaling time. */
    static u16 frame[256*192] __attribute__((aligned(32)));
    static int frame_w, frame_h;
    u16 *dst = frame;
    if (w != frame_w || h != frame_h) {
        memset(frame, 0, sizeof frame);
        frame_w=w; frame_h=h;
    }
    psx_display_font_presented=text_count;
#ifdef PORT_GX_HYBRID
    if (gxh_active()) {
        gxh_present(d, scale_region);
        port_profile_end(PORT_PROFILE_SCALE,profile_scale);
        drawing = 0;
        pace();
        return;
    }
#endif
    if (d->rgb24)
        ds_scale_display_rgb24(dst, psx_vram, d->x, d->y, w, h);
    else
        ds_scale_display_text_pair(dst, psx_vram, d->x, d->y, w, h,
                                  text_rects, text_count, previous_rects, previous_count);
    DC_FlushRange(frame, sizeof frame);
    port_profile_end(PORT_PROFILE_SCALE,profile_scale);
    /* The back buffer is scanned out until a queued swap happens: wait for it before overwriting. */
    unsigned int profile_wait=port_profile_begin();
    while (queued_buffer >= 0) swiWaitForVBlank();
    port_profile_end(PORT_PROFILE_WAIT,profile_wait);
    dmaCopyWords(2, frame, BG_BMP_RAM_MAIN(back_buffer*8), sizeof frame);
    queued_scroll = -(192-h*4/5)/2;
    queued_buffer = back_buffer;
    back_buffer^=1;
    /* An extra VSync without drawing must retain the same filtered glyphs. */
    drawing = 0;
    pace();
}
