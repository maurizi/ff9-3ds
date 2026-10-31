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
#include <math.h>
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
    float d;  /* a reordered run's primitive: its place in drawing order, as depth (run_flush); else 0 */
} Vtx;

#define VB_MAX (96 * 1024)
static Vtx *vb;
static unsigned vb_used, vb_flushed;

#define STAGING_SIZE (3 * 1024 * 1024)
static u8 *staging;
static unsigned staging_used, staging_flushed;

/* The GPU reads main memory, not the CPU's data cache, and only once a command list is sent (C3D_FrameEnd): what the
 * CPU wrote for it is flushed then, each buffer once (a flush is a request to the GSP service: one per vertex quad or
 * texture block cost more than the drawing). Vertices and staging grow contiguously; texture blocks are listed. */
#define MAX_DIRTY 64
static struct { u8 *p; unsigned n; } dirty[MAX_DIRTY];
static unsigned ndirty;
unsigned ctr_flushes, ctr_batch_why[6], ctr_kind_pairs[8][8];
static void flush_range(void *p, unsigned n) {
    if (!n) return;
    GSPGPU_FlushDataCache(p, n);
    ctr_flushes++;
}
static void flush_pending(void);
static void mark_dirty(void *p, unsigned n) {
    u8 *b = p;
    for (unsigned i = 0; i < ndirty; i++) { /* (adjacent blocks of one texture: one range) */
        if (dirty[i].p + dirty[i].n == b) { dirty[i].n += n; return; }
        if (b + n == dirty[i].p) { dirty[i].p = b; dirty[i].n += n; return; }
    }
    if (ndirty == MAX_DIRTY) flush_pending();
    dirty[ndirty].p = b;
    dirty[ndirty++].n = n;
}
static void flush_pending(void) {
    flush_range(vb + vb_flushed, (vb_used - vb_flushed) * sizeof(Vtx));
    vb_flushed = vb_used;
    flush_range(staging + staging_flushed, staging_used - staging_flushed);
    staging_flushed = staging_used;
    for (unsigned i = 0; i < ndirty; i++) flush_range(dirty[i].p, dirty[i].n);
    ndirty = 0;
}
/* the command list goes to the GPU (the frame's end, or a sync) */
static void frame_end(void) {
    flush_pending();
    C3D_FrameEnd(GX_CMDLIST_FLUSH) /* (we flush what we write ourselves: not the whole linear heap) */;
}

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
#define MAG_W 200 /* the card game's magnified area (to 400x240 on the top screen) */
#define MAG_H 120
static int menu_state; /* port_menu_state(), read at each present */
static int have_freeze; /* freeze_tex holds a scene */
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

/* the mirrors' depth buffer, for reordered runs (run_add): */
#define DEPTH_MAX 16000 /* depth values per clear (1/16384 apart, as the shader maps them) */
static void *mirror_depth;
static unsigned depth_next = 1, depth_submitted = 1; /* the next depth value; the first of this submit's */

static void frame_begin(void) {
    if (in_frame) return;
    unsigned profile_start = port_profile_begin();
    C3D_FrameBegin(0); /* waits for the GPU to finish the previous frame: its vertices and staging are free */
    port_profile_end(PORT_PROFILE_SCALE, profile_start); /* ("scale": the wait for the GPU) */
    if (mirror_depth && depth_next > DEPTH_MAX / 2) { /* (runs' depth values used up: from 0 again) */
        C3D_RenderTargetClear(mirror_rt[0], C3D_CLEAR_DEPTH, 0, 0);
        depth_next = depth_submitted = 1;
    }
    in_frame = 1;
    vb_used = vb_flushed = 0;
    staging_used = staging_flushed = 0;
    frame_serial++;
    C3D_BindProgram(&shader);
    C3D_SetAttrInfo(&attr);
    BufInfo_Init(&bufinfo);
    BufInfo_Add(&bufinfo, vb, sizeof(Vtx), 4, 0x3210);
    C3D_SetBufInfo(&bufinfo);
    C3D_CullFace(GPU_CULL_NONE);
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
}

static void submit(void);

