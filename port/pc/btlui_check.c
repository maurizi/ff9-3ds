/* Host side of the battle UI's native renderer (port/game/btlui_native.c), --native-ui MODE:
 *   check  the PS1's layout (identity): every list is also drawn by the software GPU over a black screen and the
 *          two are compared pixel by pixel (mismatches printed); the game then draws it as usual
 *   ds     the DS's 256x192 layout: canvas written as nui_NNNNNN.ppm in the dump directory with each --dump frame */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "btlui_native.h"
#include "psx_display.h"

extern uint16_t *psx_vram;
extern unsigned char *g_gameState;
void DrawOTag(uint32_t *p);
void nui_debug_pixel(int x, int y);

static int mode; /* 0 off, 1 check, 2 ds */
static uint8_t canvas[240][320] __attribute__((aligned(4))), top[192][256] __attribute__((aligned(4)));
static uint16_t top_palette[256];
static void set_top_palette(int i, uint16_t c) { top_palette[i] = c; }
static uint16_t palette[256];
static uint16_t scratch[16 * 320];
static uint64_t store[48 * 1024 / 8];
static unsigned long lists, mismatched_lists, mismatched_pixels;

static void set_palette(int i, uint16_t c) { palette[i] = c; }

void btlui_check_mode(const char *m) {
    mode = !strcmp(m, "check") ? 1 : !strcmp(m, "ds") ? 2 : 0;
    if (!mode) { fprintf(stderr, "--native-ui check|ds\n"); exit(1); }
    NuiCanvas c = {mode == 1 ? 320 : 256, mode == 1 ? 240 : 192, mode == 1, set_palette, store, sizeof store,
                   &canvas[0][0], 320, scratch, 16, mode == 2 ? &top[0][0] : NULL, 256, set_top_palette};
    nui_set_top((192 - 224 * 4 / 5) / 2);
    nui_init(&c);
}

void psx_vram_touch_native(int x, int y, int w, int h) {
    if (mode) nui_vram_written(x, y, w, h);
}

int psx_vblank_list_native(uint32_t *list) {
    if (!mode || !g_gameState || g_gameState[8] != 2) return 0;
    nui_list(list);
    if (mode == 2) return 1;
    /* the software GPU's drawing of it over a black screen (and an empty row cache), to compare */
    PsxDisplayInfo d;
    psx_gpu_display_info(&d);
    const int w = d.w ? d.w : 320, h = d.h ? d.h : 240;
    static uint16_t saved[240][320], saved_cache[80][64];
    for (int y = 0; y < h; ++y) {
        memcpy(saved[y], psx_vram + (d.y + y) * 1024 + d.x, (size_t)w * 2);
        memset(psx_vram + (d.y + y) * 1024 + d.x, 0, (size_t)w * 2);
    }
    for (int y = 0; y < 80; ++y) {
        memcpy(saved_cache[y], psx_vram + (432 + y) * 1024 + 960, 128);
        memset(psx_vram + (432 + y) * 1024 + 960, 0, 128);
    }
    DrawOTag(list);
    unsigned long bad = 0;
    int first_x = -1, first_y = -1;
    for (int y = 0; y < h && y < 240; ++y)
        for (int x = 0; x < w && x < 320; ++x) {
            uint16_t want = psx_vram[(d.y + y) * 1024 + d.x + x] & 0x7FFF, got = palette[canvas[y][x]];
            if (want != got) { if (!bad) first_x = x, first_y = y; ++bad; }
        }
    ++lists;
    if (bad) {
        ++mismatched_lists;
        mismatched_pixels += bad;
        if (mismatched_lists <= 20)
            fprintf(stderr, "native ui: list %lu: %lu pixels differ (first %d,%d: %04x, native %04x)\n", lists, bad,
                    first_x, first_y, psx_vram[(d.y + first_y) * 1024 + d.x + first_x] & 0x7FFF,
                    palette[canvas[first_y][first_x]]);
        if (mismatched_lists <= 3) nui_debug_pixel(first_x, first_y);
    }
    for (int y = 0; y < h; ++y) memcpy(psx_vram + (d.y + y) * 1024 + d.x, saved[y], (size_t)w * 2);
    for (int y = 0; y < 80; ++y) memcpy(psx_vram + (432 + y) * 1024 + 960, saved_cache[y], 128);
    return 0; /* drawn as usual */
}

void btlui_check_frame(void) {
    if (mode) nui_frame();
}

void btlui_check_dump(const char *dir, long frame) {
    if (mode != 2) return;
    char path[512];
    snprintf(path, sizeof path, "%s/nui_%06ld.ppm", dir, frame);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n256 192\n255\n");
    for (int y = 0; y < 192; ++y)
        for (int x = 0; x < 256; ++x) {
            uint16_t c = palette[canvas[y][x]];
            unsigned char rgb[3] = {(unsigned char)((c & 31) << 3), (unsigned char)(((c >> 5) & 31) << 3),
                                    (unsigned char)(((c >> 10) & 31) << 3)};
            fwrite(rgb, 1, 3, f);
        }
    fclose(f);
    snprintf(path, sizeof path, "%s/nui_top_%06ld.ppm", dir, frame);
    if (!(f = fopen(path, "wb"))) return;
    fprintf(f, "P6\n256 192\n255\n");
    for (int y = 0; y < 192; ++y)
        for (int x = 0; x < 256; ++x) {
            uint16_t c = top_palette[top[y][x]];
            unsigned char rgb[3] = {(unsigned char)((c & 31) << 3), (unsigned char)(((c >> 5) & 31) << 3),
                                    (unsigned char)(((c >> 10) & 31) << 3)};
            fwrite(rgb, 1, 3, f);
        }
    fclose(f);
}

void btlui_check_report(void) {
    if (mode == 1)
        fprintf(stderr, "native ui: %lu lists, %lu differ (%lu pixels); %u records drawn, %u pixels, %u unknown\n",
                lists, mismatched_lists, mismatched_pixels, nui_stats_drawn, nui_stats_pixels, nui_stats_unknown);
}

/* (gpu.c's weak one does nothing) */
void psx_vram_touch(int x, int y, int w, int h) {
    psx_vram_touch_native(x, y, w, h);
}
