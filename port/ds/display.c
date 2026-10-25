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
#include "psx_runtime.h"
#include "psx_raster.h"
#ifdef PORT_GX_HYBRID
#include "gx_hybrid.h"
#endif
extern unsigned char *g_gameState;

extern unsigned short *psx_vram;
void psx_softgpu_enable(int on);

static int bg = -1;
static int back_buffer=1;
#define TEXT_RECTS 1024
static DsTextRect *text_rects, *previous_rects; /* TEXT_RECTS each (psx_spare_alloc) */
#define BAND_ROWS 16
static u16 *band; /* the software display's scaling band (display_present; psx_spare_alloc, 32-byte aligned) */
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
    if (text_count < TEXT_RECTS)
        text_rects[text_count++] = (DsTextRect){x,y,w,h};
#endif
}

void render_gx_init(void);
static void vblank_irq(void);
static volatile unsigned int real_vblanks;
static unsigned int virtual_frames;

void display_init(void) {
    text_rects = psx_spare_alloc(TEXT_RECTS * sizeof *text_rects);
    band = psx_spare_alloc(256 * BAND_ROWS * sizeof *band);
    psx_softgpu_spare_init();
    previous_rects = psx_spare_alloc(TEXT_RECTS * sizeof *previous_rects);
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

/* The PS1's VSync(n) returns once n VBlanks have passed since the previous call, however long the game worked in
 * between: virtual time catches up to within the call's own frames of real time, so they wait only for what's left
 * (a battle frame working 10 VBlanks then runs VSync(3) at once and VSync(0) to the next VBlank, as on the PS1,
 * rather than giving up the debt past MAX_LAG and waiting three more). */
/* One VBlank of slack beyond that: VSync(0), which waits for the next VBlank (the battle's btlui_waitFrame ends with
 * one), doesn't wait again after a frame that has already run late. The PS1 only lines its swap up with the beam
 * there; the DS swaps in its VBlank interrupt. A game that runs ahead is still held to the PS1's pace. */
void plat_vsync_begin(unsigned int frames) {
    if ((int)(real_vblanks - frames - 1 - virtual_frames) > 0) virtual_frames = real_vblanks - frames - 1;
}

/* Battle's window layer is drawn by the game at every VBlank (btlui_vsync), four times a 15 fps game frame, over the
 * shown buffer: drawing it at every UI_RATE-th VBlank (and whenever the shown buffer changes) leaves it as it was in
 * between, so only its animation (gauges, cursor) is less smooth (the user's choice of 2026-10-25: renderer shortcuts
 * that look the same for speed). */
#ifndef UI_RATE
#define UI_RATE 4
#endif
int psx_skip_vblank_list(void) {
    if (UI_RATE <= 1 || !g_gameState || g_gameState[8] != 2) return 0;
    static int shown_x = -1, shown_y = -1;
    static unsigned int count;
    PsxDisplayInfo d;
    psx_gpu_display_info(&d);
    if (d.x != shown_x || d.y != shown_y) {
        shown_x = d.x; shown_y = d.y; count = 0;
        return 0;
    }
    return ++count % UI_RATE != 0;
}

static void pace(void) {
    unsigned int profile_wait = port_profile_wait_begin();
    ++virtual_frames;
    if ((int)(real_vblanks - virtual_frames) > MAX_LAG) virtual_frames = real_vblanks;
    while ((int)(real_vblanks - virtual_frames) < 0) swiWaitForVBlank();
    port_profile_wait_end(profile_wait);
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
    /* Battle (mode 2) uses the hybrid renderer, fields (mode 1) too with FIELDGX=1 and the world map (mode 3) with
     * WORLDGX=1; its 3D scenes and VRAM layout replace the software bitmaps. A mode change re-enables it: its
     * buffers live in the mode's overlay. */
    int mode = g_gameState ? g_gameState[8] : 0;
    int hybrid = mode == 2;
#ifdef PORT_FIELD_GX
    hybrid |= mode == 1;
#endif
#ifdef PORT_WORLD_GX
    hybrid |= mode == 3;
#endif
    static int hybrid_mode = -1;
    if (hybrid != gxh_active() || (hybrid && mode != hybrid_mode)) {
        if (gxh_active()) gxh_enable(0);
        gxh_enable(hybrid);
        hybrid_mode = gxh_active() ? mode : -1;
        if (!gxh_active()) { /* off, or its overlay isn't resident yet */
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
    if (psx_display_watch[0] != d->x || psx_display_watch[1] != d->y || psx_display_watch[2] != watch_w ||
        psx_display_watch[3] != h) {
        psx_display_watch[0] = d->x; psx_display_watch[1] = d->y; psx_display_watch[2] = watch_w;
        psx_display_watch[3] = h;
        ++psx_display_watch_version;
    }
    shown_generation = psx_display_generation;
    unsigned int profile_scale=port_profile_begin();
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
    /* The back buffer is scanned out until a queued swap happens: wait for it before overwriting. */
    unsigned int profile_wait=port_profile_wait_begin();
    while (queued_buffer >= 0) swiWaitForVBlank();
    port_profile_wait_end(profile_wait);
    /* Scale into cached main RAM 16 output rows at a time, each DMA'd to the VRAM back buffer: the ARM9's own
     * stores to VRAM cross the slow external bus and cost most of the scaling time, and a whole frame's buffer
     * would cost 96 KB. A band's 16 rows come from 20 source rows; one more lets the text filter at its last row
     * read the next band's first, as a whole-frame pass does. */
    enum { BAND = BAND_ROWS };
    u16 *bitmap = (u16 *)BG_BMP_RAM_MAIN(back_buffer*8);
    const int out_h = h * 4 / 5;
    for (int oy = 0; oy < out_h; oy += BAND) {
        const int rows = out_h - oy < BAND ? out_h - oy : BAND, sy = oy * 5 / 4;
        const int src_h = h - sy < BAND * 5 / 4 + 1 ? h - sy : BAND * 5 / 4 + 1;
        if (d->rgb24)
            ds_scale_display_rgb24(band, psx_vram, d->x, d->y + sy, w, src_h);
        else
            ds_scale_display_text_pair(band, psx_vram, d->x, d->y + sy, w, src_h,
                                      text_rects, text_count, previous_rects, previous_count);
        DC_FlushRange(band, rows * 256 * sizeof *band);
        dmaCopyWords(2, band, bitmap + oy * 256, rows * 256 * sizeof *band);
    }
    if (out_h < 192) dmaFillWords(0, bitmap + out_h * 256, (192 - out_h) * 256 * sizeof *bitmap);
    port_profile_end(PORT_PROFILE_SCALE,profile_scale);
    queued_scroll = -(192-h*4/5)/2;
    queued_buffer = back_buffer;
    back_buffer^=1;
    /* An extra VSync without drawing must retain the same filtered glyphs. */
    drawing = 0;
    pace();
}