/* everything recorded so far executed and finished (the CPU may then read the mirror, or rewrite a texture) */
static void gpu_sync(void) {
    submit();
    if (in_frame) {
        frame_end();
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
    u8 mask, ordered, pad2[2];  /* MASK_SET | MASK_CHECK: the PS1's mask bit as the mirror's alpha (GP0 E6);
                                 * ordered: a run's (drawn in order by depth, run_flush) */
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

/* ---- reordering within a run of alternating states -----------------------------------------------------------
 * The world map draws each terrain triangle twice, textured and then again untextured with additive shading, so
 * the state alternates with every primitive (a draw call each). A primitive that replaces the pixels it draws (no
 * blending, no mask check) may be drawn earlier than primitives before it, if those then don't draw over it: the
 * run's primitives get their place in drawing order as depth (a 16-bit depth buffer shared by both eyes' mirrors),
 * and draw where it is at least the depth there. So a run keeps two batches open, one of replacing primitives and
 * the one after it, and a replacing primitive joins the first even when the second has come since: its pixels are
 * exactly the PS1's whatever overlaps, in either eye. Everything else draws without depth, after the run. */
#define RUN_V 1536 /* vertices per open batch */
typedef struct {
    u8 kind, abr, mask;
    u16 sc[4];
    C3D_Tex *tex;
    unsigned nv;
} RunGroup;
static RunGroup run_g[2];
static int run_n;
static unsigned run_prims; /* primitives in the run (their place: Vtx.d, from 0) */
static Vtx run_v[2][RUN_V];
static int cur_ordered;
unsigned ctr_run_reorders;

static void add_batch(int kind, int abr, int eyes, const u16 sc[4], C3D_Tex *tex, unsigned first, unsigned n);
static Vtx *alloc_vtx(unsigned n);
static void gpu_sync(void);

/* the open batches into the batch list, in order, their depth values from here */
static void run_flush(void) {
    int n = run_n;
    if (!n) return;
    run_n = 0;
    unsigned prims = run_prims;
    run_prims = 0;
    /* (both eyes' values of this submit must fit before the next clear: frame_begin clears) */
    if (2 * (depth_next - depth_submitted + prims) + depth_submitted > DEPTH_MAX) gpu_sync();
    unsigned total = 0;
    for (int i = 0; i < n; i++) total += run_g[i].nv;
    Vtx *o = alloc_vtx(total); /* (one allocation: a sync it makes leaves the run's values valid) */
    float base = (float)depth_next;
    depth_next += prims;
    int saved_target = cur_target, saved_mask = cur_mask;
    for (int i = 0; i < n; i++) {
        RunGroup *g = &run_g[i];
        for (unsigned k = 0; k < g->nv; k++) o[k] = run_v[i][k], o[k].d += base;
        cur_target = 0;
        cur_mask = g->mask;
        cur_ordered = 1;
        add_batch(g->kind, g->abr, 3, g->sc, g->tex, o - vb, g->nv);
        o += g->nv;
    }
    cur_ordered = 0;
    cur_target = saved_target;
    cur_mask = saved_mask;
}

static inline int run_same(const RunGroup *g, int kind, int abr, const u16 sc[4], const C3D_Tex *tex) {
    return g->kind == kind && g->abr == abr && g->mask == cur_mask && g->tex == tex && !memcmp(g->sc, sc, sizeof g->sc);
}

/* whether a batch's primitives replace what they draw (so they may move ahead of others: their depth keeps later ones
 * off them) */
static inline int replaces(const RunGroup *g) {
    return g->abr == NO_BLEND && !(g->mask & MASK_CHECK) && (g->kind == K_TEX || g->kind == K_TEX_P1 || g->kind == K_UNTEX);
}

static void run_put(int i, const Vtx *v, unsigned n) {
    Vtx *o = run_v[i] + run_g[i].nv;
    for (unsigned k = 0; k < n; k++) o[k] = v[k], o[k].d = (float)run_prims;
    run_g[i].nv += n;
    run_prims++;
}

static void run_open(int i, int kind, int abr, const u16 sc[4], C3D_Tex *tex) {
    RunGroup *g = &run_g[i];
    g->kind = (u8)kind; g->abr = (u8)abr; g->mask = (u8)cur_mask; g->tex = tex; g->nv = 0;
    memcpy(g->sc, sc, sizeof g->sc);
    run_n = i + 1;
}

/* a primitive into the mirror (triangles) */
static void run_add(int kind, int abr, const u16 sc[4], C3D_Tex *tex, const Vtx *v, unsigned n) {
    if (cur_target != 0 || n > RUN_V || !mirror_depth) {
        run_flush();
        Vtx *o = alloc_vtx(n);
        memcpy(o, v, n * sizeof *v);
        add_batch(kind, abr, 3, sc, tex, o - vb, n);
        return;
    }
    if (run_n) {
        int last = run_n - 1;
        if (run_same(&run_g[last], kind, abr, sc, tex) && run_g[last].nv + n <= RUN_V) {
            run_put(last, v, n);
            return;
        }
        if (run_n == 2 && run_same(&run_g[0], kind, abr, sc, tex) && replaces(&run_g[0]) && run_g[0].nv + n <= RUN_V) {
            run_put(0, v, n);
            ctr_run_reorders++;
            return;
        }
        if (run_n == 1 && replaces(&run_g[0])) {
            run_open(1, kind, abr, sc, tex);
            run_put(1, v, n);
            return;
        }
        run_flush();
    }
    run_open(0, kind, abr, sc, tex);
    run_put(0, v, n);
}

static void add_batch(int kind, int abr, int eyes, const u16 sc[4], C3D_Tex *tex, unsigned first, unsigned n) {
    if (run_n) run_flush(); /* (anything else drawn ends the run: it keeps its place after it) */
    if (nbatches) {
        Batch *b = &batches[nbatches - 1];
        if (b->kind == kind && b->abr == abr && b->eyes == eyes && b->tex == tex && b->first + b->count == first &&
            b->target == cur_target && b->mask == cur_mask && b->ordered == cur_ordered &&
            !memcmp(b->sc, sc, sizeof b->sc)) {
            b->count += n;
            return;
        }
    }
#ifdef PORT_PROFILE
    if (nbatches) { /* why a new batch: kind, blend, texture, scissor, mask/target, not contiguous */
        extern unsigned ctr_batch_why[6];
        Batch *p = &batches[nbatches - 1];
        if (p->kind != kind) {
            extern unsigned ctr_kind_pairs[8][8];
            ctr_batch_why[0]++;
            ctr_kind_pairs[p->kind & 7][kind & 7]++;
        }
        else if (p->abr != abr) ctr_batch_why[1]++;
        else if (p->tex != tex) ctr_batch_why[2]++;
        else if (memcmp(p->sc, sc, sizeof p->sc)) ctr_batch_why[3]++;
        else if (p->mask != cur_mask || p->target != cur_target || p->eyes != eyes) ctr_batch_why[4]++;
        else ctr_batch_why[5]++;
    }
#endif
    if (nbatches == MAX_BATCHES) submit();
    Batch *b = &batches[nbatches++];
    b->kind = kind; b->abr = abr; b->eyes = eyes; b->tex = tex; b->target = (u8)cur_target; b->mask = (u8)cur_mask;
    b->ordered = (u8)cur_ordered;
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
    m.r[2].z = 1.0f; m.r[2].w = zplane; /* (z: the shader's depth plane, less a run's depth) */
    m.r[3].w = 1.0f;
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, u_proj, &m);
}

static void set_texscale(const C3D_Tex *t) {
    /* (texture memory runs top-down, t bottom-up: a texel row r is at t = 1 - r / height) */
    C3D_FVUnifSet(GPU_VERTEX_SHADER, u_texscale, t ? 1.0f / t->width : 0.0f, t ? -1.0f / t->height : 0.0f, 0, 1);
}

/* Replay the recorded batches into each eye's mirror. */
static void submit(void) {
    run_flush();
    if (!nbatches) return;
    unsigned profile_start = port_profile_begin();
    frame_begin();
    unsigned any_ui = 0;
    for (unsigned i = 0; i < nbatches; i++) any_ui |= 1u << batches[i].target;
    /* passes: each eye's mirror, then each UI layer (no stereo: the bottom screen is flat) */
    for (int pass = 0; pass < 6; pass++) {
        int eye = pass == 1 || pass == 5, target = pass >= 4 ? 3 : pass >= 2 ? pass - 1 : 0;
        if ((pass == 1 || pass == 5) && !stereo_on) continue;
        if (target && !(any_ui & (1u << target))) continue;
        int height = target == 3 ? 240 : target ? UI_H : VH;
        if (target == 3) { /* the top screen (its render target is the rotated 240x400): no scissor, as screen_quad */
            C3D_FrameDrawOn(top_rt[eye]);
            C3D_Mtx pm;
            Mtx_OrthoTilt(&pm, 0.0f, 400.0f, 240.0f, 0.0f, 0.0f, 1.0f, true);
            C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, u_proj, &pm);
            C3D_FVUnifSet(GPU_VERTEX_SHADER, u_stereo, 0, 0, 1, 0.5f);
            C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
        } else if (target) {
            C3D_FrameDrawOn(layers[target - 1].rt);
            set_proj_ortho(UI_W, UI_H, 0.0f);
            C3D_FVUnifSet(GPU_VERTEX_SHADER, u_stereo, 0, stereo_inv_zc, 1.0f / 4096.0f, -0.5f);
        } else {
            C3D_FrameDrawOn(mirror_rt[eye]);
            set_proj_ortho(VW, VH, 0.0f);
            C3D_FVUnifSet(GPU_VERTEX_SHADER, u_stereo, eye ? stereo_k : -stereo_k, stereo_inv_zc, 1.0f / 4096.0f, -0.5f);
        }
        int kind = -1, abr = -1, bmask = -1, ordered = 0;
        C3D_Tex *tex = (C3D_Tex *)-1;
        u16 sc[4] = {0xFFFF, 0, 0, 0};
        float k = eye ? stereo_k : -stereo_k;
        /* a run's depth values: this submit's, then the same again past them for the other eye */
        float depth_off = (float)(eye ? depth_next - depth_submitted : 0);
        for (unsigned i = 0; i < nbatches; i++) {
            Batch *b = &batches[i];
            if (b->target != target) continue;
            if (!target && !(b->eyes & (1 << eye))) continue;
            if (b->ordered != ordered) {
                ordered = b->ordered;
                C3D_DepthTest(ordered, GPU_GEQUAL, ordered ? GPU_WRITE_ALL : GPU_WRITE_COLOR);
                C3D_FVUnifSet(GPU_VERTEX_SHADER, u_stereo, target ? 0 : k, stereo_inv_zc, 1.0f / 4096.0f,
                              ordered ? -depth_off * (1.0f / 16384) : -0.5f);
            }
            if (b->abr != abr || b->mask != bmask || b->kind != kind) {
                if (b->kind != kind) set_tev_for(kind = b->kind, target != 0);
                set_blend_mask(abr = b->abr, target != 0, bmask = b->mask, kind);
            }
            if (b->tex != tex) {
                tex = b->tex;
                if (tex) C3D_TexBind(0, tex);
                set_texscale(tex);
            }
            if (target != 3 && memcmp(sc, b->sc, sizeof sc)) {
                memcpy(sc, b->sc, sizeof sc);
                /* (in window coordinates, which run bottom-up) */
                C3D_SetScissor(GPU_SCISSOR_NORMAL, sc[0], height - sc[3], sc[2], height - sc[1]);
            }
            C3D_DrawArrays(GPU_TRIANGLES, b->first, b->count);
        }
        if (ordered) C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    }
    ctr_render_stats[0] += nbatches;
    nbatches = 0;
    /* (the other eye's values were past this submit's: the next ones are past those) */
    depth_next += depth_next - depth_submitted;
    depth_submitted = depth_next;
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
    mark_dirty(dst + by * 32 * 256, 32 * 256 * 4);
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
    o->d = 0;
}

