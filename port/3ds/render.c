/* The PICA200 renderer for the PS1 primitive stream (docs/design.md §4.1).
 *
 * VRAM mirror. All of PS1 VRAM (1024x512) is a render target per eye: a 1024x512 RGB565 texture in the 3DS's VRAM
 * (the PS1's 15-bit colour fits; the mask bit isn't kept). GPU coordinates are PS1 VRAM coordinates, so the game's
 * double buffers, offscreen effect passes and screen captures all land where the PS1 drew them. The CPU keeps the
 * 1 MB shadow (psx_vram) for uploads, CLUTs and readbacks. Per 32x32 cell, either side may be stale:
 *   - a CPU write (LoadImage, ClearImage, MoveImage, fills) updates the shadow; the cell is copied to the mirror
 *     (a "blit" from a staging texture, in drawing order) before anything is drawn over it or it is displayed;
 *   - a GPU draw makes the shadow stale; a CPU read (StoreImage, a texture conversion) first reads the cell back
 *     from the left eye's mirror.
 *
 * Primitives. Each decoded primitive (gp0.c) becomes triangles, in the PS1's drawing order: batches of consecutive
 * primitives with the same pipeline state, replayed into each eye's mirror at the end of every ordering table.
 * PS1 blend modes 0-3 are GL blend equations; textured semi-transparent primitives draw their opaque texels and
 * their semi-transparent texels in two passes, told apart by the texel's alpha (RGBA8 textures: 0 transparent,
 * 0x80 semi-transparent, 0xFF opaque) through the texture combiners. Texture modulation is the PS1's: texel x
 * vertex colour x 2 (0x80 = 1.0).
 *
 * Textures. CLUT pages become 256x256 RGBA8 textures keyed by (page, depth, CLUT), converted lazily in 32x32-texel
 * blocks, each block valid until a cell it reads (texels or CLUT) is written again.
 *
 * Depth. A vertex whose packet word the GTE stored (gte.h depth capture) gets its sub-pixel position and SZ: the
 * vertex shader (prim.v.pica) shifts it for each eye by its exact stereo parallax and interpolates its texture
 * perspective-correctly. 2D primitives stay at the screen plane.
 */
#ifndef PORT_SOFT_PRESENT
#include <3ds.h>
#include <citro3d.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "ctr.h"
#include "gp0.h"
#include "gte.h"
#include "profile.h"
#include "psx_display.h"
#include "psx_mem.h"
#include "psx_runtime.h"
#include "prim_shbin.h"

extern unsigned short *psx_vram;

#define VW 1024
#define VH 512
#define CELL 32
#define CW (VW / CELL)
#define CH (VH / CELL)

/* ---- GPU objects ------------------------------------------------------------------------------------------ */

typedef struct {
    float x, y, z;
    float u, v;
    u32 rgba; /* 0xAABBGGRR as bytes R, G, B, A */
} Vtx;

#define VB_MAX (96 * 1024)
static Vtx *vb;
static unsigned vb_used, vb_submitted;

#define STAGING_SIZE (3 * 1024 * 1024)
static u8 *staging;
static unsigned staging_used;

static DVLB_s *shader_dvlb;
static shaderProgram_s shader;
static int u_proj, u_stereo, u_texscale;

static C3D_Tex mirror_tex[2];
static C3D_RenderTarget *mirror_rt[2];
static C3D_RenderTarget *top_rt[2];
/* the bottom screen's UI layer: PS1 screen coordinates at 1:1 (the bottom screen is the PS1's 320 wide) */
#define UI_W 512
#define UI_H 256
/* Two layers, one per PS1 framebuffer (a field's windows are drawn into the frame being built and belong to it
 * when it is shown), RGBA8, transparent where nothing was drawn: composited over the dimmed scene when presented. */
typedef struct {
    int x, y;              /* the framebuffer's origin in VRAM (-1: unused) */
    C3D_Tex tex;
    C3D_RenderTarget *rt;
    int shown;             /* presented since it was last cleared: the next frame drawn into the buffer clears it */
    unsigned drawn;        /* frames_drawn when UI was last drawn into it */
    int hands[CTR_MAX_HANDS][2], nhands;
} UiLayer;
static UiLayer layers[2];
/* while a menu is up, the top screen shows the scene as it was when the menu was asked for (per eye) */
static C3D_Tex freeze_tex[2];
static C3D_RenderTarget *freeze_rt[2];
static int freeze_w, freeze_h, frozen;
static int menu_state; /* port_menu_state(), read at each present */
__attribute__((weak)) int port_menu_state(void) { return 0; }
static C3D_RenderTarget *bottom_rt;

static int in_frame;
static unsigned frame_serial = 1; /* bumped at each C3D frame: what was recorded since */
static int stereo_on;            /* eye 1 drawn as well */
static float stereo_k;           /* parallax constant of eye 1 (eye 0 gets -k) */
static float stereo_inv_zc = 1.0f / 1500.0f;
#define STEREO_MAX_PX 8.0f
static unsigned display_shown_gen = ~0u, frames_drawn;
unsigned int ctr_render_stats[8];
unsigned int ctr_depth_stats[2]; /* vertices with / without GTE depth */ /* batches, primitives, blits, readbacks, texture blocks, slots, syncs */

/* ---- cell bookkeeping ------------------------------------------------------------------------------------- */

static u8 shadow_ok[CH][CW]; /* the shadow holds the cell's current pixels */
static u8 mirror_ok[CH][CW]; /* the mirror (both eyes) holds them */
static u32 cell_gen[CH][CW]; /* write counter: texture blocks made from the cell are stale when it moves */
static u32 gen_counter = 1;
/* Cells some texture block was made from (texels or CLUT), and a counter bumped by any write to one: a texture whose
 * epoch is current needs no check of its blocks' cells. Draws into the framebuffers, which aren't sources, don't
 * bump it; uploads (rare per frame) do. */
static u8 cell_is_source[CH][CW];
static u32 tex_epoch = 1;

static void readback_cells(int cx0, int cy0, int cx1, int cy1);
static void blit_cells(int cx0, int cy0, int cx1, int cy1);

static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* cells of [x0,x1)x[y0,y1), clipped to VRAM; 0 if empty */
static inline int cell_range(int x0, int y0, int x1, int y1, int *cx0, int *cy0, int *cx1, int *cy1) {
    x0 = clampi(x0, 0, VW); x1 = clampi(x1, 0, VW);
    y0 = clampi(y0, 0, VH); y1 = clampi(y1, 0, VH);
    if (x0 >= x1 || y0 >= y1) return 0;
    *cx0 = x0 / CELL; *cy0 = y0 / CELL; *cx1 = (x1 + CELL - 1) / CELL; *cy1 = (y1 + CELL - 1) / CELL;
    return 1;
}

/* The GPU is about to draw into [x0,x1)x[y0,y1): its cells must be current in the mirror; then the shadow isn't. */
static void gpu_draws(int x0, int y0, int x1, int y1) {
    int cx0, cy0, cx1, cy1;
    if (!cell_range(x0, y0, x1, y1, &cx0, &cy0, &cx1, &cy1)) return;
    for (int cy = cy0; cy < cy1; cy++)
        for (int cx = cx0; cx < cx1; cx++)
            if (!mirror_ok[cy][cx]) { blit_cells(cx0, cy0, cx1, cy1); goto ok; }
ok:
    gen_counter++;
    for (int cy = cy0; cy < cy1; cy++)
        for (int cx = cx0; cx < cx1; cx++) {
            shadow_ok[cy][cx] = 0;
            cell_gen[cy][cx] = gen_counter;
            if (cell_is_source[cy][cx]) tex_epoch++;
        }
}

/* The CPU reads [x0,x1)x[y0,y1) of the shadow. */
static void cpu_reads(int x0, int y0, int x1, int y1) {
    int cx0, cy0, cx1, cy1;
    if (!cell_range(x0, y0, x1, y1, &cx0, &cy0, &cx1, &cy1)) return;
    for (int cy = cy0; cy < cy1; cy++)
        for (int cx = cx0; cx < cx1; cx++)
            if (!shadow_ok[cy][cx]) { readback_cells(cx0, cy0, cx1, cy1); return; }
}

static int filling; /* psx_vram_filled -> psx_vram_touch: a fill the GPU draws itself */

/* gpu.c: the CPU is about to write [x,x+w)x[y,y+h) of the shadow (LoadImage, MoveImage, ClearImage, fills). Cells
 * it covers only in part keep their other pixels: read those back first if the GPU owns them. */
void psx_vram_touch(int x, int y, int w, int h) {
    int cx0, cy0, cx1, cy1;
    if (!cell_range(x, y, x + w, y + h, &cx0, &cy0, &cx1, &cy1)) return;
    int partial = (x % CELL) || (y % CELL) || ((x + w) % CELL && x + w < VW) || ((y + h) % CELL && y + h < VH);
    if (partial && !filling) cpu_reads(x, y, x + w, y + h);
    gen_counter++;
    tex_epoch++;
    for (int cy = cy0; cy < cy1; cy++)
        for (int cx = cx0; cx < cx1; cx++) {
            if (filling) {
                /* the fill is drawn into the mirror too (psx_vram_filled): both sides current where it covers a
                 * whole cell; a partly covered cell keeps the GPU's state */
                int whole = cx * CELL >= x && (cx + 1) * CELL <= x + w && cy * CELL >= y && (cy + 1) * CELL <= y + h;
                if (whole) shadow_ok[cy][cx] = mirror_ok[cy][cx] = 1;
                else if (!shadow_ok[cy][cx]) { /* (the shadow part is updated too, but other pixels are stale) */ }
            } else {
                shadow_ok[cy][cx] = 1;
                mirror_ok[cy][cx] = 0;
            }
            cell_gen[cy][cx] = gen_counter;
        }
    filling = 0;
}

/* gpu.c: before the CPU reads VRAM (StoreImage, MoveImage's source) */
void psx_vram_read(int x, int y, int w, int h) {
    cpu_reads(x, y, x + w, y + h);
}

/* ---- the C3D frame ------------------------------------------------------------------------------------------ */

static C3D_AttrInfo attr;
static C3D_BufInfo bufinfo;

static void frame_begin(void) {
    if (in_frame) return;
    C3D_FrameBegin(0); /* waits for the GPU to finish the previous frame: its vertices and staging are free */
    in_frame = 1;
    vb_used = vb_submitted = 0;
    staging_used = 0;
    frame_serial++;
    C3D_BindProgram(&shader);
    C3D_SetAttrInfo(&attr);
    BufInfo_Init(&bufinfo);
    BufInfo_Add(&bufinfo, vb, sizeof(Vtx), 3, 0x210);
    C3D_SetBufInfo(&bufinfo);
    C3D_CullFace(GPU_CULL_NONE);
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
}

static void submit(void);

/* everything recorded so far executed and finished (the CPU may then read the mirror, or rewrite a texture) */
static void gpu_sync(void) {
    submit();
    if (in_frame) {
        C3D_FrameEnd(GX_CMDLIST_FLUSH) /* (we flush what we write ourselves: not the whole linear heap) */;
        in_frame = 0;
    }
    frame_begin();
    ctr_render_stats[6]++;
}

