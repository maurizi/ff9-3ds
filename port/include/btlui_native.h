/* The battle UI's VBlank list drawn natively (port/game/btlui_native.c): its ordering table walked and its
 * primitives rasterised straight into an 8-bit indexed canvas (the DS's bottom screen), at 1:1 and laid out for the
 * canvas, redrawing only what changed since the previous list. */
#ifndef BTLUI_NATIVE_H
#define BTLUI_NATIVE_H

#include <stdint.h>

/* The platform's canvas: rows of 8-bit palette indices (index 0: nothing drawn), and its palette. */
typedef struct {
    int w, h;                       /* canvas size */
    int identity;                   /* 1: no layout (the PS1's screen coordinates; host validation) */
    void (*palette)(int index, uint16_t rgb15);
    void *store;                    /* the two lists' records */
    unsigned int store_size;
    void *tables;                   /* nui_tables_size bytes: its palette, dirty areas... */
    uint8_t *pixels;                /* the canvas (rows pitch bytes apart; written by halfwords) */
    int pitch;
    uint16_t *scratch;              /* scratch_rows rows of w RGB15 pixels: a band drawn before it's converted */
    int scratch_rows;
    uint8_t *top_pixels;            /* a second canvas (as large) for what points into the scene (NULL: dropped) */
    int top_pitch;
    void (*top_palette)(int index, uint16_t rgb15);
} NuiCanvas;

void nui_init(const NuiCanvas *canvas);
extern const unsigned int nui_tables_size;
/* draw the list at head (a battle VBlank list, btlui_submitOt); 0: not drawn (no canvas) */
int nui_list(const uint32_t *head);
/* the shown buffer changed: a frame with no list since the last change shows no UI */
void nui_frame(void);
/* VRAM written: lists sampling it redraw */
void nui_vram_written(int x, int y, int w, int h);
/* redraw everything at the next list (the canvas was changed by someone else) */
void nui_invalidate(void);
/* the top canvas's row where the scene (scaled to 4/5) starts */
void nui_set_top(int y);

extern unsigned int nui_stats_drawn, nui_stats_pixels, nui_stats_unknown, nui_stats_max;

#endif