/* the vertices of one primitive (6 for a quad, 3 for a triangle) in each pass its kind needs */
/* ---- depth for 2D primitives from their ordering-table entry ----------------------------------------------
 * The game sorts its scene into ordering-table entries by depth, so an entry stands for a depth. Primitives the GTE
 * projected give (entry, SZ) pairs; a least-squares line through a frame's pairs gives the depth of the entries
 * where 2D primitives hang (a field's background layers, sprites), and with it their stereo parallax. Full-screen
 * ones (fades, letterboxes) stay at the screen plane. */
extern int psx_ot_entry, psx_ot_side;
static float fit_n, fit_s, fit_z, fit_ss, fit_sz; /* this frame's sums */
static float fit_lo = 1e9f, fit_hi = -1e9f;       /* this frame's range of sampled entries */
static float fit_a, fit_b;                        /* the last good line: SZ = a entry + b */
static float fit_min, fit_max;                    /* ... and the entries it was fitted over */
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
            fit_min = fit_lo;
            fit_max = fit_hi;
            fit_age = 0;
        }
    }
    if (fit_age != ~0u) fit_age++;
    fit_n = fit_s = fit_z = fit_ss = fit_sz = 0;
    fit_lo = 1e9f;
    fit_hi = -1e9f;
}

static const Vtx *ot_depth(const Vtx *v, unsigned n, Vtx *out, int w, int h) {
    if (psx_ot_entry < 0 || psx_ot_side) return v; /* (the UI's table: the screen plane) */
    float s = (float)psx_ot_entry, z = 0;
    for (unsigned i = 0; i < n; i++) z += v[i].z;
    if (z > 0) { /* projected: a sample */
        z /= n;
        fit_n += 1; fit_s += s; fit_z += z; fit_ss += s * s; fit_sz += s * z;
        if (s < fit_lo) fit_lo = s;
        if (s > fit_hi) fit_hi = s;
        return v;
    }
    if (fit_age > 30 || (w >= 300 && h >= 200)) return v;
    /* (the line holds over the entries it was fitted on; past them, as at their nearest end: a scene of one model, as
     * the opening's dark room, would otherwise extrapolate a glow around it far off its depth) */
    if (s < fit_min) s = fit_min;
    if (s > fit_max) s = fit_max;
    float d = fit_a * s + fit_b;
    if (d < 64) return v;
    for (unsigned i = 0; i < n; i++) out[i] = v[i], out[i].z = d;
    return out;
}