/* ---- batches ------------------------------------------------------------------------------------------------ */

enum { K_UNTEX, K_TEX, K_TEX_P1, K_TEX_P2, K_BLIT, K_COMPOSITE, K_SCENE };
#define MASK_SET 1   /* drawn pixels get the mask bit (alpha 1); else the texel's (textured) or 0 */
#define MASK_CHECK 2 /* pixels with the mask bit keep their colour (opaque draws) */
#define MASK_COPY 4  /* an upload: the source's mask bits as they are */
static int cur_mask;  /* add_batch's: gp0_state's at the primitive (blits: MASK_SET, alpha from the source) */
#define NO_BLEND 4

typedef struct {
    u8 kind, abr, eyes, target; /* target: 0 the VRAM mirror, 1 + n UI layer n */
    u8 mask, pad2[3];           /* MASK_SET | MASK_CHECK: the PS1's mask bit as the mirror's alpha (GP0 E6) */
    u16 sc[4]; /* scissor: x0, y0, x1, y1 (exclusive) */
    C3D_Tex *tex;
    u32 first, count;
} Batch;

#define MAX_BATCHES 4096
static Batch batches[MAX_BATCHES];
static unsigned nbatches;

static Vtx *alloc_vtx(unsigned n) {
    if (!in_frame) frame_begin();
    if (vb_used + n > VB_MAX) gpu_sync();
    Vtx *v = vb + vb_used;
    vb_used += n;
    return v;
}

/* the batch the next n vertices (just allocated at first) belong to */
#ifdef PORT_NODEPTH
static const int getenv_nodepth = 1;
#else
static const int getenv_nodepth = 0;
#endif
static int ui_mode;    /* drawing the inserted list (the battle UI): to the UI layer */
static int cur_target; /* where add_batch records: 0 the mirror, 1 + n UI layer n */
static void add_batch(int kind, int abr, int eyes, const u16 sc[4], C3D_Tex *tex, unsigned first, unsigned n) {
    if (nbatches) {
        Batch *b = &batches[nbatches - 1];
        if (b->kind == kind && b->abr == abr && b->eyes == eyes && b->tex == tex && b->first + b->count == first &&
            b->target == cur_target && b->mask == cur_mask &&
            !memcmp(b->sc, sc, sizeof b->sc)) {
            b->count += n;
            return;
        }
    }
    if (nbatches == MAX_BATCHES) submit();
    Batch *b = &batches[nbatches++];
    b->kind = kind; b->abr = abr; b->eyes = eyes; b->tex = tex; b->target = (u8)cur_target; b->mask = (u8)cur_mask;
    memcpy(b->sc, sc, sizeof b->sc);
    b->first = first; b->count = n;
}

static void set_tev_for(int kind, int layer);
static void set_tev(int kind) { set_tev_for(kind, 1); }
/* (layer: a UI layer's, or a screen's; else the mirror's, whose alpha is the mask bit) */
static void set_tev_for(int kind, int layer) {
    C3D_TexEnv *e0 = C3D_GetTexEnv(0), *e1 = C3D_GetTexEnv(1);
    C3D_TexEnvInit(e0);
    C3D_TexEnvInit(e1);
    switch (kind) {
    case K_UNTEX: /* (alpha from the vertex: 1, or 0 for a UI layer's clear) */
        C3D_TexEnvSrc(e0, C3D_Both, GPU_PRIMARY_COLOR, 0, 0);
        C3D_TexEnvFunc(e0, C3D_Both, GPU_REPLACE);
        break;
    case K_COMPOSITE: /* a UI layer: its colour and alpha */
        C3D_TexEnvSrc(e0, C3D_Both, GPU_TEXTURE0, 0, 0);
        C3D_TexEnvFunc(e0, C3D_Both, GPU_REPLACE);
        break;
    case K_BLIT: /* a copy: colour and alpha (an upload's mask bits) as they are */
        C3D_TexEnvSrc(e0, C3D_Both, GPU_TEXTURE0, 0, 0);
        C3D_TexEnvFunc(e0, C3D_Both, GPU_REPLACE);
        break;
    case K_SCENE: /* the mirror shown (texel x vertex colour x 2), opaque whatever its mask bits */
        C3D_TexEnvSrc(e0, C3D_RGB, GPU_TEXTURE0, GPU_PRIMARY_COLOR, 0);
        C3D_TexEnvFunc(e0, C3D_RGB, GPU_MODULATE);
        C3D_TexEnvScale(e0, C3D_RGB, GPU_TEVSCALE_2);
        C3D_TexEnvSrc(e0, C3D_Alpha, GPU_CONSTANT, 0, 0);
        C3D_TexEnvFunc(e0, C3D_Alpha, GPU_REPLACE);
        C3D_TexEnvColor(e0, 0xFF000000);
        break;
    default: /* textured: texel x vertex colour x 2 */
        C3D_TexEnvSrc(e0, C3D_RGB, GPU_TEXTURE0, GPU_PRIMARY_COLOR, 0);
        C3D_TexEnvFunc(e0, C3D_RGB, GPU_MODULATE);
        C3D_TexEnvScale(e0, C3D_RGB, GPU_TEVSCALE_2);
        if (kind == K_TEX && layer) { /* (x 2: texels with bit 15, drawn opaque here, get alpha 1 too) */
            C3D_TexEnvSrc(e0, C3D_Alpha, GPU_TEXTURE0, 0, 0);
            C3D_TexEnvFunc(e0, C3D_Alpha, GPU_REPLACE);
            C3D_TexEnvScale(e0, C3D_Alpha, GPU_TEVSCALE_2);
        } else if (kind == K_TEX) {
            /* min(1, 2a) x (1 - a + 1/8) x 2: 0 for a transparent texel, 1 for one with bit 15 (a = 0x80), 0.25 for one
             * without (a = 1): drawn (alpha test > 0), and its alpha is the mask bit the mirror stores (0.25 -> 0) */
            C3D_TexEnvSrc(e0, C3D_Alpha, GPU_TEXTURE0, 0, 0);
            C3D_TexEnvFunc(e0, C3D_Alpha, GPU_REPLACE);
            C3D_TexEnvScale(e0, C3D_Alpha, GPU_TEVSCALE_2);
            C3D_TexEnvSrc(e1, C3D_RGB, GPU_PREVIOUS, 0, 0);
            C3D_TexEnvFunc(e1, C3D_RGB, GPU_REPLACE);
            C3D_TexEnvSrc(e1, C3D_Alpha, GPU_TEXTURE0, GPU_CONSTANT, GPU_PREVIOUS);
            C3D_TexEnvOpAlpha(e1, GPU_TEVOP_A_ONE_MINUS_SRC_ALPHA, GPU_TEVOP_A_SRC_ALPHA, GPU_TEVOP_A_SRC_ALPHA);
            C3D_TexEnvFunc(e1, C3D_Alpha, GPU_ADD_MULTIPLY);
            C3D_TexEnvScale(e1, C3D_Alpha, GPU_TEVSCALE_2);
            C3D_TexEnvColor(e1, 0x20000000);
        } else if (kind == K_TEX_P1) {
            /* opaque texels only: (a - 0x80) x 2 is ~1 for them, ~0 for semi-transparent ones */
            C3D_TexEnvSrc(e0, C3D_Alpha, GPU_TEXTURE0, GPU_CONSTANT, 0);
            C3D_TexEnvFunc(e0, C3D_Alpha, GPU_SUBTRACT);
            C3D_TexEnvScale(e0, C3D_Alpha, GPU_TEVSCALE_2);
            C3D_TexEnvColor(e0, 0x80000000);
        } else {
            /* semi-transparent texels only: min(1, 2a) x (1 - a) x 2 is ~1 for a = 0x80, 0 for 0 and 0xFF */
            C3D_TexEnvSrc(e0, C3D_Alpha, GPU_TEXTURE0, 0, 0);
            C3D_TexEnvFunc(e0, C3D_Alpha, GPU_REPLACE);
            C3D_TexEnvScale(e0, C3D_Alpha, GPU_TEVSCALE_2);
            C3D_TexEnvSrc(e1, C3D_RGB, GPU_PREVIOUS, 0, 0);
            C3D_TexEnvFunc(e1, C3D_RGB, GPU_REPLACE);
            C3D_TexEnvSrc(e1, C3D_Alpha, GPU_PREVIOUS, GPU_TEXTURE0, 0);
            C3D_TexEnvOpAlpha(e1, GPU_TEVOP_A_SRC_ALPHA, GPU_TEVOP_A_ONE_MINUS_SRC_ALPHA, 0);
            C3D_TexEnvFunc(e1, C3D_Alpha, GPU_MODULATE);
            C3D_TexEnvScale(e1, C3D_Alpha, GPU_TEVSCALE_2);
        }
        break;
    }
    if (kind == K_TEX) C3D_AlphaTest(true, GPU_GREATER, 0);
    else if (kind == K_TEX_P1 || kind == K_TEX_P2) C3D_AlphaTest(true, GPU_GREATER, 0x7F);
    else C3D_AlphaTest(false, GPU_ALWAYS, 0);
}

/* The PS1's modes. In a UI layer (layer != 0) the alpha channel tracks how much of the scene, composited under the
 * layer later, still shows (premultiplied: shown = layer + scene x (1 - alpha)): an opaque pixel sets it to 1, mode 0
 * averages it with 1 (half the scene shows), modes 1-3 leave it (the scene shows fully, the layer adds to it). Over
 * the scene that is exactly the PS1's result for modes 0, 1 and 3; mode 2 (subtract) has nothing to subtract from. */
static void set_blend_mask(int abr, int layer, int mask, int kind);
static void set_blend(int abr, int layer) { set_blend_mask(abr, layer, MASK_COPY, K_BLIT); }
/* The PS1's modes, and in the mirror its mask bit as the alpha bit: written as set_mask | (textured ? the texel's
 * bit 15 : 0) (the combiners give 1 for a texel with bit 15 and 0.25 for one without, which stores as 0; the
 * opaque-texel pass of a semi-transparent primitive draws only texels without it, the other only texels with it), an
 * upload's copied; with check_mask a pixel with it is left alone (by its alpha). The blend colour's alpha is 1:
 * set_mask writes it. */
