/* PS1 GPU command decoding shared by the renderers (port/psx/gp0.c).
 *
 * gp0_run() walks one packet's GP0 words, keeps the drawing state the E1-E6 commands set, and hands each drawing
 * command to the active backend as a decoded primitive: the software rasteriser (softgpu.c: PC reference, DS
 * debug) or the DS 3D hardware (port/ds/render_gx.c). Coordinates are already offset (E5) but not clipped.
 */
#ifndef GP0_H
#define GP0_H

typedef struct {
    int x, y;    /* VRAM coordinates, drawing offset applied */
    int r, g, b; /* 0..255 */
    int u, v;    /* texture coordinates 0..255 */
} Gp0Vtx;

typedef struct {
    int textured, raw, semi, gouraud;
    int abr;    /* semi-transparency mode 0..3 (texture page bits 5-6) */
    int depth;  /* texture depth: 0 4-bit, 1 8-bit, 2 15-bit */
    int tx, ty; /* texture page base in VRAM */
    int clut_x, clut_y;
} Gp0Mode;

/* the drawing environment GP0 E1-E6 last set */
typedef struct {
    int area_x0, area_y0, area_x1, area_y1; /* inclusive */
    int ofs_x, ofs_y;
    unsigned int texpage;
    unsigned int twin_mask_x, twin_mask_y, twin_off_x, twin_off_y;
    int set_mask, check_mask;
} Gp0State;

extern Gp0State gp0_state;

typedef struct {
    void (*poly)(const Gp0Mode *m, const Gp0Vtx *v, int n); /* n = 3 or 4 (quad strip 0,1,2 / 2,1,3) */
    void (*line)(const Gp0Mode *m, const Gp0Vtx *a, const Gp0Vtx *b);
    void (*rect)(const Gp0Mode *m, const Gp0Vtx *v, int w, int h);
    void (*fill)(int x, int y, int w, int h, unsigned int rgb); /* fill command: VRAM coordinates, no offset */
    void (*frame)(void);  /* once per emulated VBlank, including waits between backbuffer updates */
} Gp0Backend;

extern const Gp0Backend *gp0_backend;

void gp0_run(const unsigned int *w, unsigned int len);
unsigned int gp0_run_plain(const unsigned int *w, unsigned int len);
int gp0_tpage_sprite16(const unsigned int *w); /* a texture page + SPRT_16 packet; 0: not run */
void gp0_note_sprites(unsigned int texpage, unsigned int n); /* its bookkeeping for n at once (gxh_bg_run) */
void psx_gpu_draw_area(int *x, int *y, int *w, int *h);
extern unsigned int psx_gpu_env_version; /* PutDrawEnv, PutDispEnv: the areas below may have changed */
void psx_gpu_display_area(int *x, int *y, int *w, int *h);

/* the scene's main OT and the entry of it the OT walk last passed (gpu.c) */
extern unsigned int *psx_main_ot;
extern unsigned int psx_main_ot_n, psx_ot_slot, psx_ot_generation;

#endif