static int ui_ox, ui_oy; /* the displayed buffer's origin, which the UI list draws at */
static int ui_list_layer;  /* the layer the UI list draws into */
static int ui_layer_for(int x, int y, int clear);
static int ui_packet(void);
static int flat_packet(void);
static const u8 *zoom_lo, *zoom_hi; /* port_bottom_zoom's */
static float zoom_cx, zoom_cy;
static int zoom_factor;
extern const u32 *psx_gpu_packet;
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
    extern int psx_ui_list_battle, port_battle_hud_on, port_battle_hud_targets;
    /* the battle screen's own command rows and party cards replace the PS1's command window and party panel (OT
     * entries 14-16 and 18-20, and the panel header at 7-8 (shared with the lists, not open then): battle_ui.c) */
    if (ui_mode && psx_ui_list_battle && port_battle_hud_on) {
        int slot = 0x17 - psx_ot_entry;
        if (slot == 7 || slot == 8 || slot == 14 || slot == 15 || slot == 16 || (slot >= 18 && slot <= 20)) return;
        /* (the target window and its HP readout at 9-10: the port's targets) */
        if (port_battle_hud_targets && (slot == 4 || slot == 5 || slot == 9 || slot == 10)) return;
        extern int port_battle_help_on;
        if (port_battle_help_on && slot == 2) return; /* (Select help: on the top screen, battle_ui.c) */
    }
    /* the battle list's entry 17 (btlui_drawTargetCursors): the hands pointing into the scene, drawn in it */
    if (ui_mode == 2) { /* the top screen's UI list (ctr_top_ui): straight onto the top screen, at the screen plane */
        u16 tsc[4] = {(u16)clampi(sc[0], 0, 400), (u16)clampi(sc[1], 0, 240), (u16)clampi(sc[2], 0, 400), (u16)clampi(sc[3], 0, 240)};
        if (tsc[0] >= tsc[2] || tsc[1] >= tsc[3]) return;
        cur_target = 3;
        cur_mask = 0;
        ctr_render_stats[1]++;
        int passes2[2], np2 = 0, abr2 = m->semi ? m->abr : NO_BLEND;
        if (!m->textured) passes2[np2++] = K_UNTEX;
        else if (!m->semi) passes2[np2++] = K_TEX;
        else {
            if (tflags & 1) passes2[np2++] = K_TEX_P1;
            if (tflags & 2) passes2[np2++] = K_TEX_P2;
        }
        for (int p = 0; p < np2; p++) {
            Vtx *o = alloc_vtx(n);
            memcpy(o, v, n * sizeof *v);
            add_batch(passes2[p], passes2[p] == K_TEX_P1 ? NO_BLEND : abr2, 3, tsc, tex, o - vb, n);
        }
        return;
    }
    if (ui_mode && !(psx_ui_list_battle && 0x17 - psx_ot_entry == 17)) layer = ui_list_layer, ox = ui_ox, oy = ui_oy;
    /* (a 15-bit texture is a frame copied from VRAM, as the field-to-battle swirl draws: the scene, never a window,
     * whose art is CLUT-based, even when the game builds it in its window packets) */
    else if ((menu_state == 2 || ui_packet()) && !(m && m->textured && m->depth == 2)) { /* (a menu: the whole frame) */
        int x, y, w, h;
        psx_gpu_draw_area(&x, &y, &w, &h);
        layer = ui_layer_for(x, y, 0);
        ox = x;
        oy = y;
    }
    if (layer >= 0) { /* into a UI layer, relative to its framebuffer; the mirror isn't touched */
        for (unsigned i = 0; i < n; i++) moved[i] = v[i], moved[i].x -= ox, moved[i].y -= oy;
        if (zoom_factor > 1 && (const u8 *)psx_gpu_packet >= zoom_lo && (const u8 *)psx_gpu_packet < zoom_hi)
            for (unsigned i = 0; i < n; i++) /* (the world map's navigation map, zoomed: the party in the middle) */
                moved[i].x = 160 + (moved[i].x - zoom_cx) * zoom_factor,
                moved[i].y = 112 + (moved[i].y - zoom_cy) * zoom_factor;
        v = moved;
        sc[0] = (u16)clampi(sc[0] - ox, 0, UI_W); sc[2] = (u16)clampi(sc[2] - ox, 0, UI_W);
        sc[1] = (u16)clampi(sc[1] - oy, 0, UI_H); sc[3] = (u16)clampi(sc[3] - oy, 0, UI_H);
        if (sc[0] >= sc[2] || sc[1] >= sc[3]) return;
        layers[layer].drawn = frames_drawn;
        cur_target = layer + 1;
    } else {
        gpu_draws(bx0, by0, bx1, by1);
        cur_target = 0;
        /* (the field's windows, and anything under a drawing area smaller than a frame (a window's clip, a draw off
         * screen), stay at the screen plane: else text shifts per eye inside a box or clip that doesn't, and each eye
         * cuts it in a different place) */
        if (gp0_state.area_x1 - gp0_state.area_x0 >= 255 && gp0_state.area_y1 - gp0_state.area_y0 >= 200 && !flat_packet())
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
        int pabr = passes[p] == K_TEX_P1 ? NO_BLEND : abr;
        if (layer < 0 && n % 3 == 0) {
            run_add(passes[p], pabr, sc, tex, v, n);
            continue;
        }
        Vtx *o = alloc_vtx(n);
        memcpy(o, v, n * sizeof *v);
        add_batch(passes[p], pabr, 3, sc, tex, o - vb, n);
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

/* ---- the 3DS's buttons in place of the PS1's (docs/ui-design.md) ----------------------------------------------
 * Wherever the game draws one of its button icons (help text, tutorials, menus, the config screen), a 3DS one is
 * drawn instead: A for cross, B for circle, X for triangle, Y for square (the port's mapping), L / R for L1 / R1,
 * ZL / ZR for L2 / R2. The game says where its icons are (port_button_sprites); a sprite drawn from exactly that page,
 * CLUT, uv and size is one. Start, Select and the d-pad are the same on the 3DS and stay as they are. */
__attribute__((weak)) int port_button_sprites(int out[8][8]) { (void)out; return 0; }
static int btn_def[8][8], n_btn;
static C3D_Tex btn_tex[8];
static int btn_made[8];

static const u8 glyph5x7[][7] = {
    {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}, /* A */
    {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}, /* B */
    {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11}, /* X */
    {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04}, /* Y */
    {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}, /* L */
    {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}, /* R */
    {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F}, /* Z */
};

static void icon_glyph(u32 *px, int g, int x0, int y0, u32 c, int w, int h) {
    for (int y = 0; y < 7; y++)
        for (int x = 0; x < 5; x++)
            if (glyph5x7[g][y] >> (4 - x) & 1 && x0 + x >= 0 && x0 + x < w && y0 + y >= 0 && y0 + y < h)
                px[tiled(x0 + x, y0 + y, 32)] = c;
}

/* The system font's own button glyph (U+E000 A, E001 B, E002 X, E003 Y, E004 L, E005 R: the HOME menu's) for icon k,
 * box-filtered to w x h into px (light, as the 3DS draws them); 0 if the font hasn't it (an emulator's replacement
 * font). */
static int font_button(u32 *px, int k, int w, int h) {
    static int mapped = -1;
    if (mapped < 0) mapped = R_SUCCEEDED(fontEnsureMapped());
    if (!mapped || k > 5) return 0;
    CFNT_s *font = fontGetSystemFont();
    if (!font) return 0;
    int g = fontGlyphIndexFromCodePoint(font, 0xE000 + k);
    if (g < 0 || g == font->finf.alterCharIndex) return 0;
    TGLP_s *tg = fontGetGlyphInfo(font);
    if (!tg || tg->sheetFmt != GPU_A4) return 0;
    int per = tg->nRows * tg->nLines, sheet = g / per, i = g % per;
    const u8 *tex = fontGetGlyphSheetTex(font, sheet);
    int gx = (i % tg->nRows) * (tg->cellWidth + 1) + 1, gy = (i / tg->nRows) * (tg->cellHeight + 1) + 1;
    charWidthInfo_s *cw = fontGetCharWidthInfo(font, g);
    int gw = cw && cw->glyphWidth ? cw->glyphWidth : tg->cellWidth, gh = tg->cellHeight;
#define A4(x, y) (tex[tiled((unsigned)(gx + (x)), (unsigned)(gy + (y)), tg->sheetWidth) >> 1] >> \
                  ((tiled((unsigned)(gx + (x)), (unsigned)(gy + (y)), tg->sheetWidth) & 1) * 4) & 15)
    /* the glyph's own extent in its cell */
    int bx0 = gw, by0 = gh, bx1 = -1, by1 = -1;
    for (int y = 0; y < gh; y++)
        for (int x = 0; x < gw; x++)
            if (A4(x, y) > 3) {
                if (x < bx0) bx0 = x;
                if (x > bx1) bx1 = x;
                if (y < by0) by0 = y;
                if (y > by1) by1 = y;
            }
    if (bx1 < bx0) return 0;
    int ew = bx1 - bx0 + 1, eh = by1 - by0 + 1;
    /* to fit w x h, the same scale both ways (round buttons stay round), centred */
    float sc = (float)ew / w > (float)eh / h ? (float)ew / w : (float)eh / h;
    int ow = (int)(ew / sc + 0.5f), oh = (int)(eh / sc + 0.5f), ox = (w - ow) / 2, oy = (h - oh) / 2;
    const u32 light = 0xE8E8E880u, dark = 0x30303480u; /* (RGBA8 texels: r << 24 | g << 16 | b << 8 | a) */
    for (int y = 0; y < oh; y++)
        for (int x = 0; x < ow; x++) {
            int a = 0, n = 0;
            for (float sy = by0 + y * sc; sy < by0 + (y + 1) * sc; sy += 0.5f)
                for (float sx = bx0 + x * sc; sx < bx0 + (x + 1) * sc; sx += 0.5f) {
                    a += A4((int)sx, (int)sy);
                    n++;
                }
            if (n && a * 10 > n * 15 * 3) px[tiled((unsigned)(ox + x), (unsigned)(oy + y), 32)] = light; /* (>30% inked) */
        }
#undef A4
    if (k < 4) { /* a round button: its inside dark, so its rim and letter read as the console's */
        float cx = ox + (ow - 1) / 2.0f, cy = oy + (oh - 1) / 2.0f, rx = ow / 2.0f - 0.5f, ry = oh / 2.0f - 0.5f;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                float dx = (x - cx) / rx, dy = (y - cy) / ry;
                u32 *t = &px[tiled((unsigned)x, (unsigned)y, 32)];
                if (dx * dx + dy * dy < 0.85f && !*t) *t = dark;
            }
    }
    return 1;
}