static void set_blend_mask(int abr, int layer, int mask, int kind) {
    GPU_BLENDFACTOR sa = GPU_ONE, da = GPU_ZERO;
    if (layer) {
        if (abr == 0) sa = GPU_CONSTANT_ALPHA, da = GPU_ONE_MINUS_CONSTANT_ALPHA;
        else if (abr != NO_BLEND) sa = GPU_ZERO, da = GPU_ONE;
    } else {
        int check = mask & MASK_CHECK;
        if (mask & MASK_COPY) sa = GPU_ONE, da = GPU_ZERO;
        else if (mask & MASK_SET) sa = GPU_CONSTANT_ALPHA, da = GPU_ZERO;           /* 1 (masked or not) */
        else if (kind == K_TEX || kind == K_TEX_P2) sa = check ? GPU_ONE_MINUS_DST_ALPHA : GPU_ONE, da = check ? GPU_ONE : GPU_ZERO;
        else sa = GPU_ZERO, da = check ? GPU_ONE : GPU_ZERO;                         /* 0, or kept where masked */
        if (abr == NO_BLEND && check) { /* (opaque draws only: masked pixels keep their colour) */
            C3D_BlendingColor(0xFF000000);
            C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE_MINUS_DST_ALPHA, GPU_DST_ALPHA, sa, da);
            return;
        }
    }
    switch (abr) {
    case 0: /* B/2 + F/2 */
        C3D_BlendingColor(layer ? 0x80808080 : 0xFF808080);
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_CONSTANT_COLOR, GPU_ONE_MINUS_CONSTANT_COLOR, sa, da);
        break;
    case 1: /* B + F */
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ONE, sa, da);
        break;
    case 2: /* B - F */
        C3D_AlphaBlend(GPU_BLEND_REVERSE_SUBTRACT, GPU_BLEND_ADD, GPU_ONE, GPU_ONE, sa, da);
        break;
    case 3: /* B + F/4 */
        C3D_BlendingColor(layer ? 0x40404040 : 0xFF404040);
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_CONSTANT_COLOR, GPU_ONE, sa, da);
        break;
    case 5: /* compositing a UI layer: premultiplied over */
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ONE_MINUS_SRC_ALPHA, GPU_ONE, GPU_ZERO);
        break;
    default:
        C3D_BlendingColor(0xFF000000);
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, layer ? GPU_ONE : sa, layer ? GPU_ZERO : da);
        break;
    }
}

static void set_proj_ortho(float w, float h, float zplane) {
    C3D_Mtx m;
    Mtx_Zeros(&m);
    m.r[0].x = 2.0f / w; m.r[0].w = -1.0f;
    m.r[1].y = -2.0f / h; m.r[1].w = 1.0f; /* (a render target's row 0 is at clip y +1) */
    m.r[2].w = zplane;
    m.r[3].w = 1.0f;
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, u_proj, &m);
}

static void set_texscale(const C3D_Tex *t) {
    /* (texture memory runs top-down, t bottom-up: a texel row r is at t = 1 - r / height) */
    C3D_FVUnifSet(GPU_VERTEX_SHADER, u_texscale, t ? 1.0f / t->width : 0.0f, t ? -1.0f / t->height : 0.0f, 0, 1);
}

/* Replay the recorded batches into each eye's mirror. */
static void submit(void) {
    if (!nbatches) return;
    unsigned profile_start = port_profile_begin();
    frame_begin();
    /* the GPU reads main memory, not the CPU's data cache */
    GSPGPU_FlushDataCache(vb + vb_submitted, (vb_used - vb_submitted) * sizeof(Vtx));
    unsigned any_ui = 0;
    for (unsigned i = 0; i < nbatches; i++) any_ui |= 1u << batches[i].target;
    /* passes: each eye's mirror, then each UI layer (no stereo: the bottom screen is flat) */
    for (int pass = 0; pass < 4; pass++) {
        int eye = pass == 1, target = pass >= 2 ? pass - 1 : 0;
        if (pass == 1 && !stereo_on) continue;
        if (target && !(any_ui & (1u << target))) continue;
        int height = target ? UI_H : VH;
        if (target) {
            C3D_FrameDrawOn(layers[target - 1].rt);
            set_proj_ortho(UI_W, UI_H, 0.0f);
            C3D_FVUnifSet(GPU_VERTEX_SHADER, u_stereo, 0, stereo_inv_zc, 1.0f / 4096.0f, -0.5f);
        } else {
            C3D_FrameDrawOn(mirror_rt[eye]);
            set_proj_ortho(VW, VH, 0.0f);
            float k = eye ? stereo_k : -stereo_k;
            C3D_FVUnifSet(GPU_VERTEX_SHADER, u_stereo, k, stereo_inv_zc, 1.0f / 4096.0f, -0.5f);
        }
        int kind = -1, abr = -1, bmask = -1;
        C3D_Tex *tex = (C3D_Tex *)-1;
        u16 sc[4] = {0xFFFF, 0, 0, 0};
        for (unsigned i = 0; i < nbatches; i++) {
            Batch *b = &batches[i];
            if (b->target != target) continue;
            if (!target && !(b->eyes & (1 << eye))) continue;
            if (b->abr != abr || b->mask != bmask || b->kind != kind) {
                if (b->kind != kind) set_tev_for(kind = b->kind, target != 0);
                set_blend_mask(abr = b->abr, target != 0, bmask = b->mask, kind);
            }
            if (b->tex != tex) {
                tex = b->tex;
                if (tex) C3D_TexBind(0, tex);
                set_texscale(tex);
            }
            if (memcmp(sc, b->sc, sizeof sc)) {
                memcpy(sc, b->sc, sizeof sc);
                /* (in window coordinates, which run bottom-up) */
                C3D_SetScissor(GPU_SCISSOR_NORMAL, sc[0], height - sc[3], sc[2], height - sc[1]);
            }
            C3D_DrawArrays(GPU_TRIANGLES, b->first, b->count);
        }
    }
    ctr_render_stats[0] += nbatches;
    nbatches = 0;
    vb_submitted = vb_used;
    port_profile_end(PORT_PROFILE_GPU, profile_start);
}

/* ---- CPU <-> mirror transfers ------------------------------------------------------------------------------ */

/* Morton order inside an 8x8 tile, for x and y apart */
static u8 morton_x[8], morton_y[8];
static inline unsigned tiled(unsigned x, unsigned y, unsigned width) {
    return ((y >> 3) * (width >> 3) + (x >> 3)) * 64 + (morton_x[x & 7] | morton_y[y & 7]);
}

/* The mirror is RGBA5551: the PS1's 15-bit colour, and its mask bit (bit 15) as the alpha bit. */
static inline u16 bgr555_to_rgba5551(u16 c) {
    unsigned r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    return (u16)(r << 11 | g << 6 | b << 1 | c >> 15);
}
static inline u16 rgba5551_to_bgr555(u16 p) {
    return (u16)((p >> 11) | ((p >> 6) & 31) << 5 | ((p >> 1) & 31) << 10 | (p & 1) << 15);
}

static inline unsigned pow2_at_least(unsigned v) {
    unsigned p = 8;
    while (p < v) p <<= 1;
    return p;
}

static void *staging_alloc(unsigned bytes) {
    bytes = (bytes + 127) & ~127u;
    if (staging_used + bytes > STAGING_SIZE) gpu_sync();
    void *p = staging + staging_used;
    staging_used += bytes;
    return p;
}

/* a staging texture over `data` (from staging_alloc), valid until the C3D frame ends */
static C3D_Tex *staging_tex(void *data, unsigned w, unsigned h, GPU_TEXCOLOR fmt) {
    static C3D_Tex pool[512];
    static unsigned next, serial;
    if (serial != frame_serial) { serial = frame_serial; next = 0; }
    if (next == 512) gpu_sync(), serial = frame_serial, next = 0;
    C3D_Tex *t = &pool[next++];
    memset(t, 0, sizeof *t);
    t->data = data;
    t->fmt = fmt;
    t->width = w;
    t->height = h;
    t->size = w * h * (fmt == GPU_RGB8 ? 3 : 2);
    t->param = GPU_TEXTURE_MAG_FILTER(GPU_NEAREST) | GPU_TEXTURE_MIN_FILTER(GPU_NEAREST) |
               GPU_TEXTURE_WRAP_S(GPU_CLAMP_TO_EDGE) | GPU_TEXTURE_WRAP_T(GPU_CLAMP_TO_EDGE);
    return t;
}

static const u16 full_scissor[4] = {0, 0, VW, VH};

static void quad(Vtx *v, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, u32 rgba) {
    Vtx a = {x0, y0, 0, u0, v0, rgba}, b = {x1, y0, 0, u1, v0, rgba}, c = {x0, y1, 0, u0, v1, rgba},
        d = {x1, y1, 0, u1, v1, rgba};
    v[0] = a; v[1] = b; v[2] = c;
    v[3] = b; v[4] = d; v[5] = c;
}

/* Copy the shadow's pixels of the cells of [cx0,cx1)x[cy0,cy1) whose mirror is stale into the mirror, in drawing order:
 * only those (a cell the GPU drew since has a stale shadow), as runs along each row of cells (pieces of at most 256
 * pixels). */
static void blit_run(int cx0, int cx1, int cy) {
    for (int bx = cx0 * CELL; bx < cx1 * CELL; bx += 256) {
        int w = cx1 * CELL - bx, h = CELL, by = cy * CELL;
        if (w > 256) w = 256;
        unsigned tw = pow2_at_least(w), th = pow2_at_least(h);
        u16 *px = staging_alloc(tw * th * 2);
        for (int y = 0; y < h; y++) {
            const u16 *src = psx_vram + (by + y) * VW + bx;
            for (int x = 0; x < w; x++) px[tiled(x, y, tw)] = bgr555_to_rgba5551(src[x]);
        }
        GSPGPU_FlushDataCache(px, tw * th * 2);
        C3D_Tex *t = staging_tex(px, tw, th, GPU_RGBA5551);
        Vtx *v = alloc_vtx(6);
        quad(v, bx, by, bx + w, by + h, 0, 0, w, h, 0xFFFFFFFF);
        int saved = cur_target, saved_mask = cur_mask;
        cur_target = 0;       /* (into the mirror, whatever was drawn last) */
        cur_mask = MASK_COPY; /* (the shadow's pixels with their mask bits) */
        add_batch(K_BLIT, NO_BLEND, 3, full_scissor, t, v - vb, 6);
        cur_target = saved;
        cur_mask = saved_mask;
        ctr_render_stats[2]++;
    }
}
static void blit_cells(int cx0, int cy0, int cx1, int cy1) {
    if (!psx_vram) return;
    for (int cy = cy0; cy < cy1; cy++)
        for (int cx = cx0; cx < cx1;) {
            if (mirror_ok[cy][cx]) { cx++; continue; }
            int run = cx;
            while (run < cx1 && !mirror_ok[cy][run]) mirror_ok[cy][run++] = 1;
            blit_run(cx, run, cy);
            cx = run;
        }
}

/* Read cells [cx0,cx1)x[cy0,cy1) whose shadow is stale back from the left eye's mirror. */
static void readback_cells(int cx0, int cy0, int cx1, int cy1) {
    gpu_sync();
    const u16 *m = mirror_tex[0].data;
    for (int cy = cy0; cy < cy1; cy++)
        for (int cx = cx0; cx < cx1; cx++) {
            if (shadow_ok[cy][cx]) continue;
            for (int y = cy * CELL; y < (cy + 1) * CELL; y++) {
                u16 *dst = psx_vram + y * VW;
                for (int x = cx * CELL; x < (cx + 1) * CELL; x++) dst[x] = rgba5551_to_bgr555(m[tiled(x, y, VW)]);
            }
            shadow_ok[cy][cx] = 1;
        }
    ctr_render_stats[3]++;
}