/* the 3DS icon k (0-7: A B X Y L R ZL ZR) at w x h, RGBA8 in a 32x32 texture; opaque texels have alpha 0x80 (as a
 * PS1 texel with bit 15: drawn, in UI layers too). The system font's glyph when there is one, else drawn here. */
static void make_button_icon(int k, int w, int h) {
    if (!btn_made[k]) {
        if (!C3D_TexInit(&btn_tex[k], 32, 32, GPU_RGBA8)) return;
        C3D_TexSetFilter(&btn_tex[k], GPU_NEAREST, GPU_NEAREST);
        btn_made[k] = 1;
    }
    u32 *px = btn_tex[k].data;
    memset(px, 0, 32 * 32 * 4);
    if (font_button(px, k, w, h)) {
        mark_dirty(px, 32 * 32 * 4);
        return;
    }
#define RGBA(r, g, b) ((u32)(r) << 24 | (u32)(g) << 16 | (u32)(b) << 8 | 0x80u)
    static const u32 letter[4] = {RGBA(232, 72, 64), RGBA(240, 200, 56), RGBA(80, 144, 240), RGBA(72, 192, 104)};
    const u32 face = RGBA(48, 48, 52), rim = RGBA(176, 176, 184), text = RGBA(240, 240, 240);
    if (k < 4) { /* a round button with its coloured letter */
        float cx = (w - 1) / 2.0f, cy = (h - 1) / 2.0f, r = (w < h ? w : h) / 2.0f;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                float dx = x - cx, dy = y - cy, d2 = dx * dx + dy * dy;
                if (d2 <= r * r) px[tiled(x, y, 32)] = d2 >= (r - 1.2f) * (r - 1.2f) ? rim : face;
            }
        icon_glyph(px, k, (w - 5) / 2, (h - 7) / 2, letter[k], w, h);
    } else { /* a shoulder button: a rounded bar with L, R, ZL or ZR */
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                int corner = (x == 0 || x == w - 1) && (y == 0 || y == h - 1);
                if (!corner) px[tiled(x, y, 32)] = (x == 0 || x == w - 1 || y == 0 || y == h - 1) ? rim : face;
            }
        int z = k >= 6, g = (k & 1) ? 5 : 4, tw = z ? 11 : 5, x0 = (w - tw) / 2, y0 = (h - 7) / 2;
        if (z) icon_glyph(px, 6, x0, y0, text, w, h), x0 += 6;
        icon_glyph(px, g, x0, y0, text, w, h);
    }
#undef RGBA
    mark_dirty(px, 32 * 32 * 4);
}

/* each frame: where the game's button icons are (they come with its common images) */
static void button_icons_frame(void) {
    int def[8][8];
    int n = port_button_sprites(def);
    n_btn = 0;
    for (int k = 0; k < n; k++) {
        if (def[k][6] < 6 || def[k][6] > 32 || def[k][7] < 6 || def[k][7] > 32) return; /* (not loaded yet) */
        if (!btn_made[k] || memcmp(def[k], btn_def[k], sizeof def[k])) {
            memcpy(btn_def[k], def[k], sizeof def[k]);
            if (in_frame) gpu_sync();
            make_button_icon(k, def[k][6], def[k][7]);
        }
    }
    n_btn = n;
}

/* the 3DS icon a sprite stands for, -1 if it isn't a button icon */
static int button_icon(const Gp0Mode *m, int u, int v, int w, int h) {
    for (int k = 0; k < n_btn; k++) {
        const int *d = btn_def[k];
        /* (the message system draws them under another texture page: matched by CLUT, uv and size) */
        if (u == d[4] && v == d[5] && w == d[6] && h == d[7] && m->clut_x == d[2] && m->clut_y == d[3] && btn_made[k])
            return k;
    }
    return -1;
}

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
    int icon = m->textured ? button_icon(m, v->u, v->v, w, h) : -1;
    if (icon >= 0) { /* (its texels all "with bit 15": tflags 2) */
        Vtx t[6];
        quad(t, v->x, v->y, v->x + w, v->y + h, 0, 0, w, h, vcolor(m, v));
        emit(m, t, 6, v->x, v->y, v->x + w, v->y + h, &btn_tex[icon], 2);
        return;
    }
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
/* the card game's hands: drawn in the scene itself (menu_state 3), relative to their drawing area, last frame's */
static int scene_hands[CTR_MAX_HANDS][2], scene_nhands, scene_nhands_next;
int ctr_ui_active(void) { return shown_layer >= 0 || (menu_state == 3 && scene_nhands > 0); }
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
int port_game_mode(void);
void port_bottom_packets(const void *lo, const void *hi);
/* packets [lo, hi) of this frame are drawn on the bottom screen scaled by factor, (cx, cy) of them in the middle of
 * the screen (the world map's zoom: port/game/world_navi.c); factor 1: as they are */
void port_bottom_zoom(const void *lo, const void *hi, int cx, int cy, int factor) {
    zoom_lo = lo;
    zoom_hi = hi;
    zoom_cx = (float)cx;
    zoom_cy = (float)cy;
    zoom_factor = factor;
}
/* the windows' packets that stay on the top screen: drawn at the screen plane in stereo (box and text together) */
static struct { const u8 *lo, *hi; unsigned frame; } flat[8];
static int nflat;
void port_ui_packets(const void *lo, const void *hi) {
    /* In a field or on the world map, windows are speech bubbles and prompts anchored to the scene: they stay on the
     * top screen with the speaker (docs/ui-design.md), at the screen plane. Menus go to the bottom whatever the mode. */
    int mode = port_game_mode();
    if ((mode == 1 || mode == 3) && !menu_state) {
        for (int i = 0; i < nflat; i++) /* (the buffer was reused, or two presents went by: a past frame's) */
            if (((const u8 *)lo < flat[i].hi && (const u8 *)hi > flat[i].lo) || frames_drawn - flat[i].frame > 2)
                flat[i] = flat[--nflat], i--;
        if (nflat == 8) nflat--;
        flat[nflat].lo = lo;
        flat[nflat].hi = hi;
        flat[nflat].frame = frames_drawn;
        nflat++;
        return;
    }
    port_bottom_packets(lo, hi);
}
/* packets [lo, hi) of the frame's buffer are drawn on the bottom screen (the world map's navigation map too) */
void port_bottom_packets(const void *lo, const void *hi) {
    for (int i = 0; i < nranges; i++) /* (the buffer was reused: a range there is a past frame's) */
        if ((const u8 *)lo < ranges[i].hi && (const u8 *)hi > ranges[i].lo) ranges[i] = ranges[--nranges], i--;
    if (nranges == 8) nranges--;
    ranges[nranges].lo = lo;
    ranges[nranges].hi = hi;
    ranges[nranges].hit = ranges[nranges].age = 0;
    nranges++;
}
extern const u32 *psx_gpu_packet;
static int flat_packet(void) {
    const u8 *p = (const u8 *)psx_gpu_packet;
    for (int i = 0; i < nflat; i++)
        if (p >= flat[i].lo && p < flat[i].hi) return 1;
    return 0;
}
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
    if (k < 0 && menu_state == 3) {
        int dx, dy, w, h;
        psx_gpu_draw_area(&dx, &dy, &w, &h);
        if (scene_nhands_next < CTR_MAX_HANDS) {
            scene_hands[scene_nhands_next][0] = x - dx;
            scene_hands[scene_nhands_next][1] = y - dy;
            scene_nhands_next++;
        }
        return;
    }
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

/* the same, drawing over what the layer has this frame (the battle screen's own drawing after the battle list) */
void psx_ui_list_begin_keep(void) {
    submit();
    PsxDisplayInfo d;
    psx_gpu_display_info(&d);
    ui_mode = 1;
    ui_ox = d.x;
    ui_oy = d.y;
    ui_list_layer = ui_layer_for(d.x, d.y, 0);
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
    C3D_DrawArrays(GPU_TRIANGLES, v - vb, 6);
}

/* The top screen shows the frame as large as it fits whole: scaled to the screen's 240 lines (a 320x224 field or
 * battle becomes 343x240), the same in both directions, centred; filtered, as the scale isn't whole. */
static float top_scale(int w, int h) {
    float s = 240.0f / (float)h;
    if (w * s > 400.0f) s = 400.0f / (float)w;
    return s < 1.0f ? 1.0f : s;
}
static void draw_to_top(C3D_RenderTarget *rt, C3D_Tex *src, int sx, int sy, int w, int h, int fw, int fh, int edge) {
    float s = top_scale(fw, fh);
    int dw = (int)(fw * s + 0.5f), dh = (int)(fh * s + 0.5f), e = (int)(edge * s + 0.5f);
    C3D_RenderTargetClear(rt, C3D_CLEAR_ALL, 0x000000FF, 0);
    C3D_FrameDrawOn(rt);
    if (s > 1.0f) C3D_TexSetFilter(src, GPU_LINEAR, GPU_LINEAR);
    screen_quad_scaled(rt, src, K_BLIT, NO_BLEND, 0xFFFFFFFF, sx, sy, w, h, (400 - dw) / 2 + e, (240 - dh) / 2, dw - 2 * e,
                       dh);
    if (s > 1.0f) C3D_TexSetFilter(src, GPU_NEAREST, GPU_NEAREST);
}