/* gpu.c: a fill (ClearImage, a GP0 fill command): drawn by the GPU too, so the cells it covers stay current there */
void psx_vram_filled(int x, int y, int w, int h, unsigned int rgb15) {
    if (w <= 0 || h <= 0) return;
    int x1 = x + w > VW ? VW : x + w, y1 = y + h > VH ? VH : y + h;
    gpu_draws(x, y, x1, y1); /* (its partly covered cells must be current in the mirror first) */
    unsigned r = (rgb15 & 31) << 3, g = ((rgb15 >> 5) & 31) << 3, b = ((rgb15 >> 10) & 31) << 3;
    Vtx *v = alloc_vtx(6);
    quad(v, x, y, x1, y1, 0, 0, 0, 0, 0xFF000000 | b << 16 | g << 8 | r);
    int saved = cur_target, saved_mask = cur_mask;
    cur_target = 0; /* (a fill is VRAM's: the mirror, even right after a UI layer's primitives) */
    cur_mask = 0;   /* (a fill clears the mask bit, and ignores it) */
    add_batch(K_UNTEX, NO_BLEND, 3, full_scissor, NULL, v - vb, 6);
    cur_target = saved;
    cur_mask = saved_mask;
    filling = 1;
}

/* ---- textures ----------------------------------------------------------------------------------------------- */

typedef struct {
    u32 key;     /* depth << 30 | page x / 64 << 26 | page y / 256 << 25 | CLUT x / 16 << 9 | CLUT y; 0 = free */
    u32 twin;    /* the texture window baked in (GP0 E2: mask x, y, offset x, y in 8-texel units), 0 = none */
    u32 serial;  /* frame_serial when last drawn with */
    C3D_Tex tex;
    u32 stamp[64]; /* per 32x32 block: gen_counter when converted (0 = not yet) */
    u32 epoch[64]; /* per block: tex_epoch when it was last known current (no write to any source cell since) */
    u8 flags[64];  /* per block: 1 = has opaque texels, 2 = has semi-transparent ones */
    u32 hash[64];  /* per block: its source texels' and CLUT's hash when converted */
} TexSlot;

#define MAX_SLOTS 96
static TexSlot slots[MAX_SLOTS];
static int nslots;
static u32 rgba_lut[65536];

/* (key, twin) -> slot + 1, open addressing; a slot's entry is removed when it is reused */
#define SLOT_HASH 512
static u16 slot_hash[SLOT_HASH];
static inline unsigned hash_of(u32 key, u32 twin) { return ((key ^ twin * 0x9E3779B1u) * 0x85EBCA6Bu) >> 23; }
static void hash_remove(int i) {
    unsigned h = hash_of(slots[i].key, slots[i].twin);
    while (slot_hash[h] && slot_hash[h] != i + 1) h = (h + 1) & (SLOT_HASH - 1);
    if (!slot_hash[h]) return;
    slot_hash[h] = 0;
    /* (re-insert the run after it, which may have probed past it) */
    for (h = (h + 1) & (SLOT_HASH - 1); slot_hash[h]; h = (h + 1) & (SLOT_HASH - 1)) {
        int j = slot_hash[h] - 1;
        slot_hash[h] = 0;
        unsigned k = hash_of(slots[j].key, slots[j].twin);
        while (slot_hash[k]) k = (k + 1) & (SLOT_HASH - 1);
        slot_hash[k] = (u16)(j + 1);
    }
}
static TexSlot *slot_for(u32 key, u32 twin) {
    static int last = -1;
    if (last >= 0 && slots[last].key == key && slots[last].twin == twin) return &slots[last];
    for (unsigned h = hash_of(key, twin); slot_hash[h]; h = (h + 1) & (SLOT_HASH - 1)) {
        int i = slot_hash[h] - 1;
        if (slots[i].key == key && slots[i].twin == twin) return &slots[last = i];
    }
    int victim = -1;
    for (int i = 0; i < nslots; i++) {
        if (slots[i].serial != frame_serial && (victim < 0 || slots[i].serial < slots[victim].serial)) victim = i;
    }
    if (nslots < MAX_SLOTS && C3D_TexInit(&slots[nslots].tex, 256, 256, GPU_RGBA8)) {
        victim = nslots++;
        C3D_TexSetFilter(&slots[victim].tex, GPU_NEAREST, GPU_NEAREST);
        C3D_TexSetWrap(&slots[victim].tex, GPU_REPEAT, GPU_REPEAT);
        ctr_render_stats[5] = nslots;
    } else if (victim < 0) {
        gpu_sync(); /* every slot is in this frame's batches */
        victim = 0;
    }
    if (slots[victim].key) hash_remove(victim);
    TexSlot *s = &slots[last = victim];
    s->key = key;
    s->twin = twin;
    memset(s->epoch, 0, sizeof s->epoch);
    unsigned h = hash_of(key, twin);
    while (slot_hash[h]) h = (h + 1) & (SLOT_HASH - 1);
    slot_hash[h] = (u16)(victim + 1);
    memset(s->stamp, 0, sizeof s->stamp);
    return s;
}

static inline int cells_newer(int x0, int y0, int x1, int y1, u32 stamp) {
    for (int cy = y0 / CELL; cy <= (y1 - 1) / CELL; cy++)
        for (int cx = x0 / CELL; cx <= (x1 - 1) / CELL; cx++)
            if (cell_gen[cy & (CH - 1)][cx & (CW - 1)] > stamp) return 1;
    return 0;
}

static void mark_source(int x0, int y0, int x1, int y1) {
    for (int cy = y0 / CELL; cy <= (y1 - 1) / CELL; cy++)
        for (int cx = x0 / CELL; cx <= (x1 - 1) / CELL; cx++) cell_is_source[cy & (CH - 1)][cx & (CW - 1)] = 1;
}

/* a hash of w x h halfwords of the shadow at (x, y) (a rectangle within one VRAM row band; x even or not) */
static u32 shadow_hash(int x, int y, int w, int h, u32 seed) {
    u32 hsh = seed;
    for (int j = 0; j < h; j++) {
        const u16 *row = psx_vram + ((y + j) & 511) * VW;
        for (int i = 0; i < w; i++) hsh = (hsh ^ row[(x + i) & 1023]) * 0x01000193u;
    }
    return hsh;
}

/* Make block (bx, by) of the page's texture current. */
static void convert_block(TexSlot *s, const Gp0Mode *m, int bx, int by) {
    int b = by * 8 + bx;
    int shift = m->depth == 0 ? 2 : m->depth == 1 ? 1 : 0;
    int sx0 = m->tx + ((bx * 32) >> shift), sw = 32 >> shift, sy0 = m->ty + by * 32;
    /* a texture window reads only inside it: check (and read back) the whole page instead of the block */
    const unsigned mx = (s->twin & 31) * 8, my = (s->twin >> 5 & 31) * 8, ofx = (s->twin >> 10 & 31) * 8,
                   ofy = (s->twin >> 15 & 31) * 8;
    if (s->twin) sx0 = m->tx, sw = 256 >> shift, sy0 = m->ty;
    int ncl = m->depth == 0 ? 16 : m->depth == 1 ? 256 : 0;
    if (s->stamp[b] && s->epoch[b] == tex_epoch) return;
    if (s->stamp[b] && !cells_newer(sx0, sy0, sx0 + sw, sy0 + 32, s->stamp[b]) &&
        (!ncl || !cells_newer(m->clut_x, m->clut_y, m->clut_x + ncl, m->clut_y + 1, s->stamp[b]))) {
        s->epoch[b] = tex_epoch;
        return;
    }
    cpu_reads(sx0, sy0, sx0 + sw, sy0 + 32);
    if (ncl) cpu_reads(m->clut_x, m->clut_y, m->clut_x + ncl, m->clut_y + 1);
    /* a write near its sources (the 32x32 cells are coarse: the world map scrolls 16-wide strips of water and cycles
     * single CLUT rows every frame) may have left them as they were: then the block stays */
    u32 hsh = shadow_hash(sx0, sy0, sw, 32, 0x811C9DC5u);
    if (ncl) hsh = shadow_hash(m->clut_x, m->clut_y, ncl, 1, hsh);
    if (s->stamp[b] && s->hash[b] == hsh) {
        s->stamp[b] = gen_counter;
        s->epoch[b] = tex_epoch;
        return;
    }
    if (s->serial == frame_serial) gpu_sync(); /* drawn with earlier in this frame: those draws keep the old texels */
    s->hash[b] = hsh;
    u32 clut[256];
    if (ncl) {
        const u16 *c = psx_vram + (m->clut_y & 511) * VW;
        for (int i = 0; i < ncl; i++) clut[i] = rgba_lut[c[(m->clut_x + i) & 1023]];
    }
    u32 *dst = s->tex.data;
    unsigned flags = 0;
    for (int y = 0; y < 32; y++) {
        int v = by * 32 + y, sv = (int)(((unsigned)v & ~my) | (ofy & my));
        const u16 *row = psx_vram + ((m->ty + sv) & 511) * VW;
        for (int x = 0; x < 32; x++) {
            int u = bx * 32 + x, su = (int)(((unsigned)u & ~mx) | (ofx & mx));
            u32 c;
            if (m->depth == 0) c = clut[(row[(m->tx + (su >> 2)) & 1023] >> ((su & 3) * 4)) & 15];
            else if (m->depth == 1) c = clut[(row[(m->tx + (su >> 1)) & 1023] >> ((su & 1) * 8)) & 255];
            else c = rgba_lut[row[(m->tx + su) & 1023]];
            unsigned a = c & 0xFF;
            flags |= a == 0xFF ? 1 : a == 0x80 ? 2 : 0;
            dst[tiled(u, v, 256)] = c;
        }
    }
    s->flags[b] = flags;
    s->stamp[b] = gen_counter;
    s->epoch[b] = tex_epoch;
    mark_source(sx0, sy0, sx0 + sw, sy0 + 32);
    if (ncl) mark_source(m->clut_x, m->clut_y, m->clut_x + ncl, m->clut_y + 1);

    /* (the block's four rows of tiles: 32 rows of the texture, contiguous) */
    GSPGPU_FlushDataCache(dst + by * 32 * 256, 32 * 256 * 4);
    ctr_render_stats[4]++;
}

/* The texture for a textured primitive whose texels span [u0,u1]x[v0,v1] (inclusive, 0..255; u1 < u0 when it
 * wraps), with its blocks current; *flags = the union of their texel kinds. */
static C3D_Tex *texture_for(const Gp0Mode *m, int u0, int v0, int u1, int v1, unsigned *flags) {
    u32 key = (u32)m->depth << 30 | (u32)(m->tx >> 6) << 26 | (u32)(m->ty >> 8) << 25 |
              (m->depth < 2 ? (u32)(m->clut_x >> 4) << 9 | (u32)m->clut_y : 0) | 1u << 24;
    u32 twin = (gp0_state.twin_mask_x / 8) | (gp0_state.twin_mask_y / 8) << 5 | (gp0_state.twin_off_x / 8) << 10 |
               (gp0_state.twin_off_y / 8) << 15;
    TexSlot *s = slot_for(key, twin);
    int bx0 = u0 >> 5, bx1 = u1 >> 5, by0 = v0 >> 5, by1 = v1 >> 5;
    if (bx1 < bx0) bx0 = 0, bx1 = 7;
    if (by1 < by0) by0 = 0, by1 = 7;
    unsigned f = 0;
    for (int by = by0; by <= by1; by++)
        for (int bx = bx0; bx <= bx1; bx++) {
            convert_block(s, m, bx, by);
            f |= s->flags[by * 8 + bx];
        }
    s->serial = frame_serial;
    *flags = f;
    return &s->tex;
}

/* ---- primitives --------------------------------------------------------------------------------------------- */

static int area_scissor(u16 sc[4]) {
    sc[0] = (u16)gp0_state.area_x0;
    sc[1] = (u16)gp0_state.area_y0;
    sc[2] = (u16)(gp0_state.area_x1 + 1);
    sc[3] = (u16)(gp0_state.area_y1 + 1);
    return sc[2] > sc[0] && sc[3] > sc[1];
}

static inline u32 vcolor(const Gp0Mode *m, const Gp0Vtx *v) {
    if (m->textured && m->raw) return 0xFF808080;
    return 0xFF000000u | (u32)v->b << 16 | (u32)v->g << 8 | (u32)v->r;
}

static inline void make_vtx(Vtx *o, const Gp0Mode *m, const Gp0Vtx *v) {
    o->x = (float)v->x;
    o->y = (float)v->y;
    o->z = 0;
    u32 info = v->xyw && !getenv_nodepth ? psx_depth_lookup(v->xyw) : 0;
    ctr_depth_stats[info ? 0 : 1]++;
    if (info) {
        o->x += (info & 0xFF) * (1.0f / 256);
        o->y += ((info >> 8) & 0xFF) * (1.0f / 256);
        o->z = (float)(info >> 16);
    }
    o->u = (float)v->u;
    o->v = (float)v->v;
    o->rgba = vcolor(m, v);
}

/* the vertices of one primitive (6 for a quad, 3 for a triangle) in each pass its kind needs */
/* ---- depth for 2D primitives from their ordering-table entry ----------------------------------------------
 * The game sorts its scene into ordering-table entries by depth, so an entry stands for a depth. Primitives the GTE
 * projected give (entry, SZ) pairs; a least-squares line through a frame's pairs gives the depth of the entries
 * where 2D primitives hang (a field's background layers, sprites), and with it their stereo parallax. Full-screen
 * ones (fades, letterboxes) stay at the screen plane. */
extern int psx_ot_entry;
static float fit_n, fit_s, fit_z, fit_ss, fit_sz; /* this frame's sums */
static float fit_a, fit_b;                        /* the last good line: SZ = a entry + b */
static unsigned fit_age = ~0u;                    /* presents since it was fitted */

static float zc_smooth = 1500.0f; /* the convergence depth: the scene's mean depth, smoothed */
static void fit_frame(void) {
    if (fit_n >= 16) {
        zc_smooth += (fit_z / fit_n - zc_smooth) * 0.1f;
        stereo_inv_zc = 1.0f / zc_smooth;
        float den = fit_n * fit_ss - fit_s * fit_s;
        if (den > 1e-3f) {
            fit_a = (fit_n * fit_sz - fit_s * fit_z) / den;
            fit_b = (fit_z - fit_a * fit_s) / fit_n;
            fit_age = 0;
        }
    }
    if (fit_age != ~0u) fit_age++;
    fit_n = fit_s = fit_z = fit_ss = fit_sz = 0;
}

static const Vtx *ot_depth(const Vtx *v, unsigned n, Vtx *out, int w, int h) {
    if (psx_ot_entry < 0) return v;
    float s = (float)psx_ot_entry, z = 0;
    for (unsigned i = 0; i < n; i++) z += v[i].z;
    if (z > 0) { /* projected: a sample */
        z /= n;
        fit_n += 1; fit_s += s; fit_z += z; fit_ss += s * s; fit_sz += s * z;
        return v;
    }
    if (fit_age > 30 || (w >= 300 && h >= 200)) return v;
    float d = fit_a * s + fit_b;
    if (d < 64) return v;
    for (unsigned i = 0; i < n; i++) out[i] = v[i], out[i].z = d;
    return out;
}

static int ui_ox, ui_oy; /* the displayed buffer's origin, which the UI list draws at */
static int ui_list_layer;  /* the layer the UI list draws into */
static int ui_layer_for(int x, int y, int clear);
static int ui_packet(void);
static void emit(const Gp0Mode *m, const Vtx *v, unsigned n, int bx0, int by0, int bx1, int by1, C3D_Tex *tex,
                 unsigned tflags) {
    u16 sc[4];
    if (!area_scissor(sc)) return;
    if (bx0 < sc[0]) bx0 = sc[0];
    if (by0 < sc[1]) by0 = sc[1];
    if (bx1 > sc[2]) bx1 = sc[2];
    if (by1 > sc[3]) by1 = sc[3];
    if (bx0 >= bx1 || by0 >= by1) return;
    Vtx moved[6];
    int layer = -1, ox = 0, oy = 0;
    if (ui_mode) layer = ui_list_layer, ox = ui_ox, oy = ui_oy;
    else if (menu_state == 2 || ui_packet()) { /* (a menu: the whole frame) */
        int x, y, w, h;
        psx_gpu_draw_area(&x, &y, &w, &h);
        layer = ui_layer_for(x, y, 0);
        ox = x;
        oy = y;
    }
    if (layer >= 0) { /* into a UI layer, relative to its framebuffer; the mirror isn't touched */
        for (unsigned i = 0; i < n; i++) moved[i] = v[i], moved[i].x -= ox, moved[i].y -= oy;
        v = moved;
        sc[0] = (u16)clampi(sc[0] - ox, 0, UI_W); sc[2] = (u16)clampi(sc[2] - ox, 0, UI_W);
        sc[1] = (u16)clampi(sc[1] - oy, 0, UI_H); sc[3] = (u16)clampi(sc[3] - oy, 0, UI_H);
        if (sc[0] >= sc[2] || sc[1] >= sc[3]) return;
        layers[layer].drawn = frames_drawn;
        cur_target = layer + 1;
    } else {
        gpu_draws(bx0, by0, bx1, by1);
        cur_target = 0;
        v = ot_depth(v, n, moved, bx1 - bx0, by1 - by0);
    }
    cur_mask = (gp0_state.set_mask ? MASK_SET : 0) | (gp0_state.check_mask ? MASK_CHECK : 0);
    ctr_render_stats[1]++;
    int passes[2], np = 0, abr = m->semi ? m->abr : NO_BLEND;
    if (!m->textured) passes[np++] = K_UNTEX;
    else if (!m->semi) passes[np++] = K_TEX;
    else {
        if (tflags & 1) passes[np++] = K_TEX_P1;
        if (tflags & 2) passes[np++] = K_TEX_P2;
    }
    for (int p = 0; p < np; p++) {
        Vtx *o = alloc_vtx(n);
        memcpy(o, v, n * sizeof *v);
        add_batch(passes[p], passes[p] == K_TEX_P1 ? NO_BLEND : abr, 3, sc, tex, o - vb, n);
    }
}

static inline int imin(int a, int b) { return a < b ? a : b; }
static inline int imax(int a, int b) { return a > b ? a : b; }

static C3D_Tex *prim_texture(const Gp0Mode *m, const Gp0Vtx *v, int n, unsigned *tflags) {
    if (!m->textured) return NULL;
    int u0 = 255, u1 = 0, v0 = 255, v1 = 0;
    for (int i = 0; i < n; i++) {
        u0 = imin(u0, v[i].u); u1 = imax(u1, v[i].u);
        v0 = imin(v0, v[i].v); v1 = imax(v1, v[i].v);
    }
    return texture_for(m, u0, v0, u1, v1, tflags);
}

static void r_poly(const Gp0Mode *m, const Gp0Vtx *v, int n) {
    unsigned tflags = 0;
    C3D_Tex *tex = prim_texture(m, v, n, &tflags);
    Vtx t[6];
    int with_depth = 0;
    for (int i = 0; i < n; i++) make_vtx(&t[i], m, &v[i]), with_depth += t[i].z > 0;
    /* depth for all its vertices or none: a vertex the GTE clamped (near the camera, or far off screen) has none, and
     * mixing w = 1 with w ~ Z would warp the texture across the polygon */
    if (with_depth && with_depth < n)
        for (int i = 0; i < n; i++) t[i].z = 0;
    /* The PS1 draws a quad as triangles (0,1,2) and (1,3,2), and skips a triangle 1024 or more wide or 512 or more
     * high on its own: of a quad reaching far off the screen, the half on it is still drawn. */
    static const u8 tri[2][3] = {{0, 1, 2}, {1, 3, 2}};
    Vtx out[6];
    unsigned nout = 0;
    int x0 = 1 << 30, y0 = 1 << 30, x1 = -(1 << 30), y1 = -(1 << 30);
    for (int k = 0; k < (n == 4 ? 2 : 1); k++) {
        int tx0 = 1 << 30, ty0 = 1 << 30, tx1 = -(1 << 30), ty1 = -(1 << 30);
        for (int i = 0; i < 3; i++) {
            const Gp0Vtx *p = &v[tri[k][i]];
            tx0 = imin(tx0, p->x); tx1 = imax(tx1, p->x);
            ty0 = imin(ty0, p->y); ty1 = imax(ty1, p->y);
        }
        if (tx1 - tx0 > 1023 || ty1 - ty0 > 511) continue;
        for (int i = 0; i < 3; i++) out[nout++] = t[tri[k][i]];
        x0 = imin(x0, tx0); x1 = imax(x1, tx1);
        y0 = imin(y0, ty0); y1 = imax(y1, ty1);
    }
    if (!nout) return;
#ifdef PORT_LOGUI_AT
    if (ui_mode && frames_drawn == PORT_LOGUI_AT)
        port_log("ui poly n%d tex%d semi%d abr%d gour%d %d,%d %d,%d %d,%d rgb %02x%02x%02x %02x%02x%02x\n", n, m->textured,
                 m->semi, m->abr, m->gouraud, v[0].x - ui_ox, v[0].y - ui_oy, v[1].x - ui_ox, v[1].y - ui_oy,
                 v[2].x - ui_ox, v[2].y - ui_oy, v[0].r, v[0].g, v[0].b, v[1].r, v[1].g, v[1].b);
#endif
#ifdef PORT_PROBE_AT
    { /* PROBE_AT=frame: from that frame on, the first semi-transparent untextured polygons covering (160, 4) */
        extern long ctr_frames(void);
        static int logged;
        int px = gp0_state.area_x0 + 160, py = gp0_state.area_y0 + 4;
        if (ctr_frames() >= PORT_PROBE_AT && !m->textured && x0 <= px && x1 >= px && y0 <= py && y1 >= py &&
            logged++ < 12)
            port_log("probe f%ld: semi %d abr %d %d,%d %d,%d %d,%d %d,%d rgb %02x%02x%02x %02x%02x%02x %02x%02x%02x | out %u\n",
                     ctr_frames(), m->semi, m->abr, v[0].x, v[0].y, v[1].x, v[1].y, v[2].x, v[2].y, n > 3 ? v[3].x : 0,
                     n > 3 ? v[3].y : 0, v[0].r, v[0].g, v[0].b, v[1].r, v[1].g, v[1].b, v[2].r, v[2].g, v[2].b, nout);
    }
#endif
    emit(m, out, nout, x0, y0, x1 + 1, y1 + 1, tex, tflags);
}