/* what is on a screen's render target (already drawn on), darkened to frac (0-255 of 256): destination x constant */
static void screen_dim(C3D_RenderTarget *rt, unsigned frac) {
    C3D_Mtx p;
    Mtx_OrthoTilt(&p, 0.0f, rt == bottom_rt ? 320.0f : 400.0f, 240.0f, 0.0f, 0.0f, 1.0f, true);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, u_proj, &p);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, u_stereo, 0, 0, 1, 0.5f);
    set_tev(K_UNTEX);
    C3D_BlendingColor(0xFF000000u | frac << 16 | frac << 8 | frac);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ZERO, GPU_CONSTANT_COLOR, GPU_ZERO, GPU_ONE);
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    Vtx *v = alloc_vtx(6);
    quad(v, 0, 0, rt == bottom_rt ? 320 : 400, 240, 0, 0, 0, 0, 0xFFFFFFFF);
    C3D_DrawArrays(GPU_TRIANGLES, v - vb, 6);
}


__attribute__((weak)) int port_game_mode(void) { return 0; }

/* The bottom screen: the UI layer of the displayed buffer, on a plain backdrop (the scene is on the top screen only). */
int ctr_ui_oy;
static int bottom_layer(const PsxDisplayInfo *d) {
    int k = layers[0].x == d->x && layers[0].y == d->y ? 0 : layers[1].x == d->x && layers[1].y == d->y ? 1 : -1;
    if (menu_state == 3) return -1;
    if (k >= 0 && frames_drawn - layers[k].drawn > 2) k = -1; /* (nothing drawn into it lately) */
    return k;
}
/* ---- the top screen's UI list: the port's own drawing (menus' detail views: port/game/field_hud.c), built with the
 * game's window and text code as the bottom screen's, decoded onto the top screen after its scene each present ---- */
static u32 *top_ui_head;
static const void *top_ui_buf;
static unsigned top_ui_size;
void ctr_top_ui(unsigned *head, const void *buf, unsigned size) {
    top_ui_head = (u32 *)head;
    top_ui_buf = buf;
    top_ui_size = size;
}

/* ---- images of the port's own on the bottom screen (the field screen's portraits: port/game/field_hud.c) ---- */
#define HUD_IMAGES 8
static struct { C3D_Tex tex; u32 key; int ok; } hud_img[HUD_IMAGES];
static struct { int slot, x, y, w, h, dw, dh; } hud_draw[16];
static int n_hud_draw;

/* image slot: w x h (<= 64x64) 8-bit pixels through a CLUT of PS1 colours, kept while key is the same */
void ctr_hud_image(int slot, u32 key, const u16 *clut, const u8 *pix, int w, int h) {
    if (slot < 0 || slot >= HUD_IMAGES || w > 64 || h > 64) return;
    if (hud_img[slot].ok && hud_img[slot].key == key) return;
    if (!hud_img[slot].ok) {
        if (!C3D_TexInit(&hud_img[slot].tex, 64, 64, GPU_RGBA8)) return;
        C3D_TexSetFilter(&hud_img[slot].tex, GPU_NEAREST, GPU_NEAREST);
        hud_img[slot].ok = 1;
    }
    if (in_frame) gpu_sync(); /* (it may be drawn with in this frame's list) */
    u32 *dst = hud_img[slot].tex.data;
    memset(dst, 0, 64 * 64 * 4);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) dst[tiled(x, y, 64)] = rgba_lut[clut[pix[y * w + x]]];
    mark_dirty(dst, 64 * 64 * 4);
    hud_img[slot].key = key;
}

/* draw image slot at (x, y) of the bottom screen's UI (its 320x224 coordinates) this frame, w x h of it */
void ctr_hud_draw(int slot, int x, int y, int w, int h) {
    if (slot < 0 || slot >= HUD_IMAGES || !hud_img[slot].ok || n_hud_draw == 16) return;
    hud_draw[n_hud_draw].slot = slot;
    hud_draw[n_hud_draw].x = x; hud_draw[n_hud_draw].y = y;
    hud_draw[n_hud_draw].w = w; hud_draw[n_hud_draw].h = h;
    hud_draw[n_hud_draw].dw = w; hud_draw[n_hud_draw].dh = h;
    n_hud_draw++;
}

/* the same on the top screen (its 400x240), scaled to dw x dh */
void ctr_hud_draw_top(int slot, int x, int y, int w, int h, int dw, int dh) {
    ctr_hud_draw(slot, x + 1000, y, w, h);
    if (n_hud_draw && hud_draw[n_hud_draw - 1].x == x + 1000) hud_draw[n_hud_draw - 1].dw = dw, hud_draw[n_hud_draw - 1].dh = dh;
}

static void present_bottom(const PsxDisplayInfo *d, int w, int h) {
    int k = shown_layer;
    ctr_ui_oy = (240 - h) / 2;
    C3D_RenderTargetClear(bottom_rt, C3D_CLEAR_ALL, 0x000000FF, 0);
    C3D_FrameDrawOn(bottom_rt);
    if (d->rgb24) return;
    if (menu_state == 3) { /* the card game: the frame itself, to touch */
        screen_quad(bottom_rt, &mirror_tex[0], K_SCENE, NO_BLEND, 0xFF808080, d->x, d->y, w, h, 0, ctr_ui_oy);
        if (scene_nhands_next) { /* (a frame that drew its hands: the latest) */
            scene_nhands = scene_nhands_next;
            memcpy(ctr_ui_hands, scene_hands, sizeof ctr_ui_hands);
            scene_nhands_next = 0;
        }
        ctr_ui_nhands = scene_nhands;
        return;
    }
    if (k < 0) { ctr_ui_nhands = 0; return; }
    screen_quad(bottom_rt, &layers[k].tex, K_COMPOSITE, 5, 0xFFFFFFFF, 0, 0, w, h, 0, ctr_ui_oy);
    for (int i = 0; i < n_hud_draw; i++) /* (texels x 2 x grey: as drawn; transparent ones not) */
        if (hud_draw[i].x < 1000) screen_quad(bottom_rt, &hud_img[hud_draw[i].slot].tex, K_TEX, NO_BLEND, 0xFF808080, 0, 0, hud_draw[i].w,
                    hud_draw[i].h, hud_draw[i].x, hud_draw[i].y + ctr_ui_oy);
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
    C3D_Tex *t = staging_tex(px, tw, th, GPU_RGB8);
    for (int eye = 0; eye < (stereo_on ? 2 : 1); eye++) draw_to_top(top_rt[eye], t, 0, 0, w, h, w, h, 0);
    (void)ox; (void)oy;
}