static int ui_reads_cache(const Gp0Mode *m, int u0, int v0, int u1, int v1);
static void note_hand(int x, int y);
static void r_rect(const Gp0Mode *m, const Gp0Vtx *v, int w, int h) {
    if (w <= 0 || h <= 0) return;
    if (ui_mode && m->textured && ui_reads_cache(m, v->u, v->v, v->u + w - 1, v->v + h - 1)) return;
    if (m->textured && w == 23 && h == 13 && m->tx == 960 && m->ty == 256 && v->u == 227 && v->v == 21)
        note_hand(v->x, v->y); /* the pointing hand: where the game's cursor is (touch.c) */
#ifdef PORT_LOGUI_AT
    if (ui_mode && frames_drawn == PORT_LOGUI_AT)
        port_log("ui rect %d,%d %dx%d tex %d uv %d,%d page %d,%d clut %d,%d col %02x%02x%02x\n", v->x - ui_ox, v->y - ui_oy,
                 w, h, m->textured ? m->depth : -1, v->u, v->v, m->tx, m->ty, m->clut_x, m->clut_y, v->r, v->g, v->b);
#endif
    unsigned tflags = 0;
    C3D_Tex *tex = NULL;
    if (m->textured) {
        int u0 = v->u, v0 = v->v, u1 = (v->u + w - 1) & 255, v1 = (v->v + h - 1) & 255;
        if (w >= 256) u0 = 0, u1 = 255;
        if (h >= 256) v0 = 0, v1 = 255;
        tex = texture_for(m, u0, v0, u1, v1, &tflags);
    }
    Vtx t[6];
    quad(t, v->x, v->y, v->x + w, v->y + h, v->u, v->v, v->u + w, v->v + h, vcolor(m, v));
    emit(m, t, 6, v->x, v->y, v->x + w, v->y + h, tex, tflags);
}

static void r_line(const Gp0Mode *m, const Gp0Vtx *a, const Gp0Vtx *b) {
    Vtx va, vb2;
    make_vtx(&va, m, a);
    make_vtx(&vb2, m, b);
    if ((va.z > 0) != (vb2.z > 0)) va.z = vb2.z = 0;
    float dx = vb2.x - va.x, dy = vb2.y - va.y;
    /* a one-pixel-wide quad along the major axis, covering both end points */
    float ox = 0, oy = 0;
    if ((dx < 0 ? -dx : dx) >= (dy < 0 ? -dy : dy)) oy = 1; else ox = 1;
    Vtx t[6];
    t[0] = va; t[1] = vb2; t[2] = va;
    t[2].x += ox; t[2].y += oy;
    t[3] = vb2; t[4] = vb2; t[5] = t[2];
    t[4].x += ox; t[4].y += oy;
    if (oy) { t[1].x += dx >= 0 ? 1 : -1; t[3].x = t[1].x; t[4].x += dx >= 0 ? 1 : -1; }
    else { t[1].y += dy >= 0 ? 1 : -1; t[3].y = t[1].y; t[4].y += dy >= 0 ? 1 : -1; }
    emit(m, t, 6, imin(a->x, b->x), imin(a->y, b->y), imax(a->x, b->x) + 1, imax(a->y, b->y) + 1, NULL, 0);
}

static const Gp0Backend pica_backend = {r_poly, r_line, r_rect, 0, 0, 0};

/* gpu.c: the end of an ordering table */
void psx_gpu_ot_done(void);

/* ---- the UI layer (the battle UI's inserted list, gpu.c ContinueDraw) ----------------------------------------
 * Drawn over a dimmed copy of the displayed scene, so its translucent windows look as they do over the PS1's scene.
 * The list's row cache (VRAM copies of the scene under changing digits, drawn back before them) restores scene
 * pixels the layer doesn't have: its copies and the sprites reading them are skipped. */
int ctr_ui_hands[CTR_MAX_HANDS][2], ctr_ui_nhands;
static int shown_layer = -1; /* the layer of the displayed buffer, when it has UI */
int ctr_ui_active(void) { return shown_layer >= 0; }
static struct { int x0, y0, x1, y1; } ui_cache[8];
static int n_ui_cache;

/* the layer of the framebuffer at (x, y): taken over from the older one when new (clear: cleared now) */
static int ui_layer_for(int x, int y, int clear) {
    int k = layers[0].x == x && layers[0].y == y ? 0 : layers[1].x == x && layers[1].y == y ? 1 : -1;
    if (k < 0) {
        k = layers[0].drawn <= layers[1].drawn ? 0 : 1;
        layers[k].x = x;
        layers[k].y = y;
        layers[k].nhands = 0;
        clear = 1;
    }
    if (clear || layers[k].shown) {
        static const u16 sc[4] = {0, 0, UI_W, UI_H};
        int saved = cur_target;
        cur_target = k + 1;
        Vtx *v = alloc_vtx(6);
        quad(v, 0, 0, UI_W, UI_H, 0, 0, 0, 0, 0x00000000);
        add_batch(K_UNTEX, NO_BLEND, 1, sc, NULL, v - vb, 6);
        cur_target = saved;
        layers[k].shown = 0;
        layers[k].nhands = 0;
    }
    return k;
}

/* Window-system packets (port/game/ui_split.c): a range lives until an ordering table (any: the battle draws its
 * windows in the VBlank list) has drawn from it, or two presents have gone by, as the game's packet buffers are
 * reused and a stale range would take the scene's packets there (a boss's ceiling went to the bottom screen). */
static struct { const u8 *lo, *hi; int hit, age; } ranges[8];
static int nranges;
void port_ui_packets(const void *lo, const void *hi) {
    for (int i = 0; i < nranges; i++) /* (the buffer was reused: a range there is a past frame's) */
        if ((const u8 *)lo < ranges[i].hi && (const u8 *)hi > ranges[i].lo) ranges[i] = ranges[--nranges], i--;
    if (nranges == 8) nranges--;
    ranges[nranges].lo = lo;
    ranges[nranges].hi = hi;
    ranges[nranges].hit = ranges[nranges].age = 0;
    nranges++;
}
extern const u32 *psx_gpu_packet;
static int ui_packet(void) {
    const u8 *p = (const u8 *)psx_gpu_packet;
    for (int i = 0; i < nranges; i++)
        if (p >= ranges[i].lo && p < ranges[i].hi) return ranges[i].hit = 1;
    return 0;
}
static void ranges_drawn(void) { /* (an ordering table is done: what it drew from is spent) */
    for (int i = 0; i < nranges; i++)
        if (ranges[i].hit) ranges[i] = ranges[--nranges], i--;
}
static void ranges_age(void) {
    for (int i = 0; i < nranges; i++)
        if (++ranges[i].age > 2) ranges[i] = ranges[--nranges], i--;
}

/* gpu.c: an ordering table drawn into a framebuffer starts that frame's UI layer afresh, once it has been shown */
void psx_gpu_ot_begin(const u32 *ot) {
    (void)ot;
    if (ui_mode) return;
    int x, y, w, h;
    psx_gpu_draw_area(&x, &y, &w, &h);
    if (w >= 256 && h >= 192) {
        int k = layers[0].x == x && layers[0].y == y ? 0 : layers[1].x == x && layers[1].y == y ? 1 : -1;
        if (k >= 0 && layers[k].shown) ui_layer_for(x, y, 1);
    }
}

static void note_hand(int x, int y) {
    int k = ui_mode ? ui_list_layer : -1;
    int ox = ui_ox, oy = ui_oy;
    if (k < 0) {
        if (menu_state != 2 && !ui_packet()) return;
        int w, h;
        psx_gpu_draw_area(&ox, &oy, &w, &h);
        k = ui_layer_for(ox, oy, 0);
    }
    if (layers[k].nhands < CTR_MAX_HANDS) {
        layers[k].hands[layers[k].nhands][0] = x - ox;
        layers[k].hands[layers[k].nhands][1] = y - oy;
        layers[k].nhands++;
    }
}

static int ui_reads_cache(const Gp0Mode *m, int u0, int v0, int u1, int v1) {
    int shift = m->depth == 0 ? 2 : m->depth == 1 ? 1 : 0;
    int x0 = m->tx + (u0 >> shift), x1 = m->tx + (u1 >> shift) + 1, y0 = m->ty + v0, y1 = m->ty + v1 + 1;
    for (int i = 0; i < n_ui_cache; i++)
        if (x0 < ui_cache[i].x1 && x1 > ui_cache[i].x0 && y0 < ui_cache[i].y1 && y1 > ui_cache[i].y0) return 1;
    return 0;
}

int psx_ui_copy(int sx, int sy, int dx, int dy, int w, int h) {
    (void)sx; (void)sy;
    if (!ui_mode) return 0;
    if (n_ui_cache < 8) {
        ui_cache[n_ui_cache].x0 = dx; ui_cache[n_ui_cache].y0 = dy;
        ui_cache[n_ui_cache].x1 = dx + w; ui_cache[n_ui_cache].y1 = dy + h;
        n_ui_cache++;
    }
    return 1;
}

/* gpu.c ContinueDraw: the battle UI's list, drawn over the displayed buffer every VBlank: its layer anew each time */
void psx_ui_list_begin(void) {
    submit();
    PsxDisplayInfo d;
    psx_gpu_display_info(&d);
    ui_mode = 1;
    ui_ox = d.x;
    ui_oy = d.y;
    n_ui_cache = 0;
    ui_list_layer = ui_layer_for(d.x, d.y, 1);
}

void psx_ui_list_end(void) {
    submit();
    ui_mode = 0;
}

void psx_gpu_ot_done(void) {
    submit();
    ranges_drawn();
}

/* ---- presenting ------------------------------------------------------------------------------------------ */

/* a textured quad on a screen's render target (already drawn on): texels (sx, sy) w x h at (ox, oy) */
static void screen_quad_scaled(C3D_RenderTarget *rt, C3D_Tex *src, int kind, int abr, u32 rgba, int sx, int sy, int w,
                               int h, int ox, int oy, int ow, int oh);
static void screen_quad(C3D_RenderTarget *rt, C3D_Tex *src, int kind, int abr, u32 rgba, int sx, int sy, int w, int h,
                        int ox, int oy) {
    screen_quad_scaled(rt, src, kind, abr, rgba, sx, sy, w, h, ox, oy, w, h);
}
/* texels (sx, sy) w x h drawn over ow x oh at (ox, oy) */
static void screen_quad_scaled(C3D_RenderTarget *rt, C3D_Tex *src, int kind, int abr, u32 rgba, int sx, int sy, int w,
                               int h, int ox, int oy, int ow, int oh) {
    C3D_Mtx p;
    Mtx_OrthoTilt(&p, 0.0f, rt == bottom_rt ? 320.0f : 400.0f, 240.0f, 0.0f, 0.0f, 1.0f, true);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, u_proj, &p);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, u_stereo, 0, 0, 1, 0.5f);
    set_tev(kind);
    set_blend(abr, 0);
    C3D_TexBind(0, src);
    set_texscale(src);
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    Vtx *v = alloc_vtx(6);
    quad(v, ox, oy, ox + ow, oy + oh, sx, sy, sx + w, sy + h, rgba);
    GSPGPU_FlushDataCache(v, 6 * sizeof(Vtx));
    vb_submitted = vb_used;
    C3D_DrawArrays(GPU_TRIANGLES, v - vb, 6);
}

static void draw_to_screen(C3D_RenderTarget *rt, C3D_Tex *src, int sx, int sy, int w, int h, int ox, int oy) {
    C3D_RenderTargetClear(rt, C3D_CLEAR_ALL, 0x000000FF, 0);
    C3D_FrameDrawOn(rt);
    if (src) screen_quad(rt, src, K_BLIT, NO_BLEND, 0xFFFFFFFF, sx, sy, w, h, ox, oy);
}

/* an untextured rectangle on a screen's render target (already drawn on) */
static void screen_fill(C3D_RenderTarget *rt, int x, int y, int w, int h, u32 rgba, int abr) {
    C3D_Mtx p;
    Mtx_OrthoTilt(&p, 0.0f, rt == bottom_rt ? 320.0f : 400.0f, 240.0f, 0.0f, 0.0f, 1.0f, true);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, u_proj, &p);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, u_stereo, 0, 0, 1, 0.5f);
    set_tev(K_UNTEX);
    set_blend(abr, 0);
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    Vtx *v = alloc_vtx(6);
    quad(v, x, y, x + w, y + h, 0, 0, 0, 0, rgba);
    GSPGPU_FlushDataCache(v, 6 * sizeof(Vtx));
    vb_submitted = vb_used;
    C3D_DrawArrays(GPU_TRIANGLES, v - vb, 6);
}

/* The field panel (touch.c's layout): in a field or on the world map while no window is up. Its icons are the game's
 * (texture page 960,256 with the menu cursor's CLUT), fetched before the screens are drawn (a conversion may sync). */
__attribute__((weak)) int port_game_mode(void) { return 0; }
static int panel_on;
static C3D_Tex *panel_icons;
int ctr_panel_shown(void) { return panel_on; }
static void panel_prepare(void) {
    int mode = port_game_mode();
    panel_on = (mode == 1 || mode == 3) && !menu_state && shown_layer < 0;
    if (!panel_on || !psx_vram) return;
    Gp0Mode m;
    memset(&m, 0, sizeof m);
    m.textured = 1; m.raw = 1; m.depth = 0; m.tx = 960; m.ty = 256; m.clut_x = 576; m.clut_y = 236;
    unsigned f;
    panel_icons = texture_for(&m, 0, 0, 95, 31, &f);
}
static void panel_draw(void) {
    if (!panel_on || !panel_icons) return;
    /* the drag pad: the d-pad icon where the drag began (or at rest in the middle of its half), lit toward its
     * direction */
    int px = ctr_panel_pad_dir ? ctr_panel_pad_x : CTR_PANEL_PAD_W / 2, py = ctr_panel_pad_dir ? ctr_panel_pad_y : 150;
    screen_fill(bottom_rt, px - 30, py - 30, 60, 60, 0x60202020, 0);
    screen_quad_scaled(bottom_rt, panel_icons, K_TEX, NO_BLEND, ctr_panel_pad_dir ? 0xFFC0C0C0 : 0xFF808080, 24, 12, 11,
                       11, px - 22, py - 22, 44, 44);
    for (int i = 0; i < ctr_panel_nbuttons; i++) {
        const CtrPanelButton *b = &ctr_panel_buttons[i];
        int held = ctr_panel_held >> i & 1;
        screen_fill(bottom_rt, b->x, b->y, b->w, b->h, held ? 0x80606060 : 0x80303030, 0);
        int sz = b->w * 2 / 3;
        screen_quad_scaled(bottom_rt, panel_icons, K_TEX, NO_BLEND, held ? 0xFFFFFFFF : 0xFF909090, b->icon_u, b->icon_v,
                           11, 11, b->x + (b->w - sz) / 2, b->y + (b->h - sz) / 2, sz, sz);
    }
}

/* The bottom screen: the UI layer of the displayed buffer over its scene, dimmed (the scene is on the top screen). */
int ctr_ui_oy;
static int bottom_layer(const PsxDisplayInfo *d) {
    int k = layers[0].x == d->x && layers[0].y == d->y ? 0 : layers[1].x == d->x && layers[1].y == d->y ? 1 : -1;
    if (k >= 0 && frames_drawn - layers[k].drawn > 2) k = -1; /* (nothing drawn into it lately) */
    return k;
}
static void present_bottom(const PsxDisplayInfo *d, int w, int h) {
    int k = shown_layer;
    ctr_ui_oy = (240 - h) / 2;
    C3D_RenderTargetClear(bottom_rt, C3D_CLEAR_ALL, 0x000000FF, 0);
    C3D_FrameDrawOn(bottom_rt);
    if (d->rgb24) return;
    screen_quad(bottom_rt, &mirror_tex[0], K_SCENE, NO_BLEND, 0xFF383838, d->x, d->y, w, h, 0, ctr_ui_oy);
    if (k < 0) { ctr_ui_nhands = 0; panel_draw(); return; }
    screen_quad(bottom_rt, &layers[k].tex, K_COMPOSITE, 5, 0xFFFFFFFF, 0, 0, w, h, 0, ctr_ui_oy);
    layers[k].shown = 1;
    ctr_ui_nhands = layers[k].nhands;
    memcpy(ctr_ui_hands, layers[k].hands, sizeof ctr_ui_hands);
}

static void present_rgb24(const PsxDisplayInfo *d, int w, int h, int ox, int oy) {
    cpu_reads(d->x, d->y, d->x + (w * 3 + 1) / 2, d->y + h);
    unsigned tw = pow2_at_least(w), th = pow2_at_least(h);
    u8 *px = staging_alloc(tw * th * 3);
    for (int y = 0; y < h; y++) {
        const u8 *src = (const u8 *)(psx_vram + ((d->y + y) & 511) * VW + d->x);
        for (int x = 0; x < w; x++) {
            u8 *o = px + tiled(x, y, tw) * 3;
            o[0] = src[x * 3 + 2]; o[1] = src[x * 3 + 1]; o[2] = src[x * 3];
        }
    }
    GSPGPU_FlushDataCache(px, tw * th * 3);
    C3D_Tex *t = staging_tex(px, tw, th, GPU_RGB8);
    for (int eye = 0; eye < (stereo_on ? 2 : 1); eye++) draw_to_screen(top_rt[eye], t, 0, 0, w, h, ox, oy);
}

void ctr_present(long frame) {
    (void)frame;
    float slider = osGet3DSliderState();
    int want_stereo = slider > 0.01f;
    stereo_k = slider * STEREO_MAX_PX / stereo_inv_zc; /* up to STEREO_MAX_PX of parallax at infinity */
    if (want_stereo && !stereo_on) {
        /* eye 1 missed what was drawn in mono: start it from eye 0's mirror */
        submit();
        Vtx *v = alloc_vtx(6);
        quad(v, 0, 0, VW, VH, 0, 0, VW, VH, 0xFFFFFFFF);
        cur_target = 0;
        cur_mask = MASK_COPY;
        add_batch(K_BLIT, NO_BLEND, 2, full_scissor, &mirror_tex[0], v - vb, 6);
        stereo_on = 1;
        submit();
    }
    stereo_on = want_stereo;
    gfxSet3D(stereo_on);

    menu_state = port_menu_state();
    PsxDisplayInfo d;
    psx_gpu_display_info(&d);
    /* the displayed buffer is redrawn when something was drawn or the display moved (a static screen flips between
     * two finished buffers) */
    static PsxDisplayInfo last;
    int moved = memcmp(&d, &last, sizeof d) != 0;
    last = d;
    if (!moved && psx_display_generation == display_shown_gen && !nbatches) return;
    display_shown_gen = psx_display_generation;
    int w = d.w ? d.w : 320, h = d.h ? d.h : 240;
    if (w > 400) w = 400;
    if (h > 240) h = 240;
    int ox = (400 - w) / 2, oy = (240 - h) / 2;
    unsigned profile_start = port_profile_begin();
    shown_layer = bottom_layer(&d);
    panel_prepare(); /* (before any screen is drawn on: a texture conversion may sync the GPU) */
    if (d.rgb24) {
        submit();
        present_rgb24(&d, w, h, ox, oy);
    } else {
        int cx0, cy0, cx1, cy1;
        if (cell_range(d.x, d.y, d.x + w, d.y + h, &cx0, &cy0, &cx1, &cy1))
            for (int cy = cy0; cy < cy1; cy++)
                for (int cx = cx0; cx < cx1; cx++)
                    if (!mirror_ok[cy][cx]) { blit_cells(cx0, cy0, cx1, cy1); cy = cy1; break; }
        submit();
        frame_begin();
        /* in stereo, both edges are masked by the largest parallax, in both eyes: what a layer's shift uncovers at
         * the frame's edge isn't seen (a floating window) */
        int edge = stereo_on ? (int)(STEREO_MAX_PX * slider + 0.99f) : 0;
        if (menu_state && !frozen) { /* a menu was asked for: keep the scene as it is now for the top screen */
            for (int eye = 0; eye < 2; eye++) {
                C3D_FrameDrawOn(freeze_rt[eye]);
                set_proj_ortho(UI_W, UI_H, 0.0f);
                C3D_FVUnifSet(GPU_VERTEX_SHADER, u_stereo, 0, 1, 1, -0.5f);
                set_tev(K_BLIT);
                set_blend(NO_BLEND, 0);
                C3D_Tex *src = &mirror_tex[stereo_on ? eye : 0];
                C3D_TexBind(0, src);
                set_texscale(src);
                C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
                Vtx *v = alloc_vtx(6);
                quad(v, 0, 0, w, h, d.x, d.y, d.x + w, d.y + h, 0xFFFFFFFF);
                GSPGPU_FlushDataCache(v, 6 * sizeof(Vtx));
                vb_submitted = vb_used;
                C3D_DrawArrays(GPU_TRIANGLES, v - vb, 6);
            }
            frozen = 1;
            freeze_w = w;
            freeze_h = h;
        } else if (!menu_state) frozen = 0;
        for (int eye = 0; eye < (stereo_on ? 2 : 1); eye++) {
            if (frozen) draw_to_screen(top_rt[eye], &freeze_tex[eye], edge, 0, freeze_w - 2 * edge, freeze_h,
                                       (400 - freeze_w) / 2 + edge, (240 - freeze_h) / 2);
            else draw_to_screen(top_rt[eye], &mirror_tex[eye], d.x + edge, d.y, w - 2 * edge, h, ox + edge, oy);
        }
    }
    present_bottom(&d, w, h);
    C3D_FrameEnd(GX_CMDLIST_FLUSH) /* (we flush what we write ourselves: not the whole linear heap) */;
    in_frame = 0;
    frames_drawn++;
    extern unsigned int psx_depth_frame;
    psx_depth_frame++;
    ranges_age();
    fit_frame();
    port_profile_end(PORT_PROFILE_GPU, profile_start);
}