unsigned ctr_frames_presented(void) { return frames_drawn; }

void ctr_present(long frame) {
    (void)frame;
    submit(); /* (with the parallax its runs were ordered for: stereo_k changes below) */
    button_icons_frame(); /* (for the next frame's sprites) */
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
        /* the scene kept for the top while a menu is up: copied every frame without one (a menu opens with a fade,
         * which a copy taken then would hold), frozen when one opens */
        int want_menu = menu_state == 1 || menu_state == 2;
        if (!menu_state && w >= 256) {
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
                C3D_DrawArrays(GPU_TRIANGLES, v - vb, 6);
            }
            freeze_w = w;
            freeze_h = h;
            have_freeze = 1;
        }
        if (want_menu && have_freeze) frozen = 1;
        else if (!menu_state || menu_state == 3) frozen = 0;
        /* the card game: the board is on the bottom screen; the top magnifies (2x) where the game's cursor is, so
         * the card it points at (its art, arrows and stats) is large and readable */
        static float mag_x = -1, mag_y;
        int mag_rx = 0, mag_ry = 0;
        if (menu_state == 3) {
            float tx = w / 2.0f, ty = h / 2.0f;
            if (scene_nhands > 0) tx = scene_hands[0][0] + 32.0f, ty = scene_hands[0][1] + 8.0f;
            if (mag_x < 0) mag_x = tx, mag_y = ty;
            mag_x += (tx - mag_x) * 0.3f;
            mag_y += (ty - mag_y) * 0.3f;
            mag_rx = clampi((int)mag_x - MAG_W / 2, 0, w - MAG_W);
            mag_ry = clampi((int)mag_y - MAG_H / 2, 0, h - MAG_H);
        } else mag_x = -1;
        for (int eye = 0; eye < (stereo_on ? 2 : 1); eye++) {
            if (menu_state == 3) {
                C3D_RenderTargetClear(top_rt[eye], C3D_CLEAR_ALL, 0x000000FF, 0);
                C3D_FrameDrawOn(top_rt[eye]);
                screen_quad_scaled(top_rt[eye], &mirror_tex[0], K_BLIT, NO_BLEND, 0xFFFFFFFF, d.x + mag_rx, d.y + mag_ry, MAG_W,
                                   MAG_H, 0, 0, 400, 240);
            } else if (frozen) { /* a menu is up: the paused scene, dimmed to half (its panels are drawn over it after) */
                draw_to_top(top_rt[eye], &freeze_tex[eye], edge, 0, freeze_w - 2 * edge, freeze_h, freeze_w, freeze_h, edge);
                screen_dim(top_rt[eye], 0x80);
            }
            else draw_to_top(top_rt[eye], &mirror_tex[eye], d.x + edge, d.y, w - 2 * edge, h, w, h, edge);
        }
    }
    if (top_ui_head) { /* (over the top screen's scene, both eyes) */
        void port_draw_ot_buffer(unsigned int *head, const void *buffer, unsigned int size);
        submit();
        Gp0State saved = gp0_state;
        ui_mode = 2;
        port_draw_ot_buffer((unsigned *)top_ui_head, top_ui_buf, top_ui_size);
        submit();
        ui_mode = 0;
        gp0_state = saved;
        top_ui_head = NULL;
        for (int i = 0; i < n_hud_draw; i++) /* (the images meant for the top: x >= 1000) */
            if (hud_draw[i].x >= 1000)
                for (int eye = 0; eye < (stereo_on ? 2 : 1); eye++) {
                    C3D_FrameDrawOn(top_rt[eye]);
                    screen_quad_scaled(top_rt[eye], &hud_img[hud_draw[i].slot].tex, K_TEX, NO_BLEND, 0xFF808080, 0, 0,
                                       hud_draw[i].w, hud_draw[i].h, hud_draw[i].x - 1000, hud_draw[i].y,
                                       hud_draw[i].dw, hud_draw[i].dh);
                }
    }
    present_bottom(&d, w, h);
    n_hud_draw = 0;
    frame_end();
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
    AttrInfo_AddLoader(&attr, 3, GPU_FLOAT, 1);

    vb = linearAlloc(VB_MAX * sizeof(Vtx));
    staging = linearAlloc(STAGING_SIZE);
    for (int eye = 0; eye < 2; eye++) {
        if (!C3D_TexInitVRAM(&mirror_tex[eye], VW, VH, GPU_RGBA5551)) port_fatal("no VRAM for the PS1 VRAM mirror");
        C3D_TexSetFilter(&mirror_tex[eye], GPU_NEAREST, GPU_NEAREST);
        mirror_rt[eye] = C3D_RenderTargetCreateFromTex(&mirror_tex[eye], GPU_TEXFACE_2D, 0, -1);
        if (eye == 0) mirror_depth = vramAlloc(VW * VH * 2);
        if (mirror_depth) C3D_FrameBufDepth(&mirror_rt[eye]->frameBuf, mirror_depth, GPU_RB_DEPTH16);
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
    if (mirror_depth) C3D_RenderTargetClear(mirror_rt[0], C3D_CLEAR_DEPTH, 0, 0);
    port_log("renderer: PICA200, %u KB linear, %u KB VRAM free%s\n", (unsigned)(linearSpaceFree() >> 10),
             (unsigned)(vramSpaceFree() >> 10), mirror_depth ? "" : " (no depth for the mirror: runs not reordered)");
}

void ctr_shutdown(void) {
    void ctr_audio_exit(void);
    ctr_async_exit();
    void ctr_spu_exit(void);
    ctr_spu_exit();
    ctr_voice_exit();
    ctr_audio_exit();
    if (in_frame) frame_end();
    C3D_Fini();
    gfxExit();
}
#endif