/* ---- diagnostics: DUMP_AT=N writes the mirror, the shadow and the texture slots to sdmc:/3ds/ff9/dump/ ---- */

static void write_ppm(const char *name, int w, int h, u32 (*px)(int x, int y, const void *ctx), const void *ctx) {
    char path[128];
    snprintf(path, sizeof path, "sdmc:/3ds/ff9/dump/%s.ppm", name);
    FILE *f = fopen(path, "wb");
    if (!f) { port_log("dump: can't write %s\n", path); return; }
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    u8 *row = malloc(w * 3);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            u32 c = px(x, y, ctx);
            row[x * 3] = c >> 16; row[x * 3 + 1] = c >> 8; row[x * 3 + 2] = c;
        }
        fwrite(row, 1, w * 3, f);
    }
    free(row);
    fclose(f);
}
static u32 px_mirror(int x, int y, const void *ctx) {
    u16 p = ((const u16 *)ctx)[tiled(x, y, VW)];
    unsigned r = p >> 11, g = (p >> 6) & 31, b = (p >> 1) & 31;
    return (r << 3 | r >> 2) << 16 | (g << 3 | g >> 2) << 8 | (b << 3 | b >> 2);
}
static u32 px_crop(int x, int y, const void *ctx) {
    u16 p = ((const u16 *)ctx)[y * 320 + x];
    unsigned r = p >> 11, g = (p >> 5) & 63, b = p & 31;
    return (r << 3 | r >> 2) << 16 | (g << 2 | g >> 4) << 8 | (b << 3 | b >> 2);
}
static u32 px_mask(int x, int y, const void *ctx) {
    return ((const u16 *)ctx)[tiled(x, y, VW)] & 1 ? 0xFFFFFF : 0;
}
static u32 px_shadow(int x, int y, const void *ctx) {
    u16 c = ((const u16 *)ctx)[y * VW + x];
    unsigned r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    return (r << 3) << 16 | (g << 3) << 8 | b << 3;
}
static u32 px_slot(int x, int y, const void *ctx) {
    u32 c = ((const u32 *)ctx)[tiled(x, y, 256)];
    unsigned a = c & 0xFF;
    if (!a) return ((x ^ y) & 8) ? 0xFF00FF : 0x800080; /* transparent: magenta checks */
    return c >> 8;
}
/* DUMP_EVERY=N: the displayed buffer every N frames, as dump/f<frame>.ppm (a timeline of one run) */
void ctr_render_dump_display(long frame) {
    PsxDisplayInfo d;
    psx_gpu_display_info(&d);
    mkdir("sdmc:/3ds/ff9/dump", 0777);
    gpu_sync();
    static u16 crop[320 * 240];
    int w = d.w > 320 ? 320 : d.w, h = d.h > 240 ? 240 : d.h;
    const u16 *m = mirror_tex[0].data;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) { /* (as RGB565 for px_crop) */
            u16 p = m[tiled((d.x + x) & 1023, (d.y + y) & 511, VW)];
            crop[y * 320 + x] = (u16)((p & 0xF800) | (p & 0x7C0) | ((p >> 1) & 31));
        }
    /* with the displayed buffer's UI layer composited as the PS1 frame has it (premultiplied: layer + scene (1 - a)) */
    int k = layers[0].x == d.x && layers[0].y == d.y ? 0 : layers[1].x == d.x && layers[1].y == d.y ? 1 : -1;
    if (k >= 0 && frames_drawn - layers[k].drawn <= 2) {
        const u32 *l = layers[k].tex.data;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                u32 c = l[tiled(x, y, UI_W)];
                unsigned a = c & 0xFF, lr = c >> 24, lg = (c >> 16) & 0xFF, lb = (c >> 8) & 0xFF;
                u16 p = crop[y * 320 + x];
                unsigned sr = (p >> 11) << 3, sg = ((p >> 5) & 63) << 2, sb = (p & 31) << 3;
                unsigned r = lr + sr * (255 - a) / 255, g = lg + sg * (255 - a) / 255, b = lb + sb * (255 - a) / 255;
                if (r > 255) r = 255;
                if (g > 255) g = 255;
                if (b > 255) b = 255;
                crop[y * 320 + x] = (u16)((r >> 3) << 11 | (g >> 2) << 5 | b >> 3);
            }
    }
    char name[32];
    snprintf(name, sizeof name, "f%06ld", frame);
    static u16 *src;
    src = crop;
    write_ppm(name, w, h, px_crop, src);
}

void ctr_render_dump(void) {
    mkdir("sdmc:/3ds/ff9/dump", 0777);
    gpu_sync();
    write_ppm("mirror0", VW, VH, px_mirror, mirror_tex[0].data);
    write_ppm("mirror0_mask", VW, VH, px_mask, mirror_tex[0].data);
    if (stereo_on) write_ppm("mirror1", VW, VH, px_mirror, mirror_tex[1].data);
    write_ppm("shadow", VW, VH, px_shadow, psx_vram);
    for (int i = 0; i < nslots && i < 24; i++) {
        char name[48];
        snprintf(name, sizeof name, "slot%02d_%08lx", i, (unsigned long)slots[i].key);
        write_ppm(name, 256, 256, px_slot, slots[i].tex.data);
    }
    FILE *f = fopen("sdmc:/3ds/ff9/dump/cells.txt", "w");
    if (f) {
        for (int cy = 0; cy < CH; cy++) {
            for (int cx = 0; cx < CW; cx++) fputc('0' + shadow_ok[cy][cx] + 2 * mirror_ok[cy][cx], f);
            fputc('\n', f);
        }
        fprintf(f, "stats: batches %u prims %u blits %u readbacks %u blocks %u slots %u syncs %u\n",
                ctr_render_stats[0], ctr_render_stats[1], ctr_render_stats[2], ctr_render_stats[3],
                ctr_render_stats[4], ctr_render_stats[5], ctr_render_stats[6]);
        fclose(f);
    }
    port_log("dump: written\n");
}

/* ---- set-up ------------------------------------------------------------------------------------------------- */

#define DISPLAY_TRANSFER_FLAGS \
    (GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) | \
     GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | \
     GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

void ctr_video_init(void) {
    C3D_Init(0x100000);
    for (int i = 0; i < 8; i++) {
        morton_x[i] = (u8)((i & 1) | (i & 2) << 1 | (i & 4) << 2);
        morton_y[i] = (u8)(morton_x[i] << 1);
    }
    for (int c = 0; c < 65536; c++) {
        if ((c & 0x7FFF) == 0 && !(c & 0x8000)) { rgba_lut[c] = 0; continue; }
        unsigned r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
        r = r << 3 | r >> 2; g = g << 3 | g >> 2; b = b << 3 | b >> 2;
        rgba_lut[c] = r << 24 | g << 16 | b << 8 | (c & 0x8000 ? 0x80 : 0xFF);
    }
    shader_dvlb = DVLB_ParseFile((u32 *)prim_shbin, prim_shbin_size);
    shaderProgramInit(&shader);
    shaderProgramSetVsh(&shader, &shader_dvlb->DVLE[0]);
    u_proj = shaderInstanceGetUniformLocation(shader.vertexShader, "proj");
    u_stereo = shaderInstanceGetUniformLocation(shader.vertexShader, "stereo");
    u_texscale = shaderInstanceGetUniformLocation(shader.vertexShader, "texscale");
    AttrInfo_Init(&attr);
    AttrInfo_AddLoader(&attr, 0, GPU_FLOAT, 3);
    AttrInfo_AddLoader(&attr, 1, GPU_FLOAT, 2);
    AttrInfo_AddLoader(&attr, 2, GPU_UNSIGNED_BYTE, 4);

    vb = linearAlloc(VB_MAX * sizeof(Vtx));
    staging = linearAlloc(STAGING_SIZE);
    for (int eye = 0; eye < 2; eye++) {
        if (!C3D_TexInitVRAM(&mirror_tex[eye], VW, VH, GPU_RGBA5551)) port_fatal("no VRAM for the PS1 VRAM mirror");
        C3D_TexSetFilter(&mirror_tex[eye], GPU_NEAREST, GPU_NEAREST);
        mirror_rt[eye] = C3D_RenderTargetCreateFromTex(&mirror_tex[eye], GPU_TEXFACE_2D, 0, -1);
        top_rt[eye] = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, -1);
        C3D_RenderTargetSetOutput(top_rt[eye], GFX_TOP, eye ? GFX_RIGHT : GFX_LEFT, DISPLAY_TRANSFER_FLAGS);
    }
    for (int eye = 0; eye < 2; eye++) {
        if (!C3D_TexInitVRAM(&freeze_tex[eye], UI_W, UI_H, GPU_RGB565)) port_fatal("no VRAM for the menu snapshot");
        C3D_TexSetFilter(&freeze_tex[eye], GPU_NEAREST, GPU_NEAREST);
        freeze_rt[eye] = C3D_RenderTargetCreateFromTex(&freeze_tex[eye], GPU_TEXFACE_2D, 0, -1);
    }
    for (int k = 0; k < 2; k++) {
        if (!C3D_TexInitVRAM(&layers[k].tex, UI_W, UI_H, GPU_RGBA8)) port_fatal("no VRAM for the UI layers");
        C3D_TexSetFilter(&layers[k].tex, GPU_NEAREST, GPU_NEAREST);
        layers[k].rt = C3D_RenderTargetCreateFromTex(&layers[k].tex, GPU_TEXFACE_2D, 0, -1);
        layers[k].x = layers[k].y = -1;
        C3D_RenderTargetClear(layers[k].rt, C3D_CLEAR_ALL, 0, 0);
    }
    bottom_rt = C3D_RenderTargetCreate(240, 320, GPU_RB_RGBA8, -1);
    C3D_RenderTargetSetOutput(bottom_rt, GFX_BOTTOM, GFX_LEFT, DISPLAY_TRANSFER_FLAGS);
    if (!vb || !staging) port_fatal("no linear memory for the renderer");
    /* the mirror starts as the shadow does: black, and current */
    memset(shadow_ok, 1, sizeof shadow_ok);
    memset(mirror_ok, 0, sizeof mirror_ok);
    gp0_backend = &pica_backend;
    port_log("renderer: PICA200, %u KB linear free\n", (unsigned)(linearSpaceFree() >> 10));
}

void ctr_shutdown(void) {
    void ctr_audio_exit(void);
    ctr_async_exit();
    void ctr_spu_exit(void);
    ctr_spu_exit();
    ctr_audio_exit();
    if (in_frame) C3D_FrameEnd(GX_CMDLIST_FLUSH) /* (we flush what we write ourselves: not the whole linear heap) */;
    C3D_Fini();
    gfxExit();
}
#endif
