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
#include "gte_depth.h"
#include "psx_runtime.h"
#include "prim_shbin.h"

extern unsigned short *psx_vram;

/* The scene's walk may be on the other core (gpu.c psx_gpu_sync): every way in here from outside the walk waits for it
 * first, or (the packet ranges the port reports while the next frame is built) is kept and made after it, in order
 * (psx_gpu_stage, psx_gpu_staged). */
void psx_gpu_sync(void);
int psx_gpu_stage(int kind, const int *args, int nargs, const void *data, unsigned int bytes);
enum { STG_TERRAIN, STG_SHIFT, STG_ZOOM, STG_MODEL, STG_UI, STG_BOTTOM };

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

/* (sizes from the peaks measured (Castle Station, a 24-bit movie field, Ifrit, the world map and a battle swirl, Burmecia's
 * palace: at most 9150 vertices and 896 KB of staging a frame); past them a frame waits for the GPU and goes on. Larger,
 * they took 8 MB of linear memory the texture cache needs: it stopped at 67 pages, and the palace's and South Gate's
 * scenes, using more, converted pages again every frame (24-27 ms a frame on a 3DS) */
#define VB_MAX (24 * 1024)
static Vtx *vb;
static unsigned vb_used, vb_flushed;

#define STAGING_SIZE (1024 * 1024)
static u8 *staging;
static unsigned staging_used, staging_flushed;

/* Both in two halves, a frame's in one: the GPU draws a frame from its half while the CPU records the next into the
 * other (in stereo the GPU's frame takes most of a VBlank: waiting for it before recording held the game below
 * speed). A texture the CPU rewrites waits for the frame in flight only when that frame draws with it. */
static Vtx *vb_half[2];
static u8 *staging_half[2];
static int half;
static int gpu_busy; /* a frame sent and not waited for (frame_begin waits) */

/* The GPU reads main memory, not the CPU's data cache, and only once a command list is sent (C3D_FrameEnd): what the
 * CPU wrote for it is flushed then, each buffer once (a flush is a request to the GSP service: one per vertex quad or
 * texture block cost more than the drawing). Vertices and staging grow contiguously; texture blocks are listed. */
#define MAX_DIRTY 64
static struct { u8 *p; unsigned n; } dirty[MAX_DIRTY];
static unsigned ndirty;
unsigned ctr_flushes, ctr_batch_why[6], ctr_kind_pairs[8][8];
static void flush_range(void *p, unsigned n) {
    if (!n) return;
    /* (the kernel's flush, as audio.c's: GSPGPU_FlushDataCache asks the GSP service, which on an Old 3DS runs on the
     * second core in the system's share of it, and waits for its turn there) */
    svcFlushProcessDataCache(CUR_PROCESS_HANDLE, (u32)p, n);
    ctr_flushes++;
}
static void flush_pending(void);
/* HAZARD=1 (test builds): what the GPU reads in a frame it may still be drawing (the frame sent at a present, drawn
 * while the next is recorded) against what the CPU writes meanwhile. On hardware such a write changes what that frame
 * shows; an emulator draws a frame at once, so it shows nothing there. Each one is logged: "hazard: ..." */
#ifdef PORT_HAZARD
#define HZ_N 160
typedef struct { const u8 *p; unsigned n; const char *what; } HzUse;
static HzUse hz_rec[HZ_N], hz_fly[HZ_N];
static int hz_nrec, hz_nfly;
unsigned ctr_hazards;
static void hz_use(const void *p, unsigned n, const char *what) {
    if (!p || !n) return;
    for (int i = 0; i < hz_nrec; i++)
        if (hz_rec[i].p == (const u8 *)p) { if (hz_rec[i].n < n) hz_rec[i].n = n; return; }
    if (hz_nrec < HZ_N) hz_rec[hz_nrec].p = p, hz_rec[hz_nrec].n = n, hz_rec[hz_nrec++].what = what;
}
static void hz_check(const void *p, unsigned n, const char *how) {
    if (!gpu_busy) return;
    const u8 *b = p;
    for (int i = 0; i < hz_nfly; i++)
        if (b < hz_fly[i].p + hz_fly[i].n && b + n > hz_fly[i].p) {
            ctr_hazards++;
            if (ctr_hazards <= 40 || ctr_hazards % 500 == 0)
                port_log("hazard %u: %s %p+%u over in-flight %s %p+%u\n", ctr_hazards, how, (void *)b, n, hz_fly[i].what,
                         (void *)hz_fly[i].p, hz_fly[i].n);
            return;
        }
}
static void hz_sent(void) { /* (a present: the recorded frame is the one in flight now) */
    for (int i = 0; i < hz_nrec; i++) hz_fly[i] = hz_rec[i];
    hz_nfly = hz_nrec;
    hz_nrec = 0;
}
static void hz_done(void) { hz_nfly = hz_nrec = 0; } /* (the GPU finished all that was sent) */
#define HZ_TEX(t, what) do { if (t) hz_use((t)->data, (t)->size, what); } while (0)
#else
#define hz_use(p, n, w) ((void)0)
#define hz_check(p, n, h) ((void)0)
#define hz_sent() ((void)0)
#define hz_done() ((void)0)
#define HZ_TEX(t, what) ((void)0)
#endif
static inline void tex_bind(int unit, C3D_Tex *t) {
    HZ_TEX(t, "texture");
    C3D_TexBind(unit, t);
}
static void mark_dirty(void *p, unsigned n) {
    hz_check(p, n, "cpu write");
    u8 *b = p;
    for (unsigned i = 0; i < ndirty; i++) { /* (adjacent blocks of one texture: one range) */
        if (b >= dirty[i].p && b + n <= dirty[i].p + dirty[i].n) return; /* (the same tile rows, another block of them) */
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
/* the last GPU-side steps (the watchdog prints them when the GPU never finishes a frame) */
#define TRACE_N 48
static struct { const char *what; int a, b; unsigned frame; } trace_ring[TRACE_N];
static volatile unsigned trace_n;
static unsigned frames_drawn;
static void trace(const char *what, int a, int b) {
    unsigned i = trace_n++ % TRACE_N;
    trace_ring[i].what = what, trace_ring[i].a = a, trace_ring[i].b = b, trace_ring[i].frame = frames_drawn;
}
volatile int ctr_trace_quit; /* (the watchdog's dump cut short: the application is closing, platform.c) */
void ctr_trace_dump(void) { /* (platform.c's watchdog, once) */
    void ctr_console_raw(const char *, int);
    char line[96];
    unsigned n = trace_n, first = n > TRACE_N ? n - TRACE_N : 0;
    for (unsigned k = first; k < n && !ctr_trace_quit; k++) {
        unsigned i = k % TRACE_N;
        int len = snprintf(line, sizeof line, "  trace %u: %s %d %d (frame %u)\n", k, trace_ring[i].what, trace_ring[i].a, trace_ring[i].b, trace_ring[i].frame);
        ctr_console_raw(line, len);
    }
}
/* ---- the GPU's cost (PROFILE): what each part of a frame draws, and on a 3DS how long the GPU takes for it --------
 * The frame's command list is sent in parts (mirror, UI layers, outline masks, top-screen UI list, the frozen scene's
 * copy, the top screen, the bottom screen, the end), each counted as it is recorded (pixels by its triangles' area,
 * vertices, draw calls); the GPU's "command list done" event (P3D, which citro3d leaves alone) times each part. Azahar
 * takes no time for the GPU: there the counts with costs fitted on a 3DS estimate it (ctr_gpu_cost_report) */

static DVLB_s *shader_dvlb;
static shaderProgram_s shader;
static int u_proj, u_stereo, u_texscale;

static C3D_Tex mirror_tex[2];
static C3D_RenderTarget *mirror_rt[2];
static C3D_RenderTarget *top_rt[2];
static int top_queue(int stereo); /* (the top screen's copy: three buffers, ctr_video_init) */
int port_rewards_skipped(void); /* (rewards_top.c) */
int port_field_current(void); /* (field_hud.c: the field's data loaded) */
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
    int dx0, dy0, dx1, dy1; /* where anything was drawn since it was last cleared (empty: dx0 >= dx1) */
    int clear_pending;      /* to be cleared before the next thing drawn into it (ui_layer_for, layer_clear) */
    unsigned clears;        /* times it was cleared (ctr_ui_layer_serial) */
    unsigned draws;         /* primitives drawn into it (ctr_ui_layer_serial) */
} UiLayer;
static UiLayer layers[2];
static inline int imin(int a, int b);
static inline int imax(int a, int b);
static void layer_clear(int k);
/* while a menu is up, the top screen shows the scene as it was when the menu was asked for (per eye) */
static C3D_Tex freeze_tex[2];
static C3D_RenderTarget *freeze_rt[2];
static int freeze_w, freeze_h, frozen;
int ctr_freeze_held; /* the menu snapshot kept as it is (battle_ui.c: from the battle's exit fade on) */
#define MAG_W 200 /* the card game's magnified area (to 400x240 on the top screen) */
#define MAG_H 120
static int menu_state; /* port_menu_state(), read at each present */
static int have_freeze; /* freeze_tex holds a scene */
#define UNDIM_FRAMES 12 /* a closed menu's paused scene undimmed over these presents */
static int prev_mode;
static int prev_menu, closing; /* (ctr_present: the menu at the last present; a closed menu's undimming, in presents) */
static u32 present_gen;        /* gen_counter at the last present (cells drawn since: cell_gen past it) */
int port_game_mode(void);
int port_fade_busy(void);
__attribute__((weak)) int port_menu_state(void) { return 0; }
static C3D_RenderTarget *bottom_rt;

static int in_frame;
static unsigned frame_serial = 1; /* bumped at each C3D frame: what was recorded since */
static int stereo_on;            /* eye 1 drawn as well */
static float stereo_k;           /* parallax constant of eye 1 (eye 0 gets -k) */

/* (fitted on an Old 3DS's battle, ms each (the same GPU on a New 3DS): a command list (its sending and the GSP's
 * latency included), a pixel (more in the mirror: depth, blending, the mask and dither stages), a vertex. Within 0.4 ms
 * of each part's measured time there (ff9-test-gpucal, 2026-11-15)) */
#define GPU_COST_LIST 0.50
#define GPU_COST_PX 0.0000090
#define GPU_COST_PX_MIRROR 0.0000039
#define GPU_COST_VX 0.00098
#define GPU_COST_END 2.0 /* (a frame's end: its last list, the screens' copies) */
static gxCmdQueue_s *c3d_queue; /* (citro3d's: GX_BindQueue wrapped, -Wl,--wrap) */
enum { GS_MIRROR, GS_LAYERS, GS_OUTLINE, GS_TOPUI, GS_FREEZE, GS_TOP, GS_BOTTOM, GS_END, GS_N };
#ifdef PORT_PROFILE
static const char *const gs_name[GS_N] = {"mirror", "layers", "outline", "topui", "freeze", "top", "bottom", "end"};
static double gs_px[GS_N], gs_vx[GS_N], gs_dc[GS_N], gs_ms[GS_N], gs_lists[GS_N];
static double gs_frame_est; /* this present's estimated GPU time so far (ms) */
static unsigned gs_ends;    /* frames ended in the window */
static double gs_wait_est;  /* the window's estimated waits for the GPU (ms) */
static double cur_px, cur_vx, cur_dc;
/* (each list's place in citro3d's GX queue, which counts the entries the GPU has finished (lastEntry): stamped as that
 * count rises, at the GSP's list, transfer and fill events; a list's time is from its start (sent, or the entry before
 * finished) to its end) */
#define GQ 256
static volatile u64 gq_sent[GQ], gq_done[GQ];
static volatile s8 gq_seg[GQ]; /* -1: not one of the parts (a transfer, a fill, a list sent elsewhere) */
static volatile unsigned gq_stamped;
static volatile double gq_ms[GS_N];
static volatile unsigned gq_odd; /* entries finished before they were sent: the pairing was off */
static void gq_event(void *unused) { /* (the GSP's event thread, after the queue has taken the event) */
    (void)unused;
    if (!c3d_queue) return;
    unsigned last = c3d_queue->lastEntry;
    if (last < gq_stamped) gq_stamped = 0; /* (the queue cleared at a frame's begin) */
    u64 now = svcGetSystemTick();
    for (unsigned e = gq_stamped; e < last && e < GQ; e++) {
        gq_done[e] = now;
        if (gq_seg[e] < 0) continue;
        u64 start = gq_sent[e];
        if (e && gq_done[e - 1] > start) start = gq_done[e - 1];
        if (now < start) gq_odd++;
        else gq_ms[gq_seg[e]] += (now - start) / (double)SYSCLOCK_ARM11 * 1000.0;
        gq_seg[e] = -1;
    }
    gq_stamped = last;
}
static void gq_sending(int seg) { /* the list about to be added to the queue is part seg */
    static int hooked;
    if (!hooked) {
        hooked = 1;
        memset((void *)gq_seg, -1, sizeof gq_seg);
        gspSetEventCallback(GSPGPU_EVENT_P3D, gq_event, NULL, false);
        gspSetEventCallback(GSPGPU_EVENT_PPF, gq_event, NULL, false);
        gspSetEventCallback(GSPGPU_EVENT_PSC0, gq_event, NULL, false);
        gspSetEventCallback(GSPGPU_EVENT_PSC1, gq_event, NULL, false);
    }
    if (!c3d_queue) return;
    unsigned e = c3d_queue->numEntries;
    if (e >= GQ) return;
    gq_sent[e] = svcGetSystemTick();
    gq_seg[e] = (s8)seg;
}
static void gq_collect(void) {
    for (int g = 0; g < GS_N; g++) gs_ms[g] += gq_ms[g], gq_ms[g] = 0;
}
static void gs_take(int seg) {
    if (cur_dc) {
        gs_lists[seg]++;
        gs_frame_est += GPU_COST_LIST + GPU_COST_VX * cur_vx + (GPU_COST_PX + (seg == GS_MIRROR ? GPU_COST_PX_MIRROR : 0)) * cur_px;
    }
    gs_px[seg] += cur_px, gs_vx[seg] += cur_vx, gs_dc[seg] += cur_dc, cur_px = cur_vx = cur_dc = 0;
}
/* (Azahar's GPU takes no time: the wait a 3DS would have, from the estimate. A present's GPU work runs from its end;
 * the next frame_begin waits for what is left of it) */
static u64 gs_present_t;
static double gs_present_est;
static void gs_presented(void) { gs_present_t = svcGetSystemTick(), gs_present_est = gs_frame_est, gs_frame_est = 0; }
static void gs_beginning(void) {
    if (!gs_present_t) return;
    double since = (svcGetSystemTick() - gs_present_t) / (double)SYSCLOCK_ARM11 * 1000.0;
    if (gs_present_est > since) gs_wait_est += gs_present_est - since;
    gs_present_t = 0;
}
bool C3Di_SplitFrame(u32 **cmdBuf, u32 *cmdBufSize); /* (citro3d's, what C3D_FrameSplit sends) */
static void gseg(int seg) { /* what was recorded since the last part: sent as a list of its own */
    if (!in_frame || !cur_dc) { gs_take(seg); return; }
    flush_pending();
    u32 *buf, size;
    gs_take(seg);
    if (C3Di_SplitFrame(&buf, &size)) gq_sending(seg), GX_ProcessCommandList(buf, size * 4, GX_CMDLIST_FLUSH);
}
/* (the pixels from every 8th frame's vertices, times 8: every vertex drawn read again, in doubles, was 0.16 ms of field
 * 1450's VBlank in Azahar as an Old 3DS (more on a 3DS, the vertices out of its cache): the profile's own cost, in its
 * submit times) */
static void gs_count(unsigned first, unsigned n) {
    cur_vx += n, cur_dc += 1;
    if (frames_drawn & 7) return;
    const Vtx *v = vb + first;
    float a = 0;
    for (unsigned i = 0; i + 2 < n; i += 3)
        a += fabsf((v[i + 1].x - v[i].x) * (v[i + 2].y - v[i].y) - (v[i + 2].x - v[i].x) * (v[i + 1].y - v[i].y)) * 0.5f;
    cur_px += 8 * (double)a;
}
/* the window's GPU time per part and frame, measured (a 3DS) and estimated from the counts */
void ctr_gpu_cost_report(unsigned frames) {
    psx_gpu_sync();
    gq_collect();
    char line[512];
    int n = 0;
    double est = 0;
    for (int g = 0; g < GS_N && n < (int)sizeof line - 64; g++) {
        if (!gs_dc[g] && !gs_ms[g]) continue;
        double e = gs_dc[g] ? GPU_COST_LIST * gs_lists[g] + GPU_COST_VX * gs_vx[g] +
                                  (GPU_COST_PX + (g == GS_MIRROR ? GPU_COST_PX_MIRROR : 0)) * gs_px[g] : 0;
        est += e;
        n += snprintf(line + n, sizeof line - n, " %s %.2f/%.2f %.0fk %.0f %.0f |", gs_name[g], gs_ms[g] / frames, e / frames,
                      gs_px[g] / frames / 1000, gs_vx[g] / frames, gs_dc[g] / frames);
        gs_ms[g] = gs_px[g] = gs_vx[g] = gs_dc[g] = gs_lists[g] = 0;
    }
    est += GPU_COST_END * gs_ends;
    extern unsigned int ctr_render_stats[8];
    static unsigned last_light;
    port_log("[gpuc] ms measured/estimated, kpixels, vertices, draws per frame:%s est %.1f, wait est %.1f, light presents %u (odd %u)\n",
             line, est / frames, gs_wait_est / frames, ctr_render_stats[7] - last_light, gq_odd);
    last_light = ctr_render_stats[7];
    gs_wait_est = 0, gs_ends = 0;
}
#define GSEG(seg) gseg(seg)
#else
#define GSEG(seg) ((void)0)
#define gs_presented() ((void)0)
#define gs_beginning() ((void)0)
#endif
static inline void draw_tris(unsigned first, unsigned n) {
#ifdef PORT_PROFILE
    gs_count(first, n);
#endif
    C3D_DrawArrays(GPU_TRIANGLES, first, n);
}
static void frame_end(void) {
    trace("frame_end", vb_used, (int)staging_used);
    flush_pending();
#ifdef PORT_PROFILE
    gs_take(GS_END);
    gq_sending(GS_END); /* (C3D_FrameEnd sends what is left as a list) */
    gs_frame_est += GPU_COST_END, gs_ends++;
#endif
    C3D_FrameEnd(GX_CMDLIST_FLUSH) /* (we flush what we write ourselves: not the whole linear heap) */;
}
static float stereo_inv_zc = 1.0f / 1500.0f;
#define STEREO_MAX_PX 8.0f
static unsigned display_shown_gen = ~0u;
unsigned int ctr_render_stats[8];
unsigned int ctr_depth_stats[2]; /* vertices with / without GTE depth */ /* batches, primitives, blits, readbacks, texture blocks, slots, syncs */

/* ---- cell bookkeeping ------------------------------------------------------------------------------------- */

static u8 shadow_ok[CH][CW]; /* the shadow holds the cell's current pixels */
static u8 mirror_ok[CH][CW]; /* the mirror (both eyes) holds them */
/* A cell the GPU owned that CPU writes covered in part, each over its whole height: the columns written (bit i =
 * column i), held in the shadow, the others still only in the mirror (neither side ok). Read back (those other columns
 * only) when anything needs the cell before writes have covered it whole. A movie background's frames are loaded in
 * slices 16 pixels wide over the field the GPU drew: each slice read its cells back first, 320 readbacks a frame,
 * each a wait for the GPU, though the next slice wrote the rest */
static u32 cell_cols[CH][CW];
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
/* (the same cells as the call before, none of them a texture's source and nothing done to any cell's state since
 * (cells_ver): that call's work again would change only gen_counter, which no stamp taken since can tell (a block is
 * stamped against its sources' cells, none of these): it is skipped. The world map's ground draws small triangles one
 * after another in the same cells) */
static unsigned cells_ver;
static void gpu_draws(int x0, int y0, int x1, int y1) {
    static int last_cells = -1;
    static unsigned last_ver;
    int cx0, cy0, cx1, cy1;
    if (!cell_range(x0, y0, x1, y1, &cx0, &cy0, &cx1, &cy1)) return;
    const int cells = cx0 | cy0 << 6 | cx1 << 12 | cy1 << 18;
    if (cells == last_cells && last_ver == cells_ver) return;
    for (int cy = cy0; cy < cy1; cy++)
        for (int cx = cx0; cx < cx1; cx++)
            if (!mirror_ok[cy][cx]) { blit_cells(cx0, cy0, cx1, cy1); goto ok; }
ok:
    gen_counter++;
    int source = 0;
    for (int cy = cy0; cy < cy1; cy++)
        for (int cx = cx0; cx < cx1; cx++) {
            shadow_ok[cy][cx] = 0;
            cell_gen[cy][cx] = gen_counter;
            if (cell_is_source[cy][cx]) tex_epoch++, source = 1;
        }
    last_cells = source ? -1 : cells;
    last_ver = cells_ver;
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
/* gpu.c: a fill's cells it covers whole stay the GPU's alone (drawn there by psx_vram_filled): the CPU doesn't fill
 * its copy of them, which is read back if the CPU ever reads them (a field's frame, cleared every frame, isn't) */
static int fill_kept;
int psx_vram_fill_kept(int x, int y, int w, int h, int *ix0, int *iy0, int *ix1, int *iy1) {
    psx_gpu_sync();
    if (!filling || x < 0 || y < 0 || x + w > VW || y + h > VH) return 0;
    const int a0 = (x + CELL - 1) / CELL * CELL, b0 = (y + CELL - 1) / CELL * CELL;
    const int a1 = (x + w) / CELL * CELL, b1 = (y + h) / CELL * CELL;
    if (a0 >= a1 || b0 >= b1) return 0;
    *ix0 = a0, *iy0 = b0, *ix1 = a1, *iy1 = b1;
    return fill_kept = 1;
}
void psx_vram_touch(int x, int y, int w, int h) {
    psx_gpu_sync();
    int cx0, cy0, cx1, cy1;
    if (!cell_range(x, y, x + w, y + h, &cx0, &cy0, &cx1, &cy1)) return;
    int partial = (x % CELL) || (y % CELL) || ((x + w) % CELL && x + w < VW) || ((y + h) % CELL && y + h < VH);
    const int xe = x + w < VW ? x + w : VW, ye = y + h < VH ? y + h : VH;
    /* (columns: every partly covered cell the shadow doesn't hold is covered over its whole height: kept in part
     * (cell_cols), not read back) */
    int columns = 0;
    if (partial && !filling) {
        columns = 1;
        for (int cy = cy0; cy < cy1 && columns; cy++)
            for (int cx = cx0; cx < cx1; cx++) {
                if (!shadow_ok[cy][cx] && !(cy * CELL >= y && (cy + 1) * CELL <= ye)) { columns = 0; break; }
            }
        if (!columns) cpu_reads(x, y, x + w, y + h);
    }
    gen_counter++;
    tex_epoch++;
    cells_ver++;
    for (int cy = cy0; cy < cy1; cy++)
        for (int cx = cx0; cx < cx1; cx++) {
            if (filling) {
                /* the fill is drawn into the mirror too (psx_vram_filled): both sides current where it covers a
                 * whole cell; a partly covered cell keeps the GPU's state */
                int whole = cx * CELL >= x && (cx + 1) * CELL <= x + w && cy * CELL >= y && (cy + 1) * CELL <= y + h;
                if (whole) mirror_ok[cy][cx] = 1, shadow_ok[cy][cx] = !fill_kept, cell_cols[cy][cx] = 0; /* (kept: the CPU's copy not filled) */
                else if (!shadow_ok[cy][cx]) { /* (the shadow part is updated too, but other pixels are stale) */ }
            } else if (columns && !shadow_ok[cy][cx]) {
                const int a = x > cx * CELL ? x - cx * CELL : 0, b = xe < (cx + 1) * CELL ? xe - cx * CELL : CELL;
                const u32 cols = cell_cols[cy][cx] | (b - a == 32 ? ~0u : ((1u << (b - a)) - 1) << a);
                if (cols == ~0u) shadow_ok[cy][cx] = 1, cell_cols[cy][cx] = 0;
                else cell_cols[cy][cx] = cols;
                mirror_ok[cy][cx] = 0;
            } else {
                shadow_ok[cy][cx] = 1;
                mirror_ok[cy][cx] = 0;
                cell_cols[cy][cx] = 0;
            }
            cell_gen[cy][cx] = gen_counter;
        }
    filling = fill_kept = 0;
}

/* gpu.c: before the CPU reads VRAM (StoreImage, MoveImage's source) */
void psx_vram_read(int x, int y, int w, int h) {
    psx_gpu_sync();
    cpu_reads(x, y, x + w, y + h);
}

/* ---- the C3D frame ------------------------------------------------------------------------------------------ */

static C3D_AttrInfo attr;
static C3D_Tex dither_tex; /* the PS1 dither pattern (set_tev_for), on texture unit 1 */
static C3D_BufInfo bufinfo;

/* the mirrors' depth buffer, for reordered runs (run_add): */
#define DEPTH_MAX 16000 /* depth values per clear (1/16384 apart, as the shader maps them) */
static void *mirror_depth;
static unsigned depth_next = 1, depth_submitted = 1; /* the next depth value; the first of this submit's */
static int depth_clear; /* the depth buffer to be cleared at the next frame's begin (submit) */

static int model_free_pending; /* (the menu's model textures to give back: model_tex_free) */
static unsigned model_gen;
static void model_tex_free(void);
static unsigned texscale_wh = ~0u; /* (the size set last: the atlases share one, a switch between them sets nothing) */
static void frame_begin(void) {
    if (in_frame) return;
    gs_beginning();
    unsigned profile_start = port_profile_begin();
    extern volatile const char *ctr_where;
    const char *was = (const char *)ctr_where;
    ctr_where = "C3D_FrameBegin (waiting for the GPU)";
    C3D_FrameBegin(0); /* waits for the GPU to finish what was sent: the other half is free */
    ctr_where = was;
    if (depth_clear) { /* (runs' depth values used up: submit) */
        C3D_RenderTargetClear(mirror_rt[0], C3D_CLEAR_DEPTH, 0, 0);
        depth_clear = 0;
    }
    if (model_free_pending) model_tex_free();
    port_profile_end(PORT_PROFILE_SCALE, profile_start); /* ("scale": the wait for the GPU) */
    gpu_busy = 0;
    hz_done();
    in_frame = 1;
    C3D_BindProgram(&shader);
    C3D_SetAttrInfo(&attr);
    texscale_wh = ~0u; /* (the uniforms set again in a new frame) */
    if (dither_tex.data) tex_bind(1, &dither_tex); /* (set_tev_for's dithering: unit 1 throughout) */
    BufInfo_Init(&bufinfo);
    BufInfo_Add(&bufinfo, vb, sizeof(Vtx), 4, 0x3210);
    C3D_SetBufInfo(&bufinfo);
    C3D_CullFace(GPU_CULL_NONE);
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
}

static void submit(void);

/* the current half from its start, what was recorded before drawn (frame_serial: what is recorded from here) */
static void half_restart(void) {
    vb = vb_half[half];
    staging = staging_half[half];
    vb_used = vb_flushed = 0;
    staging_used = staging_flushed = 0;
    frame_serial++;
}

/* everything recorded so far executed and finished (the CPU may then read the mirror, or rewrite a texture) */
static void gpu_sync(void) {
    trace("gpu_sync", vb_used, in_frame);
    submit();
    if (in_frame) {
        frame_end();
        in_frame = 0;
    }
    frame_begin();
    half_restart(); /* (the GPU is idle: both halves free) */
    ctr_render_stats[6]++;
}
/* the same, only when the GPU may still draw with something recorded (a texture is about to be rewritten) */
static unsigned nbatches;
static void gpu_quiet(void) {
    if (in_frame || gpu_busy || nbatches) gpu_sync();
}

/* ---- batches ------------------------------------------------------------------------------------------------ */

enum { K_UNTEX, K_TEX, K_TEX_P1, K_TEX_P2, K_BLIT, K_COMPOSITE, K_SCENE, K_PAGE_OP, K_PAGE_SEMI, K_PAGE_COPY };
#define MASK_SET 1   /* drawn pixels get the mask bit (alpha 1); else the texel's (textured) or 0 */
#define MASK_CHECK 2 /* pixels with the mask bit keep their colour (opaque draws) */
#define MASK_COPY 4  /* an upload: the source's mask bits as they are */
#define MASK_DITHER 8 /* (not a mask: the PS1's dithering on, GP0 E1 bit 9, for a draw into the mirror: set_tev_for) */
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

static void run_flush(void);
static Vtx *alloc_vtx(unsigned n) {
    run_flush(); /* (an open run holds the rest of the buffer: run_open) */
    if (vb_used + n > VB_MAX) gpu_sync();
    Vtx *v = vb + vb_used;
    vb_used += n;
    hz_check(v, n * sizeof(Vtx), "vertices");
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
 * exactly the PS1's whatever overlaps, in either eye. Everything else draws without depth, after the run.
 * Several open (RUN_GROUPS): a primitive joins the last one, or, replacing its pixels, any earlier one of its state, all
 * drawn in the order they were opened; one that blends (or checks the mask) only the last, as it must be drawn after
 * everything before it. A field's background and models alternate among three texture atlases and more: with two
 * open, Lindblum's castle station was 214 batches a frame, each a draw call and its state. */
#define RUN_V 1536 /* vertices per open batch (the first; the others RUN_V2) */
#define RUN_V2 1024
#define RUN_GROUPS 6
typedef struct {
    u8 kind, abr, mask;
    u16 sc[4];
    C3D_Tex *tex;
    unsigned nv;
} RunGroup;
static RunGroup run_g[RUN_GROUPS];
static int run_n;
static unsigned run_prims; /* primitives in the run (their place: Vtx.d, from the run's first value) */
/* (the open batches' vertices are written straight into the vertex buffer, each with its final depth value: group 0 from
 * where the buffer was, group 1 at its end, moved down after group 0 when the run ends. They were written into arrays
 * of their own and copied again at the run's end, every vertex of a battle's models twice more through memory an Old
 * 3DS doesn't keep in its cache) */
static unsigned run_at[RUN_GROUPS], run_cap[RUN_GROUPS]; /* each group's place in vb and its room */
static float run_d0;                  /* the run's first depth value */
static unsigned run_max;              /* ... and its most primitives */
static inline unsigned run_room(void) {
    int r = (DEPTH_MAX - (int)depth_submitted) / 2 - (int)(depth_next - depth_submitted);
    return r > 0 ? (unsigned)r : 0;
}
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
    depth_next += run_prims;
    run_prims = 0;
    /* (the other groups moved down after the first, in order: each to below where it was) */
    unsigned at[RUN_GROUPS], end = run_at[0] + run_g[0].nv;
    at[0] = run_at[0];
    for (int i = 1; i < n; i++) {
        memmove(vb + end, vb + run_at[i], run_g[i].nv * sizeof(Vtx));
        at[i] = end;
        end += run_g[i].nv;
    }
    vb_used = end;
    int saved_target = cur_target, saved_mask = cur_mask;
    for (int i = 0; i < n; i++) {
        RunGroup *g = &run_g[i];
        cur_target = 0;
        cur_mask = g->mask;
        cur_ordered = 1;
        add_batch(g->kind, g->abr, 3, g->sc, g->tex, at[i], g->nv);
    }
    cur_ordered = 0;
    cur_target = saved_target;
    cur_mask = saved_mask;
}

/* two scissors (four u16 each) the same: one 64-bit compare (memcmp, a library call a few times for every primitive,
 * was 0.3 ms of a busy field's VBlank on an Old 3DS) */
static inline int sc_same(const u16 *a, const u16 *b) {
    u64 x, y;
    memcpy(&x, a, sizeof x);
    memcpy(&y, b, sizeof y);
    return x == y;
}
static inline int run_same(const RunGroup *g, int kind, int abr, const u16 sc[4], const C3D_Tex *tex) {
    return g->kind == kind && g->abr == abr && g->mask == cur_mask && g->tex == tex && sc_same(g->sc, sc);
}

/* whether a batch's primitives replace what they draw (so they may move ahead of others: their depth keeps later ones
 * off them) */
static inline int replaces(const RunGroup *g) {
    return g->abr == NO_BLEND && !(g->mask & MASK_CHECK) && (g->kind == K_TEX || g->kind == K_TEX_P1 || g->kind == K_UNTEX);
}

/* room for a primitive's n vertices in group i, and its depth value (*d): the caller writes them, each with d */
static inline Vtx *run_take(int i, unsigned n, float *d) {
    Vtx *o = vb + run_at[i] + run_g[i].nv;
    *d = run_d0 + (float)run_prims;
    run_g[i].nv += n;
    run_prims++;
    return o;
}

static void run_open(int i, int kind, int abr, const u16 sc[4], C3D_Tex *tex) {
    if (i == 0) { /* (the run's room: the buffer's rest, its last RUN_V for group 1; nothing else allocates meanwhile) */
        /* (both eyes' values of this submit must fit before the next clear (frame_begin clears): the run ends at as many
         * primitives as there are values left) */
        const unsigned others = (RUN_GROUPS - 1) * RUN_V2; /* (the other groups' room: the buffer's end, in order) */
        if (run_room() < 16 || vb_used + others + 64 > VB_MAX) gpu_sync();
        run_max = run_room();
        run_at[0] = vb_used, run_cap[0] = VB_MAX - others - vb_used;
        if (run_cap[0] > RUN_V) run_cap[0] = RUN_V;
        for (int k = 1; k < RUN_GROUPS; k++) run_at[k] = VB_MAX - others + (k - 1) * RUN_V2, run_cap[k] = RUN_V2;
        hz_check(vb + vb_used, (VB_MAX - vb_used) * sizeof(Vtx), "vertices");
        vb_used = VB_MAX;
        run_d0 = (float)depth_next;
    }
    RunGroup *g = &run_g[i];
    g->kind = (u8)kind; g->abr = (u8)abr; g->mask = (u8)cur_mask; g->tex = tex; g->nv = 0;
    memcpy(g->sc, sc, sizeof g->sc);
    run_n = i + 1;
}

/* where a primitive's n vertices go in the run (cur_target 0, n <= RUN_V, mirror_depth: run_add's case), *d their
 * depth value */
static Vtx *run_slot(int kind, int abr, const u16 sc[4], C3D_Tex *tex, unsigned n, float *d) {
    if (run_n && run_prims >= run_max) run_flush();
    if (run_n) {
        int last = run_n - 1;
        if (run_same(&run_g[last], kind, abr, sc, tex) && run_g[last].nv + n <= run_cap[last]) return run_take(last, n, d);
        for (int i = 0; i < last; i++)
            if (run_same(&run_g[i], kind, abr, sc, tex) && replaces(&run_g[i]) && run_g[i].nv + n <= run_cap[i]) {
                ctr_run_reorders++;
                return run_take(i, n, d);
            }
        if (run_n < RUN_GROUPS) {
            run_open(run_n, kind, abr, sc, tex);
            return run_take(run_n - 1, n, d);
        }
        run_flush();
    }
    run_open(0, kind, abr, sc, tex);
    return run_take(0, n, d);
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
    float d;
    Vtx *o = run_slot(kind, abr, sc, tex, n, &d);
    for (unsigned k = 0; k < n; k++) o[k] = v[k], o[k].d = d;
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
static int tev_dither; /* set_tev_for: the PS1's dither added (a mirror draw with dithering on) */
static int tev_setmask; /* set_tev_for: a mirror draw with set_mask (its pixels' alpha 1: their mask bit) */
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
    case K_PAGE_OP: /* the mirror sampled as a texture, its texels without the mask bit (alpha 0): drawn opaque */
    case K_PAGE_SEMI: /* ... and those with it (alpha 1): blended. Alpha 1 for the ones drawn (the test), written as
                       * the mask bit by the blend (set_blend_mask: 0 for the opaque pass, 1 for the blended one) */
        C3D_TexEnvSrc(e0, C3D_RGB, GPU_TEXTURE0, GPU_PRIMARY_COLOR, 0);
        C3D_TexEnvFunc(e0, C3D_RGB, GPU_MODULATE);
        C3D_TexEnvScale(e0, C3D_RGB, GPU_TEVSCALE_2);
        if (kind == K_PAGE_SEMI) {
            C3D_TexEnvSrc(e0, C3D_Alpha, GPU_TEXTURE0, 0, 0);
            C3D_TexEnvFunc(e0, C3D_Alpha, GPU_REPLACE);
            break;
        }
        /* (none for 0x0000, which the PS1 never draws: (R + G) x 4, + B, x 4, then x (1 - a) x 4 in stage 2 (below):
         * at least 0.5 for a texel of any colour without the mask bit, 0 for black or one with it) */
        C3D_TexEnvSrc(e0, C3D_Alpha, GPU_TEXTURE0, GPU_TEXTURE0, 0);
        C3D_TexEnvOpAlpha(e0, GPU_TEVOP_A_SRC_R, GPU_TEVOP_A_SRC_G, 0);
        C3D_TexEnvFunc(e0, C3D_Alpha, GPU_ADD);
        C3D_TexEnvScale(e0, C3D_Alpha, GPU_TEVSCALE_4);
        C3D_TexEnvSrc(e1, C3D_RGB, GPU_PREVIOUS, 0, 0);
        C3D_TexEnvFunc(e1, C3D_RGB, GPU_REPLACE);
        C3D_TexEnvSrc(e1, C3D_Alpha, GPU_PREVIOUS, GPU_TEXTURE0, 0);
        C3D_TexEnvOpAlpha(e1, GPU_TEVOP_A_SRC_ALPHA, GPU_TEVOP_A_SRC_B, 0);
        C3D_TexEnvFunc(e1, C3D_Alpha, GPU_ADD);
        C3D_TexEnvScale(e1, C3D_Alpha, GPU_TEVSCALE_4);
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
    /* The PS1 dithers what it draws (GP0 E1 bit 9) before keeping 5 bits per channel, and FF9 counts on it: a smooth
     * shade (the world map's fogged ground) in the mirror's 5 bits without it shows as bands. Its 4x4 pattern (-4..+3
     * of 255 per pixel) is added here, from a texture tiled over the framebuffer (prim.v.pica's texcoord1). */
    C3D_TexEnv *e2 = C3D_GetTexEnv(2);
    C3D_TexEnvInit(e2);
    if (tev_dither && kind != K_BLIT && kind != K_COMPOSITE && kind != K_SCENE) {
        C3D_TexEnvSrc(e2, C3D_RGB, GPU_PREVIOUS, GPU_TEXTURE1, 0);
        C3D_TexEnvFunc(e2, C3D_RGB, GPU_ADD_SIGNED);
        C3D_TexEnvSrc(e2, C3D_Alpha, GPU_PREVIOUS, 0, 0);
        C3D_TexEnvFunc(e2, C3D_Alpha, GPU_REPLACE);
    } else if (kind == K_PAGE_OP) {
        C3D_TexEnvSrc(e2, C3D_RGB, GPU_PREVIOUS, 0, 0);
        C3D_TexEnvFunc(e2, C3D_RGB, GPU_REPLACE);
    }
    if (kind == K_PAGE_OP) {
        C3D_TexEnvSrc(e2, C3D_Alpha, GPU_PREVIOUS, GPU_TEXTURE0, 0);
        C3D_TexEnvOpAlpha(e2, GPU_TEVOP_A_SRC_ALPHA, GPU_TEVOP_A_ONE_MINUS_SRC_ALPHA, 0);
        C3D_TexEnvFunc(e2, C3D_Alpha, GPU_MODULATE);
        C3D_TexEnvScale(e2, C3D_Alpha, GPU_TEVSCALE_4);
    }
    /* set_mask: every pixel drawn gets the mask bit. The blend writes it as the source's alpha x the constant's 1, and
     * a texel without bit 15 has 0.25 (K_TEX: stored as 0): with set_mask each drawn pixel's alpha is raised to 1
     * here (x 16; those the test drops are 0 exactly), a copy's set to 1. Stored as 0, the target hand (whose CLUT has
     * no bit 15) was left unmasked in the frame, and Flare's afterimages, which paste a frame's unmasked pixels
     * opaque, kept a dark copy of it on Nova Dragon */
    C3D_TexEnv *e3 = C3D_GetTexEnv(3), *e4 = C3D_GetTexEnv(4);
    C3D_TexEnvInit(e3);
    C3D_TexEnvInit(e4);
    if (tev_setmask) {
        C3D_TexEnvSrc(e3, C3D_RGB, GPU_PREVIOUS, 0, 0);
        C3D_TexEnvFunc(e3, C3D_RGB, GPU_REPLACE);
        C3D_TexEnvSrc(e4, C3D_RGB, GPU_PREVIOUS, 0, 0);
        C3D_TexEnvFunc(e4, C3D_RGB, GPU_REPLACE);
        if (kind == K_BLIT) {
            C3D_TexEnvSrc(e3, C3D_Alpha, GPU_CONSTANT, 0, 0);
            C3D_TexEnvFunc(e3, C3D_Alpha, GPU_REPLACE);
            C3D_TexEnvColor(e3, 0xFF000000);
        } else {
            C3D_TexEnvSrc(e3, C3D_Alpha, GPU_PREVIOUS, 0, 0);
            C3D_TexEnvFunc(e3, C3D_Alpha, GPU_REPLACE);
            C3D_TexEnvScale(e3, C3D_Alpha, GPU_TEVSCALE_4);
        }
        C3D_TexEnvSrc(e4, C3D_Alpha, GPU_PREVIOUS, 0, 0);
        C3D_TexEnvFunc(e4, C3D_Alpha, GPU_REPLACE);
        C3D_TexEnvScale(e4, C3D_Alpha, GPU_TEVSCALE_4);
    }
    if (kind == K_TEX) C3D_AlphaTest(true, GPU_GREATER, 0);
    else if (kind == K_PAGE_OP) C3D_AlphaTest(true, GPU_GREATER, 0x3F);
    else if (kind == K_TEX_P1 || kind == K_TEX_P2 || kind == K_PAGE_SEMI) C3D_AlphaTest(true, GPU_GREATER, 0x7F);
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
        else if (kind == K_TEX || kind == K_TEX_P2 || kind == K_PAGE_SEMI) sa = check ? GPU_ONE_MINUS_DST_ALPHA : GPU_ONE, da = check ? GPU_ONE : GPU_ZERO;
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
        C3D_BlendingColor(0xFF000000); /* (set_mask's alpha: else as the last draw left it, a UI layer's 0x80 or 0x40) */
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ONE, sa, da);
        break;
    case 2: /* B - F */
        C3D_BlendingColor(0xFF000000);
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
    unsigned wh = t ? (unsigned)t->width << 16 | t->height : 0;
    if (wh == texscale_wh) return;
    texscale_wh = wh;
    /* (texture memory runs top-down, t bottom-up: a texel row r is at t = 1 - r / height) */
    C3D_FVUnifSet(GPU_VERTEX_SHADER, u_texscale, t ? 1.0f / t->width : 0.0f, t ? -1.0f / t->height : 0.0f, 0, 1);
}

/* Replay the recorded batches into each eye's mirror. */
static C3D_RenderTarget *outline_rt[2][2]; /* (the outline masks, [buffer][eye]: below) */
static void outline_proj(int x0, int y0);
/* a framebuffer's mask: the second one when it lies right of (battles: x 320) or below (fields: y 256) the first */
static inline int outline_buf(int x, int y) { return x >= 256 || y >= 256; }
static void page_copy(const Batch *b);
static unsigned nsubmits, page_pending = ~0u; /* (submits so far; the one a page copy is recorded in: mirror_page) */
static void submit(void) {
    trace("submit", (int)nbatches, (int)vb_used);
    run_flush();
    if (!nbatches) return;
    nsubmits++;
    unsigned profile_start = port_profile_begin();
    frame_begin();
    unsigned any_ui = 0;
    for (unsigned i = 0; i < nbatches; i++) any_ui |= 1u << batches[i].target;
    /* passes: each eye's mirror, then each UI layer (no stereo: the bottom screen is flat) */
    for (int pass = 0; pass < 8; pass++) {
        int eye = pass == 1 || pass == 5 || pass == 7, target = pass >= 6 ? 4 : pass >= 4 ? 3 : pass >= 2 ? pass - 1 : 0;
        if ((pass == 1 || pass == 5 || pass == 7) && !stereo_on) continue;
        if (target && !(any_ui & (1u << target))) continue;
        int height = target == 3 ? 240 : target == 4 ? VH : target ? UI_H : VH;
        if (target == 3) { /* the top screen (its render target is the rotated 240x400): no scissor, as screen_quad */
            C3D_FrameDrawOn(top_rt[eye]);
            C3D_Mtx pm;
            Mtx_OrthoTilt(&pm, 0.0f, 400.0f, 240.0f, 0.0f, 0.0f, 1.0f, true);
            C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, u_proj, &pm);
            C3D_FVUnifSet(GPU_VERTEX_SHADER, u_stereo, 0, 0, 1, 0.5f);
            C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
        } else if (target == 4) { /* the models' outline masks (outline_draw), as the mirror's pass, at half size */
            if (!outline_rt[0][0]) continue;
            C3D_FVUnifSet(GPU_VERTEX_SHADER, u_stereo, eye ? stereo_k : -stereo_k, stereo_inv_zc, 1.0f / 4096.0f, -0.5f);
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
            if ((!target || target == 4) && !(b->eyes & (1 << eye))) continue;
            if (b->kind == K_PAGE_COPY) { /* a page of the mirror copied for primitives after it (mirror_page) */
                page_copy(b);
                C3D_FrameDrawOn(mirror_rt[eye]);
                set_proj_ortho(VW, VH, 0.0f);
                C3D_FVUnifSet(GPU_VERTEX_SHADER, u_stereo, k, stereo_inv_zc, 1.0f / 4096.0f, -0.5f);
                ordered = 0, kind = abr = bmask = -1, tex = (C3D_Tex *)-1, sc[0] = 0xFFFF;
                continue;
            }
            if (b->ordered != ordered) {
                ordered = b->ordered;
                C3D_DepthTest(ordered, GPU_GEQUAL, ordered ? GPU_WRITE_ALL : GPU_WRITE_COLOR);
                C3D_FVUnifSet(GPU_VERTEX_SHADER, u_stereo, target && target != 4 ? 0 : k, stereo_inv_zc, 1.0f / 4096.0f,
                              ordered ? -depth_off * (1.0f / 16384) : -0.5f);
            }
            if (b->abr != abr || b->mask != bmask || b->kind != kind) {
                if (b->kind != kind || ((b->mask ^ bmask) & (MASK_DITHER | MASK_SET))) {
                    tev_dither = !target && (b->mask & MASK_DITHER);
                    tev_setmask = !target && (b->mask & MASK_SET) && !(b->mask & MASK_COPY);
                    set_tev_for(kind = b->kind, target != 0 && target != 4);
                    tev_dither = tev_setmask = 0;
                }
                if (target == 4) abr = b->abr, bmask = b->mask, set_blend(NO_BLEND, 0);
                else set_blend_mask(abr = b->abr, target != 0, bmask = b->mask, kind);
            }
            if (b->tex != tex) {
                tex = b->tex;
                if (tex) tex_bind(0, tex);
                set_texscale(tex);
            }
            if (target == 4) { /* (the framebuffer this batch draws into: its mask, its area's projection) */
                if (b->sc[0] != sc[0] || b->sc[1] != sc[1]) {
                    sc[0] = b->sc[0], sc[1] = b->sc[1];
                    C3D_FrameDrawOn(outline_rt[outline_buf(sc[0], sc[1])][eye]);
                    outline_proj(sc[0], sc[1]);
                    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
                }
                draw_tris(b->first, b->count);
                continue;
            }
            if (target != 3 && memcmp(sc, b->sc, sizeof sc)) {
                memcpy(sc, b->sc, sizeof sc);
                /* (in window coordinates, which run bottom-up) */
                C3D_SetScissor(GPU_SCISSOR_NORMAL, sc[0], height - sc[3], sc[2], height - sc[1]);
            }
            draw_tris(b->first, b->count);
        }
        if (ordered) C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
        GSEG(target == 0 ? GS_MIRROR : target == 3 ? GS_TOPUI : target == 4 ? GS_OUTLINE : GS_LAYERS);
    }
    ctr_render_stats[0] += nbatches;
    nbatches = 0;
    /* (the other eye's values were past this submit's: the next ones are past those) */
    depth_next += depth_next - depth_submitted;
    depth_submitted = depth_next;
    if (mirror_depth && depth_next > DEPTH_MAX / 2) {
        /* runs' depth values used up: from 1 again, the depth buffer cleared after what was drawn with them (a GX
         * fill runs in the GPU's queue, not its command list: the list so far goes in first). Here, with nothing
         * recorded that holds a value: the values are given as runs are recorded, before their frame begins. */
        /* (the clear right after a frame's begin, the GPU idle: a fill in the middle of a frame (split off) hung the
         * GPU on a 3DS, though not in an emulator) */
        depth_next = depth_submitted = 1;
        depth_clear = 1;
        frame_end();
        in_frame = 0;
        frame_begin();
        half_restart();
    }
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
typedef u32 __attribute__((may_alias)) u32a; /* (a word of 16-bit pixels) */
/* two at once: (c1 << 16 | c0) -> (rgba5551(c1) << 16 | rgba5551(c0)) */
static inline u32 bgr555x2_to_rgba5551(u32 c) {
    return (c & 0x001F001Fu) << 11 | (c & 0x03E003E0u) << 1 | (c & 0x7C007C00u) >> 9 | (c & 0x80008000u) >> 15;
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
    hz_check(p, bytes, "staging");
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
        /* tile by tile (w a multiple of 32, h 32): a tile's row y is four pairs of texels at morton_y[y] + 0, 4, 16, 20,
         * each pair (x, x + 1) side by side, converted two at a time in a word; each texel where tiled() puts it, as
         * bgr555_to_rgba5551 has it (a host test, every run width: the same staging bytes). Per texel through tiled()
         * it was ~20 instructions: a movie background's frame, 70k pixels blitted every other frame */
        for (int tx = 0; tx < w; tx += 8) {
            u32a *t = (u32a *)(px + (tx >> 3) * 64);
            for (int ty = 0; ty < h; ty += 8, t += (tw >> 3) * 32)
                for (int y = 0; y < 8; y++) {
                    const u32a *s = (const u32a *)(psx_vram + (by + ty + y) * VW + bx + tx);
                    u32a *o = t + morton_y[y] / 2;
                    o[0] = bgr555x2_to_rgba5551(s[0]), o[2] = bgr555x2_to_rgba5551(s[1]);
                    o[8] = bgr555x2_to_rgba5551(s[2]), o[10] = bgr555x2_to_rgba5551(s[3]);
                }
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
    trace("blit_cells", cx0 | cy0 << 8, cx1 | cy1 << 8);
    if (!psx_vram) return;
    for (int cy = cy0; cy < cy1; cy++) /* (a cell written in part: the rest read back first) */
        for (int cx = cx0; cx < cx1; cx++)
            if (cell_cols[cy][cx]) readback_cells(cx, cy, cx + 1, cy + 1);
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
    trace("readback", cx0 | cy0 << 8, cx1 | cy1 << 8);
    cells_ver++;
    extern volatile const char *ctr_where;
    ctr_where = "readback";
    gpu_sync();
    const u16 *m = mirror_tex[0].data;
    for (int cy = cy0; cy < cy1; cy++)
        for (int cx = cx0; cx < cx1; cx++) {
            if (shadow_ok[cy][cx]) continue;
            const u32 kept = cell_cols[cy][cx]; /* (columns the CPU wrote since: not the mirror's) */
            for (int y = cy * CELL; y < (cy + 1) * CELL; y++) {
                u16 *dst = psx_vram + y * VW;
                for (int x = cx * CELL; x < (cx + 1) * CELL; x++)
                    if (!(kept >> (x - cx * CELL) & 1)) dst[x] = rgba5551_to_bgr555(m[tiled(x, y, VW)]);
            }
            shadow_ok[cy][cx] = 1;
            cell_cols[cy][cx] = 0;
        }
    ctr_render_stats[3]++;
}

/* gpu.c: a fill (ClearImage, a GP0 fill command): drawn by the GPU too, so the cells it covers stay current there */
void psx_vram_filled(int x, int y, int w, int h, unsigned int rgb15) {
    psx_gpu_sync();
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
    u16 ox, oy;  /* its 256x256 place in its atlas (texels) */
    u8 atlas;    /* the atlas it is in (atlases[]) */
    u32 stamp[64]; /* per 32x32 block: gen_counter when converted (0 = not yet) */
    u32 epoch[64]; /* per block: tex_epoch when it was last known current (no write to any source cell since) */
    u8 flags[64];  /* per block: 1 = has opaque texels, 2 = has semi-transparent ones */
    u32 hash[64];  /* per block: its source texels' hash when converted */
    u32 chash[64]; /* ... and its CLUT's */
    u64 cur;       /* blocks known current (stamp set, epoch == cur_epoch): texture_for's test of them at once */
    u64 fl1, fl2;  /* blocks whose flags have 1, 2 (as flags[]: texture_for's flags of current blocks at once) */
    u32 cur_epoch;
    u32 was_key, was_twin; /* given up by slot_renew (key 0): the page its blocks still hold, as stamp, hash and chash say */
} TexSlot;

#define MAX_SLOTS 96
#define SLOT_RESERVE (512u << 10)
static TexSlot slots[MAX_SLOTS];
static int nslots;
int ctr_texture_pages(void) { psx_gpu_sync(); return nslots; } /* (the heartbeat's: platform.c) */
/* The slots live side by side in atlases (1024x1024: 16 of them; 512x512 or one alone when linear memory is short):
 * primitives from different slots of one atlas are drawn in one batch. Each texture bind was a batch of its own, and
 * a busy field background switches between pages and CLUTs hundreds of times a frame (Lindblum's Castle Station:
 * 454 batches, 27 ms a frame in Azahar; 16 batches, 18 ms with one texture). */
#define MAX_ATLASES MAX_SLOTS
static struct { C3D_Tex tex; u8 cells, used; u16 occ; } atlases[MAX_ATLASES]; /* (occ: its cells in use) */
static int natlases;
/* a new slot's place: a free cell (the last atlas's first), or a new atlas (largest that fits); 0 if none */
static void cell_xy(int a, int c, TexSlot *s) {
    unsigned per = atlases[a].tex.width / 256;
    s->atlas = (u8)a, s->ox = (u16)((c % per) * 256), s->oy = (u16)((c / per) * 256);
}
static int slot_place(TexSlot *s) {
    for (int a = natlases - 1; a >= 0; a--)
        for (int c = 0; c < atlases[a].cells; c++)
            if (!(atlases[a].occ >> c & 1)) {
                atlases[a].occ |= (u16)(1u << c), atlases[a].used++;
                cell_xy(a, c, s);
                return 1;
            }
    if (natlases == MAX_ATLASES) return 0;
    static const unsigned sizes[3] = {1024, 512, 256};
    int ok = 0;
    for (int k = 0; k < 3 && !ok; k++) {
        unsigned w = sizes[k];
        if (linearSpaceFree() < w * w * 4 + SLOT_RESERVE) continue;
        ok = C3D_TexInit(&atlases[natlases].tex, w, w, GPU_RGBA8);
    }
    if (!ok) return 0;
    C3D_TexSetFilter(&atlases[natlases].tex, GPU_NEAREST, GPU_NEAREST);
    unsigned per = atlases[natlases].tex.width / 256;
    int a = natlases++;
    atlases[a].cells = (u8)(per * per), atlases[a].used = 1, atlases[a].occ = 1;
    cell_xy(a, 0, s);
    return 1;
}
static inline C3D_Tex *slot_tex(const TexSlot *s) { return &atlases[s->atlas].tex; }
static int tex_ou, tex_ov; /* texture_for: the returned texture's offset of the primitive's page (texels) */
static int slots_held_at = -1; /* (slot_for: the size the cache stopped growing at) */
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
static unsigned slots_made; /* (slot_for: slots given a new key, a scene's change) */
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
    /* (the cache stops growing with SLOT_RESERVE of linear memory left: 96 slots are more than there is, and what
     * allocates later (a voice line's buffers, the HUD's images) would find none; the menu's models take theirs back
     * with slot_shrink) */
    /* (linear memory is asked only while the cache may still grow: held_at is the size it stopped at, cleared when
     * the menu's models give theirs back) */
    if (slots_held_at == nslots) {
    } else if (nslots < MAX_SLOTS && linearSpaceFree() <= SLOT_RESERVE) {
        slots_held_at = nslots;
        port_log("renderer: texture cache held at %d pages (linear memory)\n", nslots);
    }
    if (nslots < MAX_SLOTS && slots_held_at != nslots && slot_place(&slots[nslots])) {
        victim = nslots++;
        ctr_render_stats[5] = nslots;
    } else if (victim < 0) {
        gpu_sync(); /* every slot is in this frame's batches */
        victim = 0;
    }
    if (slots[victim].key) hash_remove(victim);
    TexSlot *s = &slots[last = victim];
    slots_made++;
    s->key = key;
    s->twin = twin;
    s->was_key = 0;
    memset(s->epoch, 0, sizeof s->epoch);
    s->cur = 0;
    unsigned h = hash_of(key, twin);
    while (slot_hash[h]) h = (h + 1) & (SLOT_HASH - 1);
    slot_hash[h] = (u16)(victim + 1);
    memset(s->stamp, 0, sizeof s->stamp);
    return s;
}

/* The page's texture changed where a frame not yet drawn uses it (Nova Dragon's arena scrolls its sky, 8-bit, at every
 * step of the battle's): those draws keep the slot, out of the cache, and the page gets another, converted afresh: one
 * given up so earlier that no frame left to draw uses it, else the cache's. Converted again in the same slot, each
 * waited for the GPU to finish the frame so far, or the frame in flight: some 850 waits in 128 frames there, 4 ms of
 * a VBlank */
static void hash_insert(int i);
/* The slot taken is one at the same place in an atlas of the same size, so the page's texture coordinates stay the same
 * to the bit: where a texture sits changes which texel a pixel near a texel's edge samples (its coordinates, the slot's
 * place added, interpolated at the GPU's precision). A page rewritten every frame (the world map's water and CLUT
 * cycling, Kuja's arena) went to whatever slot was free, and its pixels by texel edges changed with the place: in
 * Azahar, slots moved by extra GPU syncs changed the world map's far ground in 24 of 43 frames; none once pages kept
 * their places (and each its own texture: the rest of it, pages cached and given up as the camera turns, not done) */
static int same_place(const TexSlot *r, const TexSlot *s) {
    return r->ox == s->ox && r->oy == s->oy && atlases[r->atlas].tex.width == atlases[s->atlas].tex.width;
}
static TexSlot *slot_take(TexSlot *r, u32 key, u32 twin) {
    if (r->key) hash_remove((int)(r - slots));
    r->key = key;
    r->twin = twin;
    r->was_key = 0;
    memset(r->epoch, 0, sizeof r->epoch);
    memset(r->stamp, 0, sizeof r->stamp);
    r->cur = 0;
    hash_insert((int)(r - slots));
    return r;
}
/* (slot_renew: a slot the page gave up, the page's again with the blocks it holds) */
#define RENEW_OWN 4
int ctr_slot_keep = 1; /* (0: a page given another slot never takes one of its own back: as before, for comparing) */
static int renew_b;                  /* convert_block: the block that must change, and what its sources hash to now */
static u32 renew_hash, renew_chash;
static TexSlot *slot_back(TexSlot *r, u32 key, u32 twin) {
    r->key = key, r->twin = twin, r->was_key = 0;
    memset(r->epoch, 0, sizeof r->epoch);
    r->cur = 0;
    hash_insert((int)(r - slots));
    return r;
}
static TexSlot *slot_renew(TexSlot *s) {
    u32 key = s->key, twin = s->twin;
    hash_remove((int)(s - slots));
    s->key = 0;
    s->twin = 0xFFFFFFFFu; /* (slot_for's last-hit check can't match it) */
    s->was_key = key, s->was_twin = twin;
    /* (a free one in its place; else the longest unused there (no frame left to draw uses it); else a new one there) */
    TexSlot *lru = NULL, *free_one = NULL, *own_oldest = NULL;
    int own = 0;
    const int b = renew_b;
    for (int i = 0; i < nslots; i++) {
        TexSlot *r = &slots[i];
        if (r == s || r->atlas == 0xFF || r->serial + 2 > frame_serial || !same_place(r, s)) continue;
        if (!r->key) {
            /* One this page gave up itself: its blocks are the page's as it was then, and each is checked against
             * what it was converted from before it is drawn with (convert_block: its sources' cells written since its
             * stamp, then their hashes), so only what changed is converted. The world map's pages go between two
             * slots as each frame's change comes while the frame before is in flight (the water's CLUT rows cycle
             * through four states, its strips scroll): taken empty, each converted every block it draws with again,
             * every frame: four pages, 682 blocks, 27 ms of the disc 3 airship's 53 ms walk in Azahar as an Old 3DS.
             * So the one whose block that asked for the change (convert_block: renew_b) already is what it must become:
             * the state of a cycle met again. A page keeps up to RENEW_OWN of them; past that its oldest is taken. */
            if (ctr_slot_keep && r->was_key == key && r->was_twin == twin) {
                if (r->stamp[b] && r->hash[b] == renew_hash && r->chash[b] == renew_chash) return slot_back(r, key, twin);
                own++;
                if (!own_oldest || r->serial < own_oldest->serial) own_oldest = r;
                continue;
            }
            if (!free_one) free_one = r;
            continue;
        }
        if (!lru || r->serial < lru->serial) lru = r;
    }
    if (own >= RENEW_OWN) return slot_back(own_oldest, key, twin);
    if (free_one) return slot_take(free_one, key, twin);
    if (nslots < MAX_SLOTS && slots_held_at != nslots)
        for (int a = 0; a < natlases; a++) {
            if (a == s->atlas || atlases[a].tex.width != atlases[s->atlas].tex.width) continue;
            unsigned per = atlases[a].tex.width / 256, c = s->oy / 256 * per + s->ox / 256;
            if (atlases[a].occ >> c & 1) continue;
            atlases[a].occ |= (u16)(1u << c), atlases[a].used++;
            TexSlot *r = &slots[nslots++];
            cell_xy(a, (int)c, r);
            r->key = 0, r->serial = 0;
            ctr_render_stats[5] = nslots;
            return slot_take(r, key, twin);
        }
    if (own_oldest) return slot_back(own_oldest, key, twin);
    if (lru) return slot_take(lru, key, twin);
    /* none in its place (the cache full: the world map's camera turned): the GPU waited for (what this frame recorded
     * so far drawn, or the frame in flight finished) and the page rewritten where it is. Moved to any free slot, as
     * before, its place changed, and on a 3DS so did the texels its pixels by a texel's edge pick: the world map's far
     * ocean, rewritten every frame, flickered on the right of the horizon (an Old 3DS: some 15 moves in 128 VBlanks there,
     * a page alternating between two places) */
    s->key = key, s->twin = twin;
    hash_insert((int)(s - slots));
    if (s->serial == frame_serial) gpu_sync();
    else frame_begin();
    return s;
}

/* the cache's last atlas given up with its slots (its texture freed), for memory someone else needs now; 0 if none */
static void hash_insert(int i) {
    unsigned h = hash_of(slots[i].key, slots[i].twin);
    while (slot_hash[h]) h = (h + 1) & (SLOT_HASH - 1);
    slot_hash[h] = (u16)(i + 1);
}
static int slot_shrink(void) {
    if (!natlases) return 0;
    int a = natlases - 1;
    for (int i = 0; i < nslots; i++)
        if (slots[i].atlas == a) {
            if (slots[i].key) hash_remove(i);
            slots[i].key = 0;
            slots[i].twin = 0xFFFFFFFFu; /* (never asked for: slot_for's last-hit check can't match it) */
            slots[i].atlas = 0xFF;
        }
    C3D_TexDelete(&atlases[a].tex);
    natlases--;
    for (int i = 0; i < nslots; i++) { /* (the slots left, at the front: the last ones moved into the holes) */
        if (slots[i].atlas != 0xFF) continue;
        while (nslots > i + 1 && slots[nslots - 1].atlas == 0xFF) nslots--;
        if (nslots == i + 1) { nslots = i; break; }
        int j = nslots - 1;
        if (slots[j].key) hash_remove(j);
        slots[i] = slots[j];
        if (slots[i].key) hash_insert(i);
        slots[j].key = 0, slots[j].twin = 0xFFFFFFFFu, slots[j].atlas = 0xFF;
        nslots--;
    }
    ctr_render_stats[5] = nslots;
    return 1;
}

static inline int cells_newer(int x0, int y0, int x1, int y1, u32 stamp) {
    for (int cy = y0 / CELL; cy <= (y1 - 1) / CELL; cy++)
        for (int cx = x0 / CELL; cx <= (x1 - 1) / CELL; cx++)
            if (cell_gen[cy & (CH - 1)][cx & (CW - 1)] > stamp) return 1;
    return 0;
}

static void mark_source(int x0, int y0, int x1, int y1) {
    cells_ver++;
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

/* Make block (bx, by) of the page's texture current; 1 (nothing done) when it must be converted again while a frame
 * not yet drawn uses the slot: texture_for gives the page another (slot_renew). */
static int convert_block(TexSlot *s, const Gp0Mode *m, int bx, int by) {
    int b = by * 8 + bx;
    if (s->stamp[b] && s->epoch[b] == tex_epoch) return 0; /* (current: nearly every call) */
    int shift = m->depth == 0 ? 2 : m->depth == 1 ? 1 : 0;
    int sx0 = m->tx + ((bx * 32) >> shift), sw = 32 >> shift, sy0 = m->ty + by * 32;
    /* a texture window reads only inside it: check (and read back) the whole page instead of the block */
    const unsigned mx = (s->twin & 31) * 8, my = (s->twin >> 5 & 31) * 8, ofx = (s->twin >> 10 & 31) * 8,
                   ofy = (s->twin >> 15 & 31) * 8;
    if (s->twin) sx0 = m->tx, sw = 256 >> shift, sy0 = m->ty;
    int ncl = m->depth == 0 ? 16 : m->depth == 1 ? 256 : 0;
    int texels_new = !s->stamp[b] || cells_newer(sx0, sy0, sx0 + sw, sy0 + 32, s->stamp[b]);
    int clut_new = ncl && (!s->stamp[b] || cells_newer(m->clut_x, m->clut_y, m->clut_x + ncl, m->clut_y + 1, s->stamp[b]));
    if (!texels_new && !clut_new) {
        s->epoch[b] = tex_epoch;
        return 0;
    }
    /* a write near its sources (the 32x32 cells are coarse: the world map scrolls 16-wide strips of water and cycles
     * single CLUT rows every frame) may have left them as they were: then the block stays. Texels and CLUT each
     * hashed again only if their own cells were written: the rewards rewrite a few uploads every frame in the cell of
     * the font's CLUTs, and every block of the font pages hashed its texels again with them (with a texture window,
     * the page's whole width: 130 of them a frame, 8.5 ms in Azahar as an Old 3DS) */
    u32 hsh = s->hash[b], chsh = s->chash[b];
    if (texels_new) cpu_reads(sx0, sy0, sx0 + sw, sy0 + 32), hsh = shadow_hash(sx0, sy0, sw, 32, 0x811C9DC5u);
    if (clut_new)
        cpu_reads(m->clut_x, m->clut_y, m->clut_x + ncl, m->clut_y + 1),
            chsh = shadow_hash(m->clut_x, m->clut_y, ncl, 1, 0x811C9DC5u);
    if (s->stamp[b] && s->hash[b] == hsh && s->chash[b] == chsh) {
        s->stamp[b] = gen_counter;
        s->epoch[b] = tex_epoch;
        return 0;
    }
    /* (drawn with earlier in this frame, or in the frame in flight: those draws keep the old texels. Only a block
     * already converted for this page: one converted for the first time was read by none of them, as a page met for
     * the first time mid-frame (models walking in: on a 3DS with the cache full, each such block waited for the GPU,
     * 10-15 waits a frame in Burmecia)) */
    /* (... and known current since the slot took the page, so maybe drawn with: a slot taken back by slot_renew holds
     * blocks of its time before, which nothing has drawn with since) */
    if (s->stamp[b] && s->epoch[b] && (s->serial == frame_serial || (gpu_busy && s->serial + 1 == frame_serial))) {
        renew_b = b, renew_hash = hsh, renew_chash = chsh;
        return 1;
    }
    if (gpu_busy && s->serial + 1 == frame_serial) frame_begin(); /* (the frame in flight's: it finishes first) */
    s->hash[b] = hsh;
    s->chash[b] = chsh;
    u32 clut[256];
    if (ncl) {
        const u16 *c = psx_vram + (m->clut_y & 511) * VW;
        for (int i = 0; i < ncl; i++) clut[i] = rgba_lut[c[(m->clut_x + i) & 1023]];
    }
    C3D_Tex *at = slot_tex(s);
    u32 *dst = at->data;
    const unsigned aw = at->width;
    unsigned flags = 0;
    if (!s->twin) { /* (no texture window, the usual: each depth's loop, the tiled address as a row's and a column's part) */
        unsigned col[32];
        for (int x = 0; x < 32; x++) {
            unsigned u = s->ox + bx * 32 + x;
            col[x] = (u >> 3) * 64 + morton_x[u & 7];
        }
        for (int y = 0; y < 32; y++) {
            unsigned v = s->oy + by * 32 + y;
            u32 *d = dst + (v >> 3) * (aw >> 3) * 64 + morton_y[v & 7];
            const u16 *row = psx_vram + ((m->ty + by * 32 + y) & 511) * VW;
            u32 c;
#define PUT(x) (d[col[x]] = c, flags |= (c & 0xFF) == 0xFF ? 1 : (c & 0xFF) == 0x80 ? 2 : 0)
            if (m->depth == 0)
                for (int x = 0; x < 32; x += 4) {
                    unsigned w = row[(m->tx + ((bx * 32 + x) >> 2)) & 1023];
                    c = clut[w & 15], PUT(x);
                    c = clut[w >> 4 & 15], PUT(x + 1);
                    c = clut[w >> 8 & 15], PUT(x + 2);
                    c = clut[w >> 12], PUT(x + 3);
                }
            else if (m->depth == 1)
                for (int x = 0; x < 32; x += 2) {
                    unsigned w = row[(m->tx + ((bx * 32 + x) >> 1)) & 1023];
                    c = clut[w & 255], PUT(x);
                    c = clut[w >> 8], PUT(x + 1);
                }
            else
                for (int x = 0; x < 32; x++) c = rgba_lut[row[(m->tx + bx * 32 + x) & 1023]], PUT(x);
#undef PUT
        }
    } else
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
            dst[tiled(s->ox + u, s->oy + v, aw)] = c;
        }
    }
    s->flags[b] = flags;
    s->fl1 = (s->fl1 & ~((u64)1 << b)) | (u64)(flags & 1) << b;
    s->fl2 = (s->fl2 & ~((u64)1 << b)) | (u64)(flags >> 1 & 1) << b;
    s->stamp[b] = gen_counter;
    s->epoch[b] = tex_epoch;
    mark_source(sx0, sy0, sx0 + sw, sy0 + 32);
    if (ncl) mark_source(m->clut_x, m->clut_y, m->clut_x + ncl, m->clut_y + 1);

    /* (the block's four rows of tiles, each the slot's 256 texels across: contiguous) */
    for (int r = 0; r < 4; r++) mark_dirty(dst + tiled(s->ox, s->oy + by * 32 + r * 8, aw), 256 * 8 * 4);
    ctr_render_stats[4]++;
    return 0;
}

/* Slots drawn one after the other are kept in one atlas where they can be: the pairs that follow each other are
 * counted, and every REGROUP_FRAMES frames, if a grouping (greedy, by those counts, 16 a group at most) would cut
 * the atlas changes by a quarter, the slots are moved into it (their texels copied cell to cell: a few ms, once). */
#define REGROUP_FRAMES 64
static u16 co[MAX_SLOTS][MAX_SLOTS];
static int co_last = -1;
static inline int cell_of(const TexSlot *t) {
    return t->oy / 256 * (atlases[t->atlas].tex.width / 256) + t->ox / 256;
}
/* a 256x256 cell copied on the GPU's transfer engine (no CPU time): its 32 rows of 8x8 tiles, each 256 texels across
 * and contiguous, with the gap to the next row in each texture */
static void copy_cell(u32 *dst, unsigned dw, unsigned dx, unsigned dy, const u32 *src, unsigned sw, unsigned sx,
                      unsigned sy) {
    const unsigned line = 256 * 8 * 4 / 16; /* (in 16-byte units) */
    C3D_SyncTextureCopy((u32 *)(src + tiled(sx, sy, sw)), GX_BUFFER_DIM(line, (sw - 256) * 8 * 4 / 16),
                        dst + tiled(dx, dy, dw), GX_BUFFER_DIM(line, (dw - 256) * 8 * 4 / 16), 256 * 256 * 4,
                        GX_TRANSFER_RAW_COPY(1));
}
static void slot_regroup(void) {
    static u32 frame, wait = REGROUP_FRAMES, made_at;
    if (++frame < wait || natlases < 2) return;
    frame = 0;
    if (slots_made - made_at >= 8) wait = REGROUP_FRAMES; /* (a new scene: looked at soon again) */
    made_at = slots_made;
    int n = nslots;
    static u8 grp[MAX_SLOTS], gsize[MAX_SLOTS];
    for (int i = 0; i < n; i++) grp[i] = (u8)i, gsize[i] = 1;
    /* (the pairs by count, greedy: largest first) */
    static struct { u16 w; u8 a, b; } pairs[MAX_SLOTS * MAX_SLOTS / 2];
    int np = 0;
    unsigned now = 0;
    for (int a = 0; a < n; a++)
        for (int b = a + 1; b < n; b++) {
            unsigned w = co[a][b] + co[b][a];
            if (!w) continue;
            if (slots[a].atlas != slots[b].atlas) now += w;
            if (np < (int)(sizeof pairs / sizeof pairs[0])) pairs[np].w = (u16)(w > 0xFFFF ? 0xFFFF : w), pairs[np].a = (u8)a, pairs[np].b = (u8)b, np++;
        }
    memset(co, 0, sizeof co);
    if (now < wait * 16) return; /* (fewer than 16 changes a frame: nothing worth a move) */
    for (int i = 1; i < np; i++) { /* (insertion sort by weight, descending: a few hundred pairs) */
        typeof(pairs[0]) t = pairs[i];
        int j = i - 1;
        while (j >= 0 && pairs[j].w < t.w) pairs[j + 1] = pairs[j], j--;
        pairs[j + 1] = t;
    }
    for (int k = 0; k < np; k++) {
        int ga = grp[pairs[k].a], gb = grp[pairs[k].b];
        if (ga == gb || gsize[ga] + gsize[gb] > 16) continue;
        for (int i = 0; i < n; i++) if (grp[i] == gb) grp[i] = (u8)ga;
        gsize[ga] += gsize[gb], gsize[gb] = 0;
    }
    /* the groups into the atlases, largest first (first fit), then the slots left anywhere free */
    static u8 na[MAX_SLOTS], nc[MAX_SLOTS];
    u16 occ[MAX_ATLASES] = {0};
    static u8 order[MAX_SLOTS];
    int ng = 0;
    for (int g = 0; g < n; g++) if (gsize[g] > 1) order[ng++] = (u8)g;
    for (int i = 1; i < ng; i++) { u8 t = order[i]; int j = i - 1; while (j >= 0 && gsize[order[j]] < gsize[t]) order[j + 1] = order[j], j--; order[j + 1] = t; }
    static u8 placed[MAX_SLOTS];
    memset(placed, 0, sizeof placed);
    /* (a group into the atlas holding most of it, if it has room; its slots there keep their cells: the fewest
     * moves) */
    for (int i = 0; i < n; i++) na[i] = 0xFF;
    for (int k = 0; k < ng; k++) {
        int g = order[k], best = -1, best_here = -1;
        for (int a = 0; a < natlases; a++) {
            int here = 0, free = atlases[a].cells - __builtin_popcount(occ[a]);
            for (int i = 0; i < n; i++) if (grp[i] == g && slots[i].atlas == a && !(occ[a] >> cell_of(&slots[i]) & 1)) here++;
            if (free < gsize[g]) continue;
            if (here > best_here) best_here = here, best = a;
        }
        if (best < 0) continue; /* (no room as a whole: its slots placed singly below) */
        for (int pass = 0; pass < 2; pass++)
            for (int i = 0; i < n; i++) {
                if (grp[i] != g || placed[i]) continue;
                int c = slots[i].atlas == best && !(occ[best] >> cell_of(&slots[i]) & 1) ? cell_of(&slots[i]) : -1;
                if (pass == 0 && c < 0) continue;
                if (c < 0) c = __builtin_ctz(~occ[best]);
                occ[best] |= (u16)(1u << c), na[i] = (u8)best, nc[i] = (u8)c, placed[i] = 1;
            }
    }
    for (int i = 0; i < n; i++) { /* (the rest: their own cell if still free) */
        if (placed[i] || slots[i].atlas == 0xFF) continue;
        int a = slots[i].atlas, c = cell_of(&slots[i]);
        if (occ[a] >> c & 1) continue;
        occ[a] |= (u16)(1u << c), na[i] = (u8)a, nc[i] = (u8)c, placed[i] = 1;
    }
    for (int i = 0; i < n; i++) { /* (and the ones whose cell went to a group: anywhere free) */
        if (placed[i] || slots[i].atlas == 0xFF) continue;
        int a;
        for (a = 0; a < natlases; a++) if (__builtin_popcount(occ[a]) < atlases[a].cells) break;
        if (a == natlases) return; /* (can't happen: as many cells as slots) */
        int c = __builtin_ctz(~occ[a]);
        occ[a] |= (u16)(1u << c), na[i] = (u8)a, nc[i] = (u8)c, placed[i] = 1;
    }
    /* the switches the new places would give (by the same counts): worth a move only for a quarter fewer */
    unsigned after = 0;
    for (int k = 0; k < np; k++) if (na[pairs[k].a] != na[pairs[k].b]) after += pairs[k].w;
    if (after * 2 > now) return; /* (worth a move only for half the changes or fewer) */
    if (wait < REGROUP_FRAMES * 32) wait *= 2; /* (then looked at again later and later) */
    /* move: nothing in flight may draw from the atlases meanwhile, and the GPU's copies run outside a frame */
    gpu_sync();
    if (in_frame) frame_end(), in_frame = 0;
    static u32 *tmp; /* (in linear memory: the GPU copies through it) */
    if (!tmp) tmp = linearAlloc(256 * 256 * 4);
    if (!tmp) return;
    /* cell ids a * 16 + c. Any slot whose new cell is free moves; again until none can; a cycle left is broken by
     * parking one slot's texels in tmp (its cell then free), and the rest follow */
    static int owner[MAX_ATLASES * 16], ocell[MAX_SLOTS], ncell[MAX_SLOTS];
    for (int k = 0; k < natlases * 16; k++) owner[k] = -1;
    for (int i = 0; i < n; i++) {
        ocell[i] = ncell[i] = -1;
        if (slots[i].atlas == 0xFF) continue;
        unsigned per = atlases[slots[i].atlas].tex.width / 256;
        ocell[i] = slots[i].atlas * 16 + slots[i].oy / 256 * per + slots[i].ox / 256;
        ncell[i] = na[i] * 16 + nc[i];
        owner[ocell[i]] = i;
    }
#define CELL_OF(k) (u32 *)atlases[(k) / 16].tex.data, atlases[(k) / 16].tex.width, \
                   (unsigned)((k) % 16 % (atlases[(k) / 16].tex.width / 256) * 256), \
                   (unsigned)((k) % 16 / (atlases[(k) / 16].tex.width / 256) * 256)
    int parked = -1;
    unsigned moves = 0;
    for (;;) {
        int progress = 0;
        for (int i = 0; i < n; i++) {
            if (ocell[i] < 0 || ocell[i] == ncell[i] || owner[ncell[i]] >= 0) continue;
            if (i == parked) copy_cell(CELL_OF(ncell[i]), tmp, 256, 0, 0), parked = -1;
            else copy_cell(CELL_OF(ncell[i]), CELL_OF(ocell[i])), owner[ocell[i]] = -1;
            owner[ncell[i]] = i, ocell[i] = ncell[i], moves++, progress = 1;
        }
        if (progress) continue;
        if (parked >= 0) break; /* (can't happen: a parked slot's cycle always frees its cell) */
        int i;
        for (i = 0; i < n; i++) if (ocell[i] >= 0 && ocell[i] != ncell[i]) break;
        if (i == n) break;
        copy_cell(tmp, 256, 0, 0, CELL_OF(ocell[i]));
        owner[ocell[i]] = -1, parked = i;
    }
#undef CELL_OF
    for (int a = 0; a < natlases; a++) atlases[a].occ = occ[a], atlases[a].used = (u8)__builtin_popcount(occ[a]);
    for (int i = 0; i < n; i++)
        if (slots[i].atlas != 0xFF) cell_xy(na[i], nc[i], &slots[i]);
    /* (written by the GPU: what the CPU's cache holds of them is stale, and a later write into a line of it would
     * flush the stale rest back) */
    for (int a = 0; a < natlases; a++) GSPGPU_InvalidateDataCache(atlases[a].tex.data, atlases[a].tex.size);
    frame_begin();
    half_restart();
    port_log("renderer: textures regrouped (%u slots moved, atlas changes %u -> %u)\n", moves, now, after);
}

/* The texture for a textured primitive whose texels span [u0,u1]x[v0,v1] (inclusive, 0..255; u1 < u0 when it
 * wraps), with its blocks current; *flags = the union of their texel kinds. */
/* A 15-bit texture read from VRAM the GPU has drawn (the field-to-battle swirl samples the frame it draws into): the
 * page copied on the GPU from the mirror into page_tex, once per frame and page, instead of the mirror read back to
 * the CPU and converted (that readback every frame held the transition at ~6 s per 120 frames on a 3DS). Alpha is
 * kept 1: the mirror's alpha is the PS1's mask bit, not transparency (a texel 0 is drawn black, not skipped); a
 * semi-transparent primitive's copy keeps it (alpha), for its two passes. */
static C3D_Tex page_tex;
static C3D_RenderTarget *page_rt;
static int page_x = -1, page_y = -1, page_alpha;
static unsigned page_serial;
static int gpu_owned(int x0, int y0, int x1, int y1) {
    int cx0, cy0, cx1, cy1;
    if (!cell_range(x0, y0, x1, y1, &cx0, &cy0, &cx1, &cy1)) return 0;
    for (int cy = cy0; cy < cy1; cy++)
        for (int cx = cx0; cx < cx1; cx++) {
            /* (a cell written in part (cell_cols) is the shadow's once the rest is read back, as before such cells were
             * kept: the same texture path as then for a page sampled there) */
            if (cell_cols[cy][cx]) readback_cells(cx, cy, cx + 1, cy + 1);
            if (!shadow_ok[cy][cx]) return 1;
        }
    return 0;
}
static void submit(void);
/* (alpha: the mirror's alpha (the mask bit) copied too, else 1) */
static C3D_Tex *mirror_page(int tx, int ty, int alpha) {
    trace("mirror_page", tx, ty);
    if (!page_rt) return NULL; /* (made at init: ctr_init) */
    if (page_x == tx && page_y == ty && page_alpha == alpha && page_serial == frame_serial && page_pending == nsubmits)
        return &page_tex;
    /* (in stereo the right eye's pass comes after the whole left one: with a second page copied in the same submit, its
     * primitives would read the last page. Submitted first then, as every copy was) */
    if (stereo_on && page_pending == nsubmits) submit();
    int cx0, cy0, cx1, cy1; /* (the mirror current there first: cells the CPU wrote since) */
    if (cell_range(tx, ty, tx + 256, ty + 256, &cx0, &cy0, &cx1, &cy1))
        for (int cy = cy0; cy < cy1; cy++)
            for (int cx = cx0; cx < cx1; cx++)
                if (!mirror_ok[cy][cx]) { blit_cells(cx0, cy0, cx1, cy1); cy = cy1; break; }
    /* recorded as a batch of its own, in the mirror's pass: drawn after what was drawn before this primitive and before
     * what samples it, in order with the rest (submit). It was drawn at once: everything recorded before it submitted
     * and the GPU's previous frame waited for, early in the frame (an Old 3DS's battle: 7-8 ms a frame waiting for the
     * GPU that the next frame's drawing could have overlapped) */
    Vtx *v = alloc_vtx(6);
    quad(v, 0, 0, 256, 256, tx, ty, tx + 256, ty + 256, 0xFFFFFFFF);
    int saved = cur_target, saved_mask = cur_mask;
    cur_target = 0;
    cur_mask = 0;
    add_batch(K_PAGE_COPY, alpha, 1, full_scissor, &mirror_tex[0], v - vb, 6);
    cur_target = saved;
    cur_mask = saved_mask;
    page_x = tx, page_y = ty, page_alpha = alpha, page_serial = frame_serial, page_pending = nsubmits;
    return &page_tex;
}
static void page_copy(const Batch *b) {
    C3D_FrameDrawOn(page_rt);
    set_proj_ortho(256, 256, 0.0f);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, u_stereo, 0, 0, 1, -0.5f); /* (in the clip volume, as the mirror's passes: at +0.5 the
                                                                * copy was clipped away, the page left its clear 0x00FF) */
    set_tev(K_BLIT);
    set_blend(NO_BLEND, 0);
    C3D_DepthTest(false, GPU_ALWAYS, b->abr ? GPU_WRITE_COLOR : GPU_WRITE_RED | GPU_WRITE_GREEN | GPU_WRITE_BLUE); /* (else alpha stays 1) */
    tex_bind(0, &mirror_tex[0]);
    set_texscale(&mirror_tex[0]);
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    draw_tris(b->first, b->count);
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
}

static C3D_Tex *texture_for(const Gp0Mode *m, int u0, int v0, int u1, int v1, unsigned *flags) {
    /* A semi-transparent 15-bit texture read from VRAM the GPU has drawn, outside the area being drawn into (the
     * field-to-battle swirl: the frame on screen warped into the next): the mirror sampled where it is, its alpha the
     * texels' bit 15 (the mask bit), which the two page passes split as the PS1 does (opaque without it, blended with
     * it). It was read back to the CPU and converted every frame (a readback and some ten GPU syncs a frame: the
     * swirl at 25-31 ms a frame in Azahar, the transition's lag on a 3DS) */
    if (m->depth == 2 && m->semi && !gp0_state.twin_mask_x && !gp0_state.twin_mask_y && u1 >= u0 && v1 >= v0) {
        int x0 = m->tx + u0, y0 = m->ty + v0, x1 = m->tx + u1 + 1, y1 = m->ty + v1 + 1;
        int apart = x1 <= gp0_state.area_x0 || x0 > gp0_state.area_x1 || y1 <= gp0_state.area_y0 || y0 > gp0_state.area_y1;
        if (apart && x1 <= VW && y1 <= VH && gpu_owned(x0, y0, x1, y1)) {
            int cx0, cy0, cx1, cy1; /* (the mirror current there: cells the CPU wrote since) */
            if (cell_range(x0, y0, x1, y1, &cx0, &cy0, &cx1, &cy1))
                for (int cy = cy0; cy < cy1; cy++)
                    for (int cx = cx0; cx < cx1; cx++)
                        if (!mirror_ok[cy][cx]) { blit_cells(cx0, cy0, cx1, cy1); cy = cy1; break; }
            *flags = 8;
            tex_ou = (float)m->tx, tex_ov = (float)m->ty;
            return &mirror_tex[0];
        }
        /* (where it takes in some of the area being drawn: the page as drawn so far, copied with its alpha. Flare's
         * afterimages sample half the frame before, x 160-320, drawing from x 320: read back and converted, hundreds
         * of readbacks in the spell) */
        if (!apart && x1 <= VW && y1 <= VH && gpu_owned(x0, y0, x1, y1)) {
            C3D_Tex *t = mirror_page(m->tx, m->ty, 1);
            if (t) { *flags = 8; tex_ou = tex_ov = 0; return t; }
        }
    }
    /* (a translucent primitive's texels by their bit 15, as the slots hold it (alpha 0xFF / 0x80, flags): the copied
     * page has the mirror's one alpha bit, and with no flags it drew nothing, Demi's sphere and its glow on the ground) */
    if (m->depth == 2 && !m->semi && !gp0_state.twin_mask_x && !gp0_state.twin_mask_y &&
        gpu_owned(m->tx + u0, m->ty + v0, m->tx + u1 + 1, m->ty + v1 + 1)) {
        C3D_Tex *t = mirror_page(m->tx, m->ty, 0);
        if (t) { *flags = 0; tex_ou = tex_ov = 0; return t; }
    }
    u32 key = (u32)m->depth << 30 | (u32)(m->tx >> 6) << 26 | (u32)(m->ty >> 8) << 25 |
              (m->depth < 2 ? (u32)(m->clut_x >> 4) << 9 | (u32)m->clut_y : 0) | 1u << 24;
    u32 twin = (gp0_state.twin_mask_x / 8) | (gp0_state.twin_mask_y / 8) << 5 | (gp0_state.twin_off_x / 8) << 10 |
               (gp0_state.twin_off_y / 8) << 15;
    int bx0 = u0 >> 5, bx1 = u1 >> 5, by0 = v0 >> 5, by1 = v1 >> 5;
    if (bx1 < bx0) bx0 = 0, bx1 = 7;
    if (by1 < by0) by0 = 0, by1 = 7;
    /* the blocks made current: the primitive's, and those a texel beyond its texels (cx/cy): a 3DS's GPU may sample a
     * texel past the edge of what a primitive spans (its precision), which in a block not yet converted (a slot just
     * given a page) is whatever was there before: black and white specks and triangles on the world map's far ground as
     * the camera turned, on an Old 3DS. The flags (which passes it needs) are its own blocks' alone */
    int cx0 = bx0, cx1 = bx1, cy0 = by0, cy1 = by1;
    if (u1 >= u0) cx0 = (u0 > 0 ? u0 - 1 : 0) >> 5, cx1 = (u1 < 255 ? u1 + 1 : 255) >> 5;
    if (v1 >= v0) cy0 = (v0 > 0 ? v0 - 1 : 0) >> 5, cy1 = (v1 < 255 ? v1 + 1 : 255) >> 5;
    /* (the same page, CLUT, window and blocks as the last lookup, nothing written to any source since and the same
     * frame: its answer again, every block then current (convert_block's first test). Runs of glyphs from one font
     * block, of a model's faces from one texture block, are most lookups) */
    static struct { u32 key, twin, epoch, serial; int b; TexSlot *s; unsigned f; } last;
    const int blocks = bx0 | bx1 << 3 | by0 << 6 | by1 << 9 | cx0 << 12 | cx1 << 15 | cy0 << 18 | cy1 << 21;
    TexSlot *s;
    if (last.s && last.key == key && last.twin == twin && last.serial == frame_serial && last.s->key == key &&
        last.s->twin == twin) {
        if (last.epoch == tex_epoch && last.b == blocks) {
            *flags = last.f;
            tex_ou = last.s->ox, tex_ov = last.s->oy;
            co_last = (int)(last.s - slots);
            return slot_tex(last.s);
        }
        s = last.s; /* (the same page as the last lookup, other blocks: the world map's ground, face after face) */
    } else
        s = slot_for(key, twin);
    unsigned f = 0;
    /* (the blocks wanted, as a mask: when every one is known current, convert_block's first test for each at once.
     * Nearly every lookup misses the one before (a model's faces alternate between pages, CLUTs and blocks)) */
    static const u64 rows[8] = {0x01, 0x0101, 0x010101, 0x01010101, 0x0101010101ull, 0x010101010101ull,
                                0x01010101010101ull, 0x0101010101010101ull}; /* (n + 1 rows from row 0) */
    const u64 want = (u64)(((2u << cx1) - 1) & ~((1u << cx0) - 1)) * (rows[cy1 - cy0] << (cy0 * 8));
    if (s->cur_epoch == tex_epoch && !(want & ~s->cur)) {
        const u64 own = (u64)(((2u << bx1) - 1) & ~((1u << bx0) - 1)) * (rows[by1 - by0] << (by0 * 8));
        f = (own & s->fl1 ? 1 : 0) | (own & s->fl2 ? 2 : 0);
    } else {
        for (int by = cy0; by <= cy1; by++)
            for (int bx = cx0; bx <= cx1; bx++) {
                if (convert_block(s, m, bx, by)) { s = slot_renew(s), f = 0, by = cy0, bx = cx0 - 1; continue; }
                if (bx >= bx0 && bx <= bx1 && by >= by0 && by <= by1) f |= s->flags[by * 8 + bx];
            }
        if (s->cur_epoch != tex_epoch) s->cur = 0, s->cur_epoch = tex_epoch;
        for (int by = cy0; by <= cy1; by++) /* (each checked: a conversion may have moved the epoch on) */
            for (int bx = cx0; bx <= cx1; bx++) {
                const int b = by * 8 + bx;
                if (s->stamp[b] && s->epoch[b] == tex_epoch) s->cur |= (u64)1 << b;
            }
    }
    s->serial = frame_serial;
    *flags = f;
    tex_ou = s->ox, tex_ov = s->oy;
    last.key = key, last.twin = twin, last.epoch = tex_epoch, last.serial = frame_serial, last.b = blocks, last.s = s,
    last.f = f;
    int id = (int)(s - slots);
    if (co_last >= 0 && co_last != id && co[co_last][id] < 0xFFFF) co[co_last][id]++;
    co_last = id;
    return slot_tex(s);
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

static int world_ground; /* (ctr_present: the world map is up) */
static inline void make_vtx(Vtx *o, const Gp0Mode *m, const Gp0Vtx *v) {
    o->x = (float)v->x;
    o->y = (float)v->y;
    o->z = 0;
    /* (the lookup inline (gte_depth.h): on an Old 3DS the call for every vertex drawn was 0.4 ms of a busy field's
     * VBlank, which an emulator doesn't show) */
    /* (in 3D only: in 2D the PS1's own drawing, its corners whole pixels and its textures affine. With the GTE's
     * depths there, sub-pixel corners and perspective-correct textures went to the ~78% of model vertices its by-value
     * table kept (Lindblum), which could change from frame to frame; against the PS1's frame (the host's software
     * renderer, Lindblum's castle station) 2727 pixels differed, 2043 without them, and the lookups were ~2 ms of a
     * model-heavy field's VBlank on an Old 3DS) */
    u32 info = v->xyw && !getenv_nodepth && stereo_on ? psx_depth_find(v->xyw, &psx_depth_by_value) : 0;
    ctr_depth_stats[info ? 0 : 1]++;
    if (info) {
        /* (not on the world map: its ground's blocks are projected each on its own, and the fractions of a vertex they
         * share differ, moving one block's edge off its neighbour's; the PS1 draws from the whole pixel) */
        if (!world_ground) {
            o->x += (info & 0xFF) * (1.0f / 256);
            o->y += ((info >> 8) & 0xFF) * (1.0f / 256);
        }
        /* (on the world map, found by value, the depth may be another vertex's that projected to the same pixel (its
         * ground reaches the horizon: many vertices per pixel) or an older frame's: for the parallax alone (negative),
         * the texture mapped flat as the PS1 did. As a perspective, a wrong one warped the face's texture past its edge
         * onto transparent texels: black gaps along the ground. Elsewhere (battles: their models' vertices staged and
         * copied, found by value) the depth is right, and flat there broke the models: black boxes, smeared faces) */
        o->z = psx_depth_by_value && world_ground ? -(float)(info >> 16) : (float)(info >> 16);
    }
    /* (the world map's texels as the PS1 picks them: the texel nearest the interpolated (u, v), where the GPU's nearest
     * filter takes the one it falls in. At a corner (u, v) is whole, on a texel's edge, and the GPU's rounding took the
     * texel before as often as not: a transparent one at a block's corner, the fog drawn under it added twice, a pale
     * dot. Half a texel on, the same texels as the PS1's everywhere) */
    const float uv_half = world_ground && !ui_mode ? 0.5f : 0;
    o->u = (float)v->u + uv_half;
    o->v = (float)v->v + uv_half;
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

/* The screen plane (convergence) is at the scene's nearest depth: everything goes into the screen, so the windows
 * and text at the screen plane are always in front of what they cover (a model nearer than the plane they're drawn
 * over is a clash the eyes can't resolve). A field's background layers all go at one depth, the scene's mean: a
 * layer of its own depth comes apart from the layer under it (an overlay cut from the same picture, as a table over
 * the floor), which shows as a ghost or a hole beside it. In a field the screen plane is that background depth
 * instead: a model nearer than it (the shader flattens what is nearer than the plane) would stand out of the picture
 * while the picture's own overlays (the pots in front of the Alexandria kitchen's chef) still cut into it. */
static float zc_smooth = 1500.0f;   /* the scene's mean depth, smoothed */
static float near_smooth = 1500.0f; /* its nearest, smoothed: the convergence depth */
static float fit_zmin = 1e9f;       /* this frame's nearest sample */
static int ctr_flat_bg;             /* in a field: its background layers at one depth (ctr_flat_background) */
void ctr_flat_background(int on) { psx_gpu_sync(); ctr_flat_bg = on; } /* (port/game/field_hud.c, each frame) */
/* The world map's terrain in stereo: each block of it is projected on its own (near and far blocks at different
 * precision), so where two meet the same ground point has slightly different depths, and each side shifted by its
 * own parallax: hairline cracks along the blocks' seams. Its depth is instead the ground's as seen: 1/Z is linear
 * in screen y for a plane in perspective, fitted over the terrain's own vertices each frame (the last frame's line):
 * points that meet on the screen get the same parallax. Models keep theirs. (port/game/world_native.c reports the
 * blocks' packets: port_terrain_packets.) */
extern const u32 *psx_gpu_packet;
int port_game_mode(void);
static int model_packet(void);
#define MAX_TERRAIN 64
static struct { const u8 *lo, *hi; unsigned frame; } terrain[MAX_TERRAIN];
static int nterrain;
void port_terrain_packets(const void *lo, const void *hi) {
    if (lo == hi) return;
    if (psx_gpu_stage(STG_TERRAIN, (const int[]){(int)lo, (int)hi}, 2, NULL, 0)) return;
    for (int i = 0; i < nterrain; i++)
        if (frames_drawn - terrain[i].frame > 1) terrain[i] = terrain[--nterrain], i--;
    if (nterrain && terrain[nterrain - 1].hi == (const u8 *)lo && terrain[nterrain - 1].frame == frames_drawn) {
        terrain[nterrain - 1].hi = hi; /* (blocks write one after another: one range) */
        return;
    }
    if (nterrain == MAX_TERRAIN) nterrain--;
    terrain[nterrain].lo = lo, terrain[nterrain].hi = hi, terrain[nterrain].frame = frames_drawn;
    nterrain++;
}
static int terrain_packet(void) {
    const u8 *p = (const u8 *)psx_gpu_packet;
    for (int i = 0; i < nterrain; i++)
        if (p >= terrain[i].lo && p < terrain[i].hi) return 1;
    return 0;
}
static float tf_n, tf_y, tf_w, tf_yy, tf_yw, tf_wmin = 1e9f, tf_wmax = -1e9f; /* this frame's sums: w = 1 / Z by y */
static float tf_a, tf_b, tf_lo, tf_hi;                                        /* the last line, and its w range */
static int tf_ok;
#define WELD_N 4096 /* (terrain_depth: this frame's corners by pixel, open addressing) */
static struct { unsigned frame; short x, y; float z; } weld[WELD_N];
/* (int)floorf(x), for x within int's range: the Old 3DS's VFPv2 has no rounding instruction and floorf is a library
 * call, two for each model corner in 3D (model_weld: some 2.7 ms of a busy field's VBlank there with the weld) */
static inline int floor_int(float x) {
    const int i = (int)x; /* (toward zero) */
    return (float)i > x ? i - 1 : i;
}
static const Vtx *terrain_depth(const Vtx *v, unsigned n, Vtx *out) {
    int bare = 0;
    for (unsigned i = 0; i < n; i++) {
        if (v[i].z == 0) { bare++; continue; } /* (a corner whose word the GTE didn't store: no depth of its own) */
        float w = 1.0f / fabsf(v[i].z), y = v[i].y - (float)gp0_state.area_y0; /* (from the frame's top: either buffer) */
        tf_n += 1; tf_y += y; tf_w += w; tf_yy += y * y; tf_yw += y * w;
        if (w < tf_wmin) tf_wmin = w;
        if (w > tf_wmax) tf_wmax = w;
    }
    (void)bare;
    /* (a corner without one gets the ground's, for its parallax only: as a negative depth, which the shader takes for
     * the shift and not for the texture's perspective (the face was mapped flat): some faces have none at all, and
     * one left at the screen plane beside shifted ones cracked the ground along their edges; given as a depth, their
     * textures warped into rows. And a corner on a pixel another face's corner had this frame takes that one's depth:
     * the blocks' depths differ there (and a bare corner's fit from a real one's), pinholes in stereo.) */
    for (unsigned i = 0; i < n; i++) {
        out[i] = v[i];
        float z = fabsf(v[i].z); /* (negative: a parallax depth (found by value), a depth all the same) */
        if (z <= 0) {
            if (!tf_ok) continue;
            float w = tf_a * (v[i].y - (float)gp0_state.area_y0) + tf_b;
            if (w < tf_lo) w = tf_lo;
            if (w > tf_hi) w = tf_hi;
            z = 1.0f / w;
        }
        int px = floor_int(v[i].x + 0.5f), py = floor_int(v[i].y + 0.5f);
        unsigned h = ((unsigned)px * 73856093u ^ (unsigned)py * 19349663u) & (WELD_N - 1);
        for (unsigned k = 0; k < 8; k++, h = (h + 1) & (WELD_N - 1)) {
            if (weld[h].frame != frames_drawn + 1) {
                weld[h].frame = frames_drawn + 1, weld[h].x = (short)px, weld[h].y = (short)py, weld[h].z = z;
                break;
            }
            if (weld[h].x == px && weld[h].y == py) { z = weld[h].z; break; }
        }
        out[i].z = v[i].z > 0 ? z : -z;
    }
    return out;
}
static void terrain_fit_frame(void) {
    float den = tf_n * tf_yy - tf_y * tf_y;
    tf_ok = tf_n >= 64 && den > 1e-3f && tf_wmin > 0;
    if (tf_ok) {
        tf_a = (tf_n * tf_yw - tf_y * tf_w) / den;
        tf_b = (tf_w - tf_a * tf_y) / tf_n;
        tf_lo = tf_wmin, tf_hi = tf_wmax;
    }
    tf_n = tf_y = tf_w = tf_yy = tf_yw = 0;
    tf_wmin = 1e9f, tf_wmax = -1e9f;
}

static void fit_frame(void) {
    terrain_fit_frame();
    if (fit_n >= 16) {
        zc_smooth += (fit_z / fit_n - zc_smooth) * 0.1f;
        float nz = fit_zmin < zc_smooth / 4 ? zc_smooth / 4 : fit_zmin > zc_smooth ? zc_smooth : fit_zmin;
        near_smooth += (nz - near_smooth) * 0.1f;
        /* the convergence held while the scene's depth stays within 8% of it, followed when it moves further (the camera
         * moving, a new scene) until within 1%: the smoothed depths drift a little every frame as the samples vary
         * (1779-1789 of them on a still world map, its mean never settling), and each drift moved the whole scene's
         * parallax by a fraction of a pixel, its texels resampled: in stereo a still scene shimmered (a 3DS video; in
         * Azahar 0.7-0.9% of pixels changed a frame against 0.1-0.2% in 2D) */
        static float zc_used = 0;
        static int following;
        float want = ctr_flat_bg ? zc_smooth : near_smooth;
        if (zc_used <= 0) zc_used = want;
        if (fabsf(want - zc_used) > 0.08f * zc_used) following = 1;
        if (following) {
            zc_used += (want - zc_used) * 0.25f;
            if (fabsf(want - zc_used) < 0.01f * zc_used) following = 0;
        }
        stereo_inv_zc = 1.0f / zc_used;
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
    fit_zmin = 1e9f;
    fit_lo = 1e9f;
    fit_hi = -1e9f;
}

static const Vtx *ot_depth(const Vtx *v, unsigned n, Vtx *out, int w, int h) {
    if (psx_ot_entry < 0 || psx_ot_side) return v; /* (the UI's table: the screen plane) */
    float s = (float)psx_ot_entry, z = 0;
    /* (a parallax depth (negative: terrain_depth's, or found by value) is a sample as a projected one is: the line and
     * the convergence (zc_smooth) are fitted on them all, as before those were made parallax depths) */
    for (unsigned i = 0; i < n; i++) z += fabsf(v[i].z);
    if (z > 0) { /* projected: a sample */
        z /= n;
        fit_n += 1; fit_s += s; fit_z += z; fit_ss += s * s; fit_sz += s * z;
        if (z < fit_zmin) fit_zmin = z;
        if (s < fit_lo) fit_lo = s;
        if (s > fit_hi) fit_hi = s;
        return v;
    }
    if (fit_age > 30 || (w >= 300 && h >= 200)) return v;
    /* (the line holds over the entries it was fitted on; past them, as at their nearest end: a scene of one model, as
     * the opening's dark room, would otherwise extrapolate a glow around it far off its depth) */
    if (s < fit_min) s = fit_min;
    if (s > fit_max) s = fit_max;
    /* (a field's background: at the screen plane itself, which is its depth there. Placed at a depth, its tiles
     * moved by a fraction of a pixel per eye, and the joins between them showed as vertical lines) */
    if (ctr_flat_bg) return v;
    float d = fit_a * s + fit_b;
    if (d < 64) return v;
    for (unsigned i = 0; i < n; i++) out[i] = v[i], out[i].z = d;
    return out;
}

static int ui_ox, ui_oy; /* the displayed buffer's origin, which the UI list draws at */
static int ui_list_layer;  /* the layer the UI list draws into */
static int ui_layer_for(int x, int y, int clear);
int ctr_keep_layers; /* the UI layers are kept by their owner (the rewards under a battle): no restart, no staleness */
static int ui_packet(void);
static int flat_packet(void);
static int in_window_packets(const void *p);
extern int ctr_outline;
static int model_packet(void);
static int model_index(void);
static const Vtx *model_weld(const Vtx *v, unsigned n, Vtx *out, int m);
static int outline_init(void);

static const u8 *zoom_lo, *zoom_hi; /* port_bottom_zoom's */
static float zoom_cx, zoom_cy;
static int zoom_factor;
static unsigned zoom_frame; /* (frames_drawn when set: as shift_frame) */
/* packets [shift_lo, shift_hi) of this frame moved by (shift_dx, shift_dy) on the bottom screen (port_bottom_shift) */
static const u8 *shift_lo, *shift_hi;
static int shift_dx, shift_dy;
static unsigned shift_frame; /* (frames_drawn when set: the frame's packets are drawn then or at the next) */
void port_bottom_shift(const void *lo, const void *hi, int dx, int dy) {
    if (psx_gpu_stage(STG_SHIFT, (const int[]){(int)lo, (int)hi, dx, dy}, 4, NULL, 0)) return;
    shift_lo = lo, shift_hi = hi, shift_dx = dx, shift_dy = dy;
    shift_frame = frames_drawn;
}
extern const u32 *psx_gpu_packet;
/* the battle screen's own command rows and party cards replace the PS1's command window and party panel (OT entries
 * 14-16 and 18-20, and the panel header at 7-8 (shared with the lists, not open then): battle_ui.c); the target window and
 * its HP readout at 9-10 (and 4-5) the port's targets; Select's help at 2 on the top screen. Asked before a primitive's
 * texture and vertices are made (r_poly, r_rect, r_line): the battle list is drawn every VBlank, these being most of it */
static int battle_list_replaced(void) {
    extern int psx_ui_list_battle, port_battle_hud_on, port_battle_hud_targets, port_battle_help_on;
    if (!ui_mode || !psx_ui_list_battle || !port_battle_hud_on) return 0;
    int slot = 0x17 - psx_ot_entry;
    if (slot == 7 || slot == 8 || slot == 14 || slot == 15 || slot == 16 || (slot >= 18 && slot <= 20)) return 1;
    if (port_battle_hud_targets && (slot == 4 || slot == 5 || slot == 9 || slot == 10)) return 1;
    return port_battle_help_on && slot == 2;
}
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
    if (battle_list_replaced()) return; /* (the port's own: battle_list_replaced) */
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
        else if (tflags & 8) passes2[np2++] = K_PAGE_OP, passes2[np2++] = K_PAGE_SEMI;
        else {
            if (tflags & 1) passes2[np2++] = K_TEX_P1;
            if (tflags & 2) passes2[np2++] = K_TEX_P2;
        }
        for (int p = 0; p < np2; p++) {
            Vtx *o = alloc_vtx(n);
            memcpy(o, v, n * sizeof *v);
            add_batch(passes2[p], passes2[p] == K_TEX_P1 || passes2[p] == K_PAGE_OP ? NO_BLEND : abr2, 3, tsc, tex, o - vb, n);
        }
        return;
    }
    if (ui_mode && !(psx_ui_list_battle && 0x17 - psx_ot_entry == 17)) layer = ui_list_layer, ox = ui_ox, oy = ui_oy;
    /* (a 15-bit texture is a frame copied from VRAM, as the field-to-battle swirl draws: the scene, never a window,
     * whose art is CLUT-based, even when the game builds it in its window packets) */
    /* (the card game's bottom screen is its frame itself (present_bottom): its windows, the "Start the game?" Yes/No
     * over the stock, are drawn into it in their place. In a UI layer, which that screen doesn't show, the question
     * was missing behind the card selection board) */
    else if (menu_state != 3 && (menu_state == 2 || ui_packet()) && !(m && m->textured && m->depth == 2)) { /* (a menu: the whole frame) */
        int x, y, w, h;
        psx_gpu_draw_area(&x, &y, &w, &h);
        layer = ui_layer_for(x, y, 0);
        ox = x;
        oy = y;
    }
    if (layer >= 0) { /* into a UI layer, relative to its framebuffer; the mirror isn't touched */
        if (layers[layer].clear_pending) layer_clear(layer);
        for (unsigned i = 0; i < n; i++) moved[i] = v[i], moved[i].x -= ox, moved[i].y -= oy;
        /* (as the block move below, only the frame's it was set for: the chocobo's menu opened over the zoomed map drew its
         * choices into the map's packets' place, and a frame's zoom left in force enlarged them, flickering) */
        if (zoom_factor > 1 && (const u8 *)psx_gpu_packet >= zoom_lo && (const u8 *)psx_gpu_packet < zoom_hi &&
            frames_drawn - zoom_frame <= 1)
            for (unsigned i = 0; i < n; i++) /* (the world map's navigation map, zoomed: the party in the middle) */
                moved[i].x = 160 + (moved[i].x - zoom_cx) * zoom_factor,
                moved[i].y = 112 + (moved[i].y - zoom_cy) * zoom_factor;
        int sdx = 0, sdy = 0; /* (a moved block: its vertices and its clip together) */
        /* (only the frame's it was set for: left in force, a later frame's packets built there moved too, as a menu opened
         * in Chocobo's Forest after its dialogue panels, its command list drawn shifted up) */
        if ((const u8 *)psx_gpu_packet >= shift_lo && (const u8 *)psx_gpu_packet < shift_hi && frames_drawn - shift_frame <= 1)
            sdx = shift_dx, sdy = shift_dy;
        for (unsigned i = 0; sdx | sdy ? i < n : 0; i++) moved[i].x += sdx, moved[i].y += sdy;
        v = moved;
        sc[0] = (u16)clampi(sc[0] - ox + sdx, 0, UI_W); sc[2] = (u16)clampi(sc[2] - ox + sdx, 0, UI_W);
        sc[1] = (u16)clampi(sc[1] - oy + sdy, 0, UI_H); sc[3] = (u16)clampi(sc[3] - oy + sdy, 0, UI_H);
        if (sc[0] >= sc[2] || sc[1] >= sc[3]) return;
        layers[layer].drawn = frames_drawn;
        layers[layer].draws++;
        cur_target = layer + 1;
        { /* (what its next clear must cover: ui_layer_for) */
            float fx0 = v[0].x, fx1 = v[0].x, fy0 = v[0].y, fy1 = v[0].y;
            for (unsigned i = 1; i < n; i++)
                fx0 = fminf(fx0, v[i].x), fx1 = fmaxf(fx1, v[i].x), fy0 = fminf(fy0, v[i].y), fy1 = fmaxf(fy1, v[i].y);
            UiLayer *L = &layers[layer];
            int x0 = imax(sc[0], (int)fx0 - 1), x1 = imin(sc[2], (int)fx1 + 2);
            int y0 = imax(sc[1], (int)fy0 - 1), y1 = imin(sc[3], (int)fy1 + 2);
            if (x0 < x1 && y0 < y1) {
                if (L->dx0 >= L->dx1) L->dx0 = x0, L->dy0 = y0, L->dx1 = x1, L->dy1 = y1;
                else L->dx0 = imin(L->dx0, x0), L->dy0 = imin(L->dy0, y0), L->dx1 = imax(L->dx1, x1), L->dy1 = imax(L->dy1, y1);
            }
        }
    } else {
        gpu_draws(bx0, by0, bx1, by1);
        cur_target = 0;
        /* (the field's windows, and anything under a drawing area smaller than a frame (a window's clip, a draw off
         * screen), stay at the screen plane: else text shifts per eye inside a box or clip that doesn't, and each eye
         * cuts it in a different place) */
        /* (the welds are for the parallax alone: in 2D they would only cost time) */
        if (stereo_on && nterrain && terrain_packet()) v = terrain_depth(v, n, moved); /* (the world map's ground) */
        /* (the entries' depth line is for the parallax alone: in 2D it would only cost time, some 4% of a battle's walk) */
        if (stereo_on && gp0_state.area_x1 - gp0_state.area_x0 >= 255 && gp0_state.area_y1 - gp0_state.area_y0 >= 200 &&
            !flat_packet())
            v = ot_depth(v, n, moved, bx1 - bx0, by1 - by0);
        int mi = stereo_on ? model_index() : 0;
        if (mi) v = model_weld(v, n, moved, mi); /* (a model's meshes meeting: one depth where their corners do) */
    }
    cur_mask = (gp0_state.set_mask ? MASK_SET : 0) | (gp0_state.check_mask ? MASK_CHECK : 0) |
               (layer < 0 && (gp0_state.texpage >> 9 & 1) ? MASK_DITHER : 0);
    ctr_render_stats[1]++;
    int passes[2], np = 0, abr = m->semi ? m->abr : NO_BLEND;
    if (!m->textured) passes[np++] = K_UNTEX;
    else if (!m->semi) passes[np++] = K_TEX;
    else if (tflags & 8) passes[np++] = K_PAGE_OP, passes[np++] = K_PAGE_SEMI;
    else {
        if (tflags & 1) passes[np++] = K_TEX_P1;
        if (tflags & 2) passes[np++] = K_TEX_P2;
    }
    for (int p = 0; p < np; p++) {
        int pabr = passes[p] == K_TEX_P1 || passes[p] == K_PAGE_OP ? NO_BLEND : abr;
        if (layer < 0 && n % 3 == 0) {
            run_add(passes[p], pabr, sc, tex, v, n);
            continue;
        }
        Vtx *o = alloc_vtx(n);
        memcpy(o, v, n * sizeof *v);
        add_batch(passes[p], pabr, 3, sc, tex, o - vb, n);
    }
    /* a model's polygon: again into the outline mask, white (outline_draw) */
    if (ctr_outline && layer < 0 && !ui_mode && n % 3 == 0 && model_packet() && outline_init()) {
        Vtx *o = alloc_vtx(n);
        for (unsigned i = 0; i < n; i++) o[i] = v[i], o[i].rgba = 0xFFFFFFFF, o[i].d = 0;
        int saved = cur_target;
        cur_target = 4;
        add_batch(K_UNTEX, NO_BLEND, 3, sc, NULL, o - vb, n);
        cur_target = saved;
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

static int small_poly(const Gp0Vtx *v, int n) { /* (under 64 pixels across and high) */
    int x0 = v[0].x, x1 = v[0].x, y0 = v[0].y, y1 = v[0].y;
    for (int i = 1; i < n; i++) x0 = imin(x0, v[i].x), x1 = imax(x1, v[i].x), y0 = imin(y0, v[i].y), y1 = imax(y1, v[i].y);
    return x1 - x0 < 64 && y1 - y0 < 64;
}
/* a convex polygon (3 corners, or 4 in the PS1's order: outline 0, 1, 3, 2) grown by d pixels: each edge moved out
 * along its normal, each corner to where its two moved edges meet (no further than 3d: a sharp corner) */
static void grow_ring(Vtx *t, int n, const int *r, float d);
static void grow_poly(Vtx *t, int n, float d) {
    static const int ring3[3] = {0, 1, 2}, ring4[4] = {0, 1, 3, 2};
    if (n == 4) { /* a quad that isn't convex (its outline turning both ways): its two triangles each on their own */
        int pos = 0, neg = 0;
        for (int k = 0; k < 4; k++) {
            const Vtx *a = &t[ring4[k]], *b = &t[ring4[(k + 1) % 4]], *c = &t[ring4[(k + 2) % 4]];
            float z = (b->x - a->x) * (c->y - b->y) - (b->y - a->y) * (c->x - b->x);
            pos += z > 1e-4f, neg += z < -1e-4f;
        }
        if (pos && neg) {
            Vtx a[3] = {t[0], t[1], t[2]}, b[3] = {t[1], t[3], t[2]};
            grow_ring(a, 3, ring3, d);
            grow_ring(b, 3, ring3, d);
            t[0] = a[0], t[3] = b[1];
            t[1].x = (a[1].x + b[0].x) * 0.5f, t[1].y = (a[1].y + b[0].y) * 0.5f;
            t[2].x = (a[2].x + b[2].x) * 0.5f, t[2].y = (a[2].y + b[2].y) * 0.5f;
            return;
        }
    }
    grow_ring(t, n, n == 4 ? ring4 : ring3, d);
}
static void grow_ring(Vtx *t, int n, const int *r, float d) {
    float area = 0;
    for (int k = 0; k < n; k++) {
        const Vtx *a = &t[r[k]], *b = &t[r[(k + 1) % n]];
        area += a->x * b->y - b->x * a->y;
    }
    if (area == 0) return;
    float side = area > 0 ? 1.0f : -1.0f; /* (the outward normal of edge a -> b: (dy, -dx) for a positive area) */
    float nx[4], ny[4];
    for (int k = 0; k < n; k++) { /* edge k: corner k to corner k + 1 */
        const Vtx *a = &t[r[k]], *b = &t[r[(k + 1) % n]];
        float dx = b->x - a->x, dy = b->y - a->y, l = sqrtf(dx * dx + dy * dy);
        if (l < 1e-4f) { nx[k] = ny[k] = 0; continue; }
        nx[k] = side * dy / l, ny[k] = -side * dx / l;
    }
    float ox[4], oy[4];
    for (int k = 0; k < n; k++) { /* corner k: between edge k - 1 and edge k */
        int p = (k + n - 1) % n;
        float ax = nx[p], ay = ny[p], bx = nx[k], by = ny[k];
        if (ax == 0 && ay == 0) ax = bx, ay = by;
        if (bx == 0 && by == 0) bx = ax, by = ay;
        float den = 1 + ax * bx + ay * by, mx = ax + bx, my = ay + by;
        float x = den > 1e-3f ? d * mx / den : 0, y = den > 1e-3f ? d * my / den : 0, l = sqrtf(x * x + y * y);
        if (l > 3 * d) x *= 3 * d / l, y *= 3 * d / l;
        ox[k] = x, oy[k] = y;
    }
    for (int k = 0; k < n; k++) t[r[k]].x += ox[k], t[r[k]].y += oy[k];
}

/* A polygon in the plain case: into the scene's mirror in 2D (no UI list or layer, no menu, no outlines), as r_poly and
 * emit draw it there: the same vertices, passes, batches and depth values, without their general
 * cases. Each vertex is made once and written where the run keeps it (it went through make_vtx, a copy per triangle
 * and one into the run: on an Old 3DS, where this path is bound by its instructions, some 14 us a polygon) */
static void r_poly_plain(const Gp0Mode *m, const Gp0Vtx *v, int n) {
    unsigned tflags = 0;
    C3D_Tex *tex = prim_texture(m, v, n, &tflags);
    ctr_depth_stats[1] += n;
    const float ou = tex ? (float)tex_ou : 0, ov = tex ? (float)tex_ov : 0;
    /* (make_vtx's half texel on the world map, then the slot's place: the same sums in the same order) */
    const float uvh = world_ground ? 0.5f : 0;
    Vtx t[4];
    for (int i = 0; i < n; i++) {
        t[i].x = (float)v[i].x + 5.0f / 16;
        t[i].y = (float)v[i].y + 7.0f / 16;
        t[i].z = 0;
        t[i].u = (float)v[i].u + uvh;
        t[i].v = (float)v[i].v + uvh;
        if (tex) t[i].u += ou, t[i].v += ov;
        t[i].rgba = vcolor(m, &v[i]);
    }
    /* (the PS1's triangles (0,1,2) and (1,3,2), each skipped 1024 or more wide or 512 or more high: r_poly) */
    static const u8 tri[2][3] = {{0, 1, 2}, {1, 3, 2}};
    u8 idx[6];
    unsigned nout = 0;
    int x0 = 1 << 30, y0 = 1 << 30, x1 = -(1 << 30), y1 = -(1 << 30);
    for (int k = 0; k < (n == 4 ? 2 : 1); k++) {
        const Gp0Vtx *a = &v[tri[k][0]], *b = &v[tri[k][1]], *c = &v[tri[k][2]];
        const int tx0 = imin(imin(a->x, b->x), c->x), tx1 = imax(imax(a->x, b->x), c->x);
        const int ty0 = imin(imin(a->y, b->y), c->y), ty1 = imax(imax(a->y, b->y), c->y);
        if (tx1 - tx0 > 1023 || ty1 - ty0 > 511) continue;
        idx[nout++] = tri[k][0], idx[nout++] = tri[k][1], idx[nout++] = tri[k][2];
        x0 = imin(x0, tx0); x1 = imax(x1, tx1);
        y0 = imin(y0, ty0); y1 = imax(y1, ty1);
    }
    if (!nout) return;
    /* (emit's: the drawing area's scissor, the bounds clipped to it) */
    u16 sc[4];
    if (!area_scissor(sc)) return;
    int bx0 = imax(x0, sc[0]), by0 = imax(y0, sc[1]), bx1 = imin(x1 + 1, sc[2]), by1 = imin(y1 + 1, sc[3]);
    if (bx0 >= bx1 || by0 >= by1) return;
    gpu_draws(bx0, by0, bx1, by1);
    cur_target = 0;
    cur_mask = (gp0_state.set_mask ? MASK_SET : 0) | (gp0_state.check_mask ? MASK_CHECK : 0) |
               ((gp0_state.texpage >> 9 & 1) ? MASK_DITHER : 0);
    ctr_render_stats[1]++;
    int passes[2], np = 0, abr = m->semi ? m->abr : NO_BLEND;
    if (!m->textured) passes[np++] = K_UNTEX;
    else if (!m->semi) passes[np++] = K_TEX;
    else if (tflags & 8) passes[np++] = K_PAGE_OP, passes[np++] = K_PAGE_SEMI;
    else {
        if (tflags & 1) passes[np++] = K_TEX_P1;
        if (tflags & 2) passes[np++] = K_TEX_P2;
    }
    for (int p = 0; p < np; p++) {
        const int pabr = passes[p] == K_TEX_P1 || passes[p] == K_PAGE_OP ? NO_BLEND : abr;
        float d;
        Vtx *o = run_slot(passes[p], pabr, sc, tex, nout, &d);
        for (unsigned k = 0; k < nout; k++) o[k] = t[idx[k]], o[k].d = d;
    }
}

/* A polygon (GP0 20-3F) from its packet words (gp0.h poly1), in r_poly_plain's case: what r_poly and r_poly_plain do
 * with it, without the decoded mode and vertices between, each kind its own code (the world map's ground is ~3300
 * gouraud triangles a frame; their decode was some 2 us each on an Old 3DS). 0: not that case, decoded as before */
/* what poly_k reads of the drawing state, the same for a run of polygons (no state command between them) */
typedef struct { u16 sc[4]; int sc_ok, ox, oy, mask; float uvh; } TriCtx;
static inline void tri_ctx(TriCtx *c) {
    c->sc_ok = area_scissor(c->sc);
    c->ox = gp0_state.ofs_x, c->oy = gp0_state.ofs_y;
    c->mask = (gp0_state.set_mask ? MASK_SET : 0) | (gp0_state.check_mask ? MASK_CHECK : 0);
    c->uvh = world_ground ? 0.5f : 0;
}
static inline __attribute__((always_inline)) void poly_k(const unsigned *w, const unsigned kind, const TriCtx *c);
static inline int poly_plain(void) {
    return !(ui_mode || stereo_on || ctr_outline || !mirror_depth || menu_state == 2 ||
             (menu_state != 3 && in_window_packets(psx_gpu_packet)));
}
static int r_poly1(const unsigned *w) {
#ifdef PORT_PROBE_AT
    (void)w;
    return 0;
#else
    if (!poly_plain()) return 0;
    TriCtx c;
    tri_ctx(&c);
    switch ((w[0] >> 26) & 7) { /* (textured, quad, gouraud bits: the vertices' words at constant places) */
    case 0: poly_k(w, 0, &c); break;
    case 1: poly_k(w, 1, &c); break;
    case 2: poly_k(w, 2, &c); break;
    case 3: poly_k(w, 3, &c); break;
    case 4: poly_k(w, 4, &c); break;
    case 5: poly_k(w, 5, &c); break;
    case 6: poly_k(w, 6, &c); break;
    default: poly_k(w, 7, &c); break;
    }
    return 1;
#endif
}
static inline __attribute__((always_inline)) void poly_k(const unsigned *w, const unsigned kind, const TriCtx *c) {
    const int textured = kind & 1, nv = (kind & 2) ? 4 : 3, gouraud = (kind >> 2) & 1;
    const unsigned cmd = w[0] >> 24;
    /* (the words of vertex i: colour, position, texture coordinates) */
#define PK_XY(i) (1 + (i) * (1 + textured) + (gouraud ? (i) : 0))
#define PK_COL(i) ((i) == 0 || !gouraud ? 0 : PK_XY(i) - 1)
#define PK_UV(i) (PK_XY(i) + 1)
    const unsigned tp = textured ? w[PK_UV(1)] >> 16 : gp0_state.texpage;
    Gp0Mode m;
    m.textured = textured, m.raw = cmd & 1, m.semi = (cmd >> 1) & 1, m.gouraud = gouraud;
    m.tx = (tp & 15) * 64, m.ty = ((tp >> 4) & 1) * 256, m.abr = (tp >> 5) & 3, m.depth = (tp >> 7) & 3;
    if (m.depth == 3) m.depth = 2;
    m.clut_x = textured ? ((w[PK_UV(0)] >> 16) & 0x3F) * 16 : 0, m.clut_y = textured ? (w[PK_UV(0)] >> 22) & 0x1FF : 0;
    if (textured) gp0_state.texpage = (gp0_state.texpage & ~0x9FFu) | (tp & 0x9FF);
    const int ox = c->ox, oy = c->oy;
    int x[4], y[4], u[4] = {0, 0, 0, 0}, v[4] = {0, 0, 0, 0};
#pragma GCC unroll 4
    for (int i = 0; i < nv; i++) {
        const unsigned xy = w[PK_XY(i)];
        x[i] = (int)(xy & 0x7FF) - (int)((xy & 0x400) << 1) + ox;
        y[i] = (int)((xy >> 16) & 0x7FF) - (int)((xy >> 15) & 0x800) + oy;
    }
    unsigned tflags = 0;
    C3D_Tex *tex = NULL;
    if (textured) {
#pragma GCC unroll 4
        for (int i = 0; i < nv; i++) u[i] = w[PK_UV(i)] & 0xFF, v[i] = (w[PK_UV(i)] >> 8) & 0xFF;
        int u0 = imin(imin(u[0], u[1]), u[2]), u1 = imax(imax(u[0], u[1]), u[2]);
        int v0 = imin(imin(v[0], v[1]), v[2]), v1 = imax(imax(v[0], v[1]), v[2]);
        if (nv == 4) u0 = imin(u0, u[3]), u1 = imax(u1, u[3]), v0 = imin(v0, v[3]), v1 = imax(v1, v[3]);
        tex = texture_for(&m, u0, v0, u1, v1, &tflags);
    }
    ctr_depth_stats[1] += nv;
    /* (the PS1's triangles (0,1,2) and (1,3,2), each skipped 1024 or more wide or 512 or more high: r_poly) */
    int x0 = imin(imin(x[0], x[1]), x[2]), x1 = imax(imax(x[0], x[1]), x[2]);
    int y0 = imin(imin(y[0], y[1]), y[2]), y1 = imax(imax(y[0], y[1]), y[2]);
    int ta = x1 - x0 <= 1023 && y1 - y0 <= 511, tb = 0;
    if (nv == 4) {
        const int qx0 = imin(imin(x[1], x[3]), x[2]), qx1 = imax(imax(x[1], x[3]), x[2]);
        const int qy0 = imin(imin(y[1], y[3]), y[2]), qy1 = imax(imax(y[1], y[3]), y[2]);
        tb = qx1 - qx0 <= 1023 && qy1 - qy0 <= 511;
        if (!ta) x0 = qx0, x1 = qx1, y0 = qy0, y1 = qy1;
        else if (tb) x0 = imin(x0, qx0), x1 = imax(x1, qx1), y0 = imin(y0, qy0), y1 = imax(y1, qy1);
    }
    if (!ta && !tb) return;
    const u16 *sc = c->sc;
    if (!c->sc_ok) return;
    int bx0 = imax(x0, sc[0]), by0 = imax(y0, sc[1]), bx1 = imin(x1 + 1, sc[2]), by1 = imin(y1 + 1, sc[3]);
    if (bx0 >= bx1 || by0 >= by1) return;
    gpu_draws(bx0, by0, bx1, by1);
    cur_target = 0;
    cur_mask = c->mask | ((gp0_state.texpage >> 9 & 1) ? MASK_DITHER : 0);
    ctr_render_stats[1]++;
    int passes[2], np = 0, abr = m.semi ? m.abr : NO_BLEND;
    if (!textured) passes[np++] = K_UNTEX;
    else if (!m.semi) passes[np++] = K_TEX;
    else if (tflags & 8) passes[np++] = K_PAGE_OP, passes[np++] = K_PAGE_SEMI;
    else {
        if (tflags & 1) passes[np++] = K_TEX_P1;
        if (tflags & 2) passes[np++] = K_TEX_P2;
    }
    /* (each vertex made once and written where the run keeps it: r_poly_plain's values, the same sums in the same
     * order) */
    const u32 rawc = textured && m.raw ? 0xFF808080 : 0;
    const float ou = tex ? (float)tex_ou : 0, ov = tex ? (float)tex_ov : 0;
    const float uvh = c->uvh;
    if (nv == 3) { /* (a triangle's vertices straight into the run, once a pass) */
        for (int p = 0; p < np; p++) {
            const int pabr = passes[p] == K_TEX_P1 || passes[p] == K_PAGE_OP ? NO_BLEND : abr;
            float d;
            Vtx *o = run_slot(passes[p], pabr, sc, tex, 3, &d);
#pragma GCC unroll 3
            for (int i = 0; i < 3; i++) {
                o[i].x = (float)x[i] + 5.0f / 16;
                o[i].y = (float)y[i] + 7.0f / 16;
                o[i].z = 0;
                float tu = (float)u[i] + uvh, tv = (float)v[i] + uvh;
                if (tex) tu += ou, tv += ov;
                o[i].u = tu, o[i].v = tv;
                o[i].rgba = rawc ? rawc : 0xFF000000u | (w[PK_COL(i)] & 0xFFFFFF);
                o[i].d = d;
            }
        }
        return;
    }
    Vtx t[4];
#pragma GCC unroll 4
    for (int i = 0; i < nv; i++) {
        t[i].x = (float)x[i] + 5.0f / 16;
        t[i].y = (float)y[i] + 7.0f / 16;
        t[i].z = 0;
        float tu = (float)u[i] + uvh, tv = (float)v[i] + uvh;
        if (tex) tu += ou, tv += ov;
        t[i].u = tu, t[i].v = tv;
        t[i].rgba = rawc ? rawc : 0xFF000000u | (w[PK_COL(i)] & 0xFFFFFF);
    }
    const unsigned nout = (ta ? 3 : 0) + (tb ? 3 : 0);
    for (int p = 0; p < np; p++) {
        const int pabr = passes[p] == K_TEX_P1 || passes[p] == K_PAGE_OP ? NO_BLEND : abr;
        float d;
        Vtx *o = run_slot(passes[p], pabr, sc, tex, nout, &d);
        if (ta) {
            o[0] = t[0], o[0].d = d;
            o[1] = t[1], o[1].d = d;
            o[2] = t[2], o[2].d = d;
            o += 3;
        }
        if (tb) {
            o[0] = t[1], o[0].d = d;
            o[1] = t[3], o[1].d = d;
            o[2] = t[2], o[2].d = d;
        }
    }
#undef PK_XY
#undef PK_COL
#undef PK_UV
}

/* A chain of the world map's ground packets (world_faces.c: G3s and GT3s, the walk's order from head, count of them):
 * r_poly1's case decided once for them all, each drawn by poly_k; one in a window's packets (never, in practice) by
 * the decoder as the walk would. 0: not taken (they are drawn one by one) */
void DrawPrim(void *p); /* (gpu.c: a packet decoded and drawn as the walk would) */
int port_terrain_chain(u32 *head, u32 count) {
    psx_gpu_sync();
#ifdef PORT_PROBE_AT
    (void)head, (void)count;
    return 0;
#else
    if (ui_mode || stereo_on || ctr_outline || !mirror_depth || menu_state == 2) return 0;
    u32 *p = head;
    unsigned n = 0;
    TriCtx c;
    tri_ctx(&c);
    for (u32 i = 0; i < count; i++) {
        const u32 cmd = p[1] >> 24;
        if ((cmd & 0xF8) != 0x30 || (p[0] >> 24) != ((cmd & 4) ? 9u : 6u) ||
            (menu_state != 3 && in_window_packets(p)))
            DrawPrim(p), tri_ctx(&c); /* (as the walk would: the state as it leaves it) */
        else {
            if (cmd & 4) poly_k((const unsigned *)p + 1, 5, &c);
            else poly_k((const unsigned *)p + 1, 4, &c);
            n++;
        }
        p = (u32 *)(((uintptr_t)psx_ram & 0xFF000000) | (p[0] & 0xFFFFFF));
    }
    gp0_note_polys(n);
    return 1;
#endif
}

static void r_poly(const Gp0Mode *m, const Gp0Vtx *v, int n) {
    if (battle_list_replaced()) return;
    /* (emit's case for a primitive into the mirror: not in a window's packets (in_window_packets: ui_packet's test
     * without its mark, which emit leaves as it was), no menu drawn into the frame) */
#ifndef PORT_PROBE_AT /* (its log is r_poly's) */
    if (!ui_mode && !stereo_on && !ctr_outline && mirror_depth && menu_state != 2 &&
        (menu_state == 3 || !in_window_packets(psx_gpu_packet))) {
        r_poly_plain(m, v, n);
        return;
    }
#endif
    unsigned tflags = 0;
    C3D_Tex *tex = prim_texture(m, v, n, &tflags);
    Vtx t[6];
    int with_depth = 0;
    for (int i = 0; i < n; i++) make_vtx(&t[i], m, &v[i]), with_depth += t[i].z > 0;
    /* The PS1 covers pixel (x, y) when the point (x, y) is inside, its top and left edges included; the 3DS's GPU
     * tests the pixel's centre (x + 0.5, y + 0.5). Moved by (5/16, 7/16) (whole steps of the GPU's 1/16 pixel),
     * the polygon's pixels are the PS1's: the centre is its point a little right and below, so a point on a top or
     * left edge is inside and on a bottom or right edge outside, and none falls on a 45-degree edge. Unmoved, slivers
     * thinner than half a pixel lost the rows the PS1 draws: the world map's far ground, where blocks meet, showed the
     * haze under it as white dashes along the horizon (a 3DS video, the Blue Narciss at sea). A polygon whose corners
     * are whole pixels covers what it did, and its texels are the PS1's more nearly (sampled there, not half a pixel
     * on) */
    for (int i = 0; i < n; i++) t[i].x += 5.0f / 16, t[i].y += 7.0f / 16;
    if (tex) for (int i = 0; i < n; i++) t[i].u += tex_ou, t[i].v += tex_ov; /* (its slot's place in the atlas) */
    /* depth for all its vertices or none: a vertex the GTE clamped (near the camera, or far off screen) has none, and
     * mixing w = 1 with w ~ Z would warp the texture across the polygon */
    if (with_depth && with_depth < n) {
        int bare = 0;
        for (int i = 0; i < n; i++) bare += t[i].z == 0;
        /* (with a corner of no depth: none at all, as before; with only parallax depths beside: the parallax kept) */
        for (int i = 0; i < n; i++) t[i].z = bare ? 0 : -fabsf(t[i].z);
    } else if (with_depth == n && small_poly(v, n)) {
        /* a small polygon (a model's: a large one, as a battle's ground, may span that much) with a corner's depth more
         * than twice or under half its median: another vertex's at the same screen position (found by value), given
         * the median (texture and parallax both). In perspective a 3:1 spread across a small triangle on Brahne's chest
         * (field 1250) pulled the fur's texels beside the strap's across it, a pale line in some frames; its parallax
         * alone opened gaps in one eye */
        float z[4];
        for (int i = 0; i < n; i++) z[i] = t[i].z;
        for (int i = 1; i < n; i++)
            for (int j = i; j > 0 && z[j] < z[j - 1]; j--) { float w = z[j]; z[j] = z[j - 1]; z[j - 1] = w; }
        float med = n == 4 ? (z[1] + z[2]) * 0.5f : z[1];
        for (int i = 0; i < n; i++)
            if (t[i].z > 2 * med || 2 * t[i].z < med) t[i].z = med;
    }
    /* in stereo, the world map's scenery (not its models: the ship, the party; not the bottom screen's UI) grown
     * outward by a pixel and a half, each edge moved out along its normal. Its blocks are projected each on its own and
     * meet at the same pixels, but each corner is then shifted by its own depth's parallax, and on a mountain's face
     * neighbouring blocks' shifts differ: gaps between them, the haze drawn under the ground showing through as pale
     * dots. (In 2D they meet exactly, and are drawn as they are.) Each face drawn once, grown with its texture: with
     * the texels as the PS1 picks them (make_vtx) its grown edges no longer reach a transparent one (an underlay
     * drawn first, opaque faces twice, cost frames on a 3DS in 3D) */
    int grown = 0;
    if (stereo_on && world_ground && !ui_mode && !ui_packet() && !model_packet()) grow_poly(t, n, 1.5f), grown = 2;
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
    emit(m, out, nout, x0 - grown, y0 - grown, x1 + 1 + grown, y1 + 1 + grown, tex, tflags);
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
            gpu_quiet();
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

/* A clear of [x0,x1)x[y0,y1) to black the GPU just drew into the mirror: the shadow's pixels there set as a readback
 * would read them back (black, no mask bit), and its cells current there again: those it covers whole, and those it
 * covers in part that were current before it (their other pixels unchanged). Else each was the GPU's alone, and the
 * movies' frames, loaded over their cleared buffer in slices narrower than a cell, read every cell back from the GPU
 * first: 480 readbacks, each a wait for the GPU, every 128 frames of a movie on an Old 3DS. PORT_CLEAR_CHECK: the
 * GPU's pixels read back and compared */
static int in_window_packets(const void *p);
static void clear_cpu(int x0, int y0, int x1, int y1, const u8 was_ok[VH / CELL][VW / CELL]) {
    for (int y = y0; y < y1; y++) memset(psx_vram + y * VW + x0, 0, (size_t)(x1 - x0) * 2);
    cells_ver++;
    int cx0, cy0, cx1, cy1;
    if (!cell_range(x0, y0, x1, y1, &cx0, &cy0, &cx1, &cy1)) return;
    for (int cy = cy0; cy < cy1; cy++)
        for (int cx = cx0; cx < cx1; cx++) {
            const int whole = cx * CELL >= x0 && (cx + 1) * CELL <= x1 && cy * CELL >= y0 && (cy + 1) * CELL <= y1;
            shadow_ok[cy][cx] = whole || was_ok[cy][cx];
            if (whole) cell_cols[cy][cx] = 0;
        }
#ifdef PORT_CLEAR_CHECK
    gpu_sync();
    const u16 *m = mirror_tex[0].data;
    unsigned bad = 0;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) bad += rgba5551_to_bgr555(m[tiled(x, y, VW)]) != psx_vram[y * VW + x];
    static unsigned checks, failed;
    checks++;
    if (bad) failed++, port_log("clear check: %u of %d pixels differ in %d,%d-%d,%d\n", bad, (x1 - x0) * (y1 - y0), x0, y0, x1, y1);
    if (checks % 100 == 0) port_log("clear check: %u clears, %u with differences\n", checks, failed);
#endif
}

static void r_rect(const Gp0Mode *m, const Gp0Vtx *v, int w, int h) {
    if (w <= 0 || h <= 0) return;
    if (ui_mode && m->textured && ui_reads_cache(m, v->u, v->v, v->u + w - 1, v->v + h - 1)) return;
    if (m->textured && w == 23 && h == 13 && m->tx == 960 && m->ty == 256 && v->u == 227 && v->v == 21)
        note_hand(v->x, v->y); /* the pointing hand: where the game's cursor is (touch.c) */
    if (battle_list_replaced()) return;
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
    /* a sprite whose texels run past its page's edge wraps to its start (the PS1's 8-bit u, v): drawn in pieces, cut
     * where it wraps (its slot is one cell of an atlas: no wrapping texture to lean on) */
    /* (the drawing area cleared to black (a DR_ENV's background, as the movies' 24-bit frames are before their slices
     * are loaded over them): see clear_cpu below) */
    const int cx0 = clampi(gp0_state.area_x0, 0, VW), cy0 = clampi(gp0_state.area_y0, 0, VH);
    const int cx1 = clampi(gp0_state.area_x1 + 1, 0, VW), cy1 = clampi(gp0_state.area_y1 + 1, 0, VH);
    const int clear = !m->textured && !m->semi && !(v->r | v->g | v->b) && !gp0_state.set_mask && !gp0_state.check_mask &&
                      !ui_mode && !(menu_state != 3 && (menu_state == 2 || in_window_packets(psx_gpu_packet))) &&
                      v->x <= cx0 && v->y <= cy0 && v->x + w >= cx1 && v->y + h >= cy1 && cx0 < cx1 && cy0 < cy1 &&
                      (!stereo_on || (cx1 - cx0 >= 300 && cy1 - cy0 >= 200)) && !ctr_outline;
    u8 was_ok[VH / CELL][VW / CELL];
    if (clear) {
        memcpy(was_ok, shadow_ok, sizeof was_ok);
        /* (the cells it covers whole are black after it whatever the mirror had: not brought up to date first (gpu_draws
         * blits a stale cell's shadow into the mirror before drawing over it): each movie frame's slices, loaded into
         * the shadow, were blitted into the mirror only to be cleared there at the next frame) */
        for (int cy = (cy0 + CELL - 1) / CELL; cy < cy1 / CELL; cy++)
            for (int cx = (cx0 + CELL - 1) / CELL; cx < cx1 / CELL; cx++) mirror_ok[cy][cx] = 1, cell_cols[cy][cx] = 0;
    }
    int ou = tex ? tex_ou : 0, ov = tex ? tex_ov : 0;
    for (int py = 0; py < h;) {
        int sv = (v->v + py) & 255, ph = h - py < 256 - sv ? h - py : 256 - sv;
        if (!tex) sv = v->v + py, ph = h - py;
        for (int px = 0; px < w;) {
            int su = (v->u + px) & 255, pw = w - px < 256 - su ? w - px : 256 - su;
            if (!tex) su = v->u + px, pw = w - px;
            Vtx t[6];
            quad(t, v->x + px, v->y + py, v->x + px + pw, v->y + py + ph, ou + su, ov + sv, ou + su + pw, ov + sv + ph,
                 vcolor(m, v));
            emit(m, t, 6, v->x + px, v->y + py, v->x + px + pw, v->y + py + ph, tex, tflags);
            px += pw;
        }
        py += ph;
    }
    if (clear) clear_cpu(cx0, cy0, cx1, cy1, was_ok);
}

/* ---- a field background's run of tiles (port/game/field_bg.c lifts it out of the OT into a marker, which gpu.c's walk
 * reaches where the run was): drawn here at once, in its order, as gp0_tpage_sprite16 and r_rect would draw each tile
 * (the same mode, texture, vertices, batches and depth values), without each tile's packet dispatch and emit's
 * per-primitive work (the scissor, the UI and stereo tests, the cells). 0: not taken, drawn tile by tile. Only the
 * plain case: the scene's mirror in 2D, no outlines, no menu; anything else is the walk's as before. Every other tile
 * the same way through gp0's sprite16 hook (r_sprite16). On an Old 3DS, whose tiles cost some 13 us each (memory
 * without an L2 cache), 1 ms of Lindblum's castle station's VBlank; an emulator showed nothing. */
int gxh_bg_runs_on(void) { return 1; }
void gxh_bg_runs_reset(void) {}
int gxh_bg_units_on(void) { return 0; } /* (units move a layer's tiles to one entry: the order changes) */
int gxh_bg_unit(const unsigned int *head, int count, const unsigned char *sprites, const unsigned int *pos, int z,
                const unsigned int *ot) {
    (void)head; (void)count; (void)sprites; (void)pos; (void)z; (void)ot;
    return 0;
}
static int in_window_packets(const void *p);
/* (the plain case: the scene's mirror in 2D, no outlines, no menu) */
static inline int bg_plain(void) { return !ui_mode && !stereo_on && !ctr_outline && menu_state != 2; }
/* one tile (t: its page word, then the sprite's three) inside the scissor sc, as r_rect and emit would draw it; 0: not
 * this way (its texels wrap past its page's edge) */
static int bg_tile(const unsigned t[4], const u16 sc[4]) {
    if ((t[3] & 0xFF) > 240 || ((t[3] >> 8) & 0xFF) > 240) return 0;
    Gp0Mode m;
    Gp0Vtx v;
    gp0_sprite16_setup(t, &m, &v);
    int bx0 = v.x, by0 = v.y, bx1 = v.x + 16, by1 = v.y + 16;
    if (bx0 < sc[0]) bx0 = sc[0];
    if (by0 < sc[1]) by0 = sc[1];
    if (bx1 > sc[2]) bx1 = sc[2];
    if (by1 > sc[3]) by1 = sc[3];
    if (bx0 >= bx1 || by0 >= by1) return 1; /* (emit's: nothing drawn) */
    const int icon = button_icon(&m, v.u, v.v, 16, 16);
    unsigned tflags = 0;
    C3D_Tex *tex = icon >= 0 ? &btn_tex[icon] : texture_for(&m, v.u, v.v, v.u + 15, v.v + 15, &tflags);
    if (icon >= 0) tflags = 2;
    const float ou = icon < 0 && tex ? tex_ou : 0, ov = icon < 0 && tex ? tex_ov : 0;
    /* (quad's corners: its triangles (a, b, c) and (b, d, c)) */
    const float qx0 = v.x, qy0 = v.y, qx1 = v.x + 16, qy1 = v.y + 16;
    const float qu0 = icon >= 0 ? 0 : ou + v.u, qv0 = icon >= 0 ? 0 : ov + v.v;
    const float qu1 = icon >= 0 ? 16 : ou + v.u + 16, qv1 = icon >= 0 ? 16 : ov + v.v + 16;
    const u32 rgba = vcolor(&m, &v);
    gpu_draws(bx0, by0, bx1, by1);
    cur_target = 0;
    cur_mask = (gp0_state.set_mask ? MASK_SET : 0) | (gp0_state.check_mask ? MASK_CHECK : 0) |
               ((gp0_state.texpage >> 9 & 1) ? MASK_DITHER : 0);
    ctr_render_stats[1]++;
    const int abr = m.semi ? m.abr : NO_BLEND;
    int passes[2], pabr[2], np = 0;
    if (!m.semi) passes[np] = K_TEX, pabr[np++] = abr;
    else if (tflags & 8) passes[np] = K_PAGE_OP, pabr[np++] = NO_BLEND, passes[np] = K_PAGE_SEMI, pabr[np++] = abr;
    else {
        if (tflags & 1) passes[np] = K_TEX_P1, pabr[np++] = NO_BLEND;
        if (tflags & 2) passes[np] = K_TEX_P2, pabr[np++] = abr;
    }
    for (int p = 0; p < np; p++) {
        if (!mirror_depth) { /* (run_add's other case) */
            Vtx q[6];
            quad(q, qx0, qy0, qx1, qy1, qu0, qv0, qu1, qv1, rgba);
            run_add(passes[p], pabr[p], sc, tex, q, 6);
            continue;
        }
        float d;
        Vtx *o = run_slot(passes[p], pabr[p], sc, tex, 6, &d);
        o[0] = (Vtx){qx0, qy0, 0, qu0, qv0, rgba, d}, o[1] = (Vtx){qx1, qy0, 0, qu1, qv0, rgba, d};
        o[2] = (Vtx){qx0, qy1, 0, qu0, qv1, rgba, d};
        o[3] = o[1], o[4] = (Vtx){qx1, qy1, 0, qu1, qv1, rgba, d}, o[5] = o[2];
    }
    return 1;
}
/* gp0_tpage_sprite16's (every texture page + SPRT_16 packet: a field background's tiles), its bookkeeping done */
static int r_sprite16(unsigned int tp, unsigned int cmd, unsigned int xy, unsigned int uvc) {
    u16 sc[4];
    if (!bg_plain() || in_window_packets(psx_gpu_packet) || !area_scissor(sc)) return 0;
    const unsigned t[4] = {tp, cmd, xy, uvc};
    return bg_tile(t, sc);
}
static inline const u32 *bg_tile_next(const u32 *p) { return (const u32 *)(((u32)(uintptr_t)psx_ram & 0xFF000000) | (*p & 0xFFFFFF)); }
int gxh_bg_run(unsigned int *head, int count) {
    psx_gpu_sync();
    u16 sc[4];
    if (!bg_plain() || count <= 0 || !area_scissor(sc)) return 0;
    /* (every tile a page + SPRT_16 packet as the game builds them (MargePrim'd: the sprite's tag a NOP between), none
     * on the font pages gp0_tpage_sprite16 leaves to the command path, none in a window's packets, none wrapping) */
    const u32 *p = (const u32 *)head;
    for (int i = 0; i < count; i++, p = bg_tile_next(p)) {
        const u32 *w = p + 1;
        if ((p[0] >> 24) != 5 || w[0] >> 24 != 0xE1 || w[1] || w[2] >> 26 != 0x7C >> 2) return 0;
        const u32 tp = w[0] & 0xFFFFFF;
        if ((tp & 0x10) && (tp & 15) >= 14) return 0;
        if ((w[4] & 0xFF) > 240 || ((w[4] >> 8) & 0xFF) > 240) return 0;
        if (in_window_packets(p)) return 0;
    }
    gp0_note_sprites(head[1], (unsigned)count); /* (each tile's bookkeeping in gp0_tpage_sprite16, before it draws) */
    p = (const u32 *)head;
    for (int i = 0; i < count; i++, p = bg_tile_next(p)) {
        const u32 *w = p + 1;
        const unsigned t[4] = {w[0], w[2], w[3], w[4]};
        psx_gpu_packet = p;
        bg_tile(t, sc);
    }
    return 1;
}

static void r_line(const Gp0Mode *m, const Gp0Vtx *a, const Gp0Vtx *b) {
    if (battle_list_replaced()) return;
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

/* gpu.c's VRAM-to-VRAM copy (set_mask: each pixel copied with its mask bit set), when its source is the GPU's (drawn since the CPU last read it) and apart from where it
 * goes: copied in the mirror, each eye's from its own, and the destination left the GPU's (read back if the CPU reads
 * it). Else the CPU copies it in the shadow, as before (a readback of the source first) */
static int r_copy(int sx, int sy, int dx, int dy, int w, int h) {
    if (!(sx + w <= dx || dx + w <= sx || sy + h <= dy || dy + h <= sy)) return 0;
    if (!gpu_owned(sx, sy, sx + w, sy + h)) return 0;
    int cx0, cy0, cx1, cy1; /* (the source current in the mirror: cells the CPU wrote since) */
    if (cell_range(sx, sy, sx + w, sy + h, &cx0, &cy0, &cx1, &cy1))
        for (int cy = cy0; cy < cy1; cy++)
            for (int cx = cx0; cx < cx1; cx++)
                if (!mirror_ok[cy][cx]) { blit_cells(cx0, cy0, cx1, cy1); cy = cy1; break; }
    gpu_draws(dx, dy, dx + w, dy + h);
    int saved = cur_target, saved_mask = cur_mask;
    cur_target = 0;
    cur_mask = gp0_state.set_mask ? MASK_SET : MASK_COPY; /* (the pixels with their mask bits, or all with it set) */
    for (int eye = 0; eye < 2; eye++) {
        Vtx *v = alloc_vtx(6);
        quad(v, dx, dy, dx + w, dy + h, sx, sy, sx + w, sy + h, 0xFFFFFFFF);
        add_batch(K_BLIT, NO_BLEND, 1 << eye, full_scissor, &mirror_tex[eye], v - vb, 6);
    }
    cur_target = saved;
    cur_mask = saved_mask;
    ctr_render_stats[2]++;
    return 1;
}
static int r_sprite16(unsigned int tp, unsigned int cmd, unsigned int xy, unsigned int uvc);
static const Gp0Backend pica_backend = {r_poly, r_line, r_rect, 0, 0, r_sprite16, r_copy, r_poly1};

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
int ctr_ui_active(void) { psx_gpu_sync(); return shown_layer >= 0 || (menu_state == 3 && scene_nhands > 0); }
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
    /* (cleared when something is drawn into it next (layer_clear): a list that draws nothing there, as the battle's own,
     * all of it the battle screen's in its place, leaves it as it was, and the battle screen's own drawing, unchanged,
     * needn't be drawn again: field_hud.c) */
    UiLayer *L = &layers[k];
    if (clear || L->shown) {
        L->clear_pending = 1;
        layers[k].shown = 0;
        layers[k].nhands = 0;
    }
    return k;
}
/* a layer's clear, before the first thing drawn into it since it was asked (ui_layer_for): only where something was drawn
 * since the last one (the battle's list cleared its layer every VBlank, and the whole 512x256 was 131k pixels each time,
 * a third of the GPU's work on a battle's VBlank on an Old 3DS) */
static void layer_clear(int k) {
    UiLayer *L = &layers[k];
    L->clear_pending = 0;
    if (L->dx0 >= L->dx1) return;
    const u16 sc[4] = {(u16)L->dx0, (u16)L->dy0, (u16)L->dx1, (u16)L->dy1};
    int saved = cur_target;
    cur_target = k + 1;
    Vtx *v = alloc_vtx(6);
    quad(v, L->dx0, L->dy0, L->dx1, L->dy1, 0, 0, 0, 0, 0x00000000);
    add_batch(K_UNTEX, NO_BLEND, 1, sc, NULL, v - vb, 6);
    cur_target = saved;
    L->dx0 = L->dx1 = 0;
    L->clears++;
}
static int displayed_layer(void) {
    PsxDisplayInfo d;
    psx_gpu_display_info(&d);
    return layers[0].x == d.x && layers[0].y == d.y ? 0 : layers[1].x == d.x && layers[1].y == d.y ? 1 : -1;
}
/* the displayed buffer's layer, and a count changing whenever it is cleared or drawn into (field_hud.c: the same as
 * after its drawing, the layer holds that and nothing else) */
unsigned ctr_ui_layer_serial(void) {
    psx_gpu_sync();
    int k = displayed_layer();
    return k < 0 ? ~0u : (unsigned)k | (layers[k].clears + layers[k].draws) << 1;
}
/* the displayed buffer's layer kept as drawn this frame, nothing drawn into it (field_hud.c) */
void ctr_ui_layer_keep(void) {
    psx_gpu_sync();
    int k = displayed_layer();
    if (k >= 0) layers[k].drawn = frames_drawn;
}

/* Window-system packets (port/game/ui_split.c): a range lives until an ordering table (any: the battle draws its
 * windows in the VBlank list) has drawn from it, or two presents have gone by, as the game's packet buffers are
 * reused and a stale range would take the scene's packets there (a boss's ceiling went to the bottom screen). */
static struct { const u8 *lo, *hi; int hit, age; } ranges[8];
static int nranges;
static unsigned ranges_ver; /* (counts changes to them: in_window_packets' interval) */
int port_game_mode(void);
void port_bottom_packets(const void *lo, const void *hi);
/* packets [lo, hi) of this frame are drawn on the bottom screen scaled by factor, (cx, cy) of them in the middle of
 * the screen (the world map's zoom: port/game/world_navi.c); factor 1: as they are */
void port_bottom_zoom(const void *lo, const void *hi, int cx, int cy, int factor) {
    if (psx_gpu_stage(STG_ZOOM, (const int[]){(int)lo, (int)hi, cx, cy, factor}, 5, NULL, 0)) return;
    zoom_lo = lo;
    zoom_hi = hi;
    zoom_cx = (float)cx;
    zoom_cy = (float)cy;
    zoom_factor = factor;
    zoom_frame = frames_drawn;
}
static float top_scale(int w, int h);
/* ---- cel-shading outlines (New 3DS option: ctr_outline) ----
 * Model polygons (the packet ranges the model drawers report: port_model_packets) are drawn a second time, white,
 * into a mask the size of the mirror (per eye, with the same parallax; in linear memory: VRAM is full). At the
 * present, per eye, the mask's displayed part spread a pixel each way less the mask itself is the outline, and the
 * top screen is darkened by it: a dark line round each model's silhouette, over the scene. */
int ctr_outline; /* (off: shelved, 2026-10-31; the mask matches the models, the edge pass is unfinished) */
static C3D_Tex outline_tex[2][2], outline_edge_tex[2]; /* the masks per PS1 framebuffer (y 0 / 256) and eye */
static C3D_RenderTarget *outline_edge_rt[2];
#define MAX_MODEL_RANGES 96
static struct { const u8 *lo, *hi; unsigned frame; float zsum, zlast; int zn; } models[MAX_MODEL_RANGES];
static int nmodels;
void port_model_packets(const void *lo, const void *hi) {
    if (psx_gpu_stage(STG_MODEL, (const int[]){(int)lo, (int)hi}, 2, NULL, 0)) return;
    float zlast = 0; /* (the range's depth last frame, carried over when the same model writes there again) */
    for (int i = 0; i < nmodels; i++)
        if (((const u8 *)lo < models[i].hi && (const u8 *)hi > models[i].lo) || frames_drawn - models[i].frame > 2) {
            if (models[i].lo == (const u8 *)lo)
                zlast = models[i].zn ? models[i].zsum / models[i].zn : models[i].zlast;
            models[i] = models[--nmodels], i--;
        }
    if (nmodels == MAX_MODEL_RANGES) nmodels--;
    models[nmodels].lo = lo, models[nmodels].hi = hi, models[nmodels].frame = frames_drawn;
    models[nmodels].zsum = 0, models[nmodels].zn = 0, models[nmodels].zlast = zlast;
    nmodels++;
}
static int model_index(void) { /* (the model range the packet is in, + 1; 0 if none) */
    /* (the ranges don't overlap (port_model_packets drops any a new one overlaps), so the one holding the packet is the
     * only one: the last found, which a model's run of polygons keeps finding, is tried first) */
    static int last;
    const u8 *p = (const u8 *)psx_gpu_packet;
    if (last < nmodels && p >= models[last].lo && p < models[last].hi) return last + 1;
    for (int i = 0; i < nmodels; i++)
        if (p >= models[i].lo && p < models[i].hi) return (last = i) + 1;
    return 0;
}
static int model_packet(void) { return model_index() != 0; }
/* A model's meshes are projected one by one (a head, the body under it), so where two meet, the same point on the
 * screen can have slightly different depths: in stereo each side shifted by its own parallax, a hairline crack down
 * the seam (the back of Zidane's head). A corner on a pixel another corner of the same model had this frame, at
 * nearly its depth (within 1/20: not a limb in front of the body), takes that depth. */
#define MWELD_N 2048
static struct { unsigned frame; short x, y, m; float z; } mweld[MWELD_N];
static const Vtx *model_weld(const Vtx *v, unsigned n, Vtx *out, int m) {
    /* a corner without a depth of its own (one the GTE didn't store; in a field with a flat background a polygon
     * without any stays at the screen plane) takes its polygon's, else its model's: parts of a model left unshifted
     * beside shifted ones came apart in stereo (a far NPC behind a near Steiner, Dali's barrels) */
    float zs = 0, mz;
    int zc = 0;
    for (unsigned i = 0; i < n; i++)
        if (v[i].z != 0) zs += fabsf(v[i].z), zc++; /* (negative: for the parallax alone, a depth all the same) */
    if (zc) models[m - 1].zsum += zs / zc, models[m - 1].zn++;
    mz = zc ? zs / zc : models[m - 1].zn ? models[m - 1].zsum / models[m - 1].zn : models[m - 1].zlast;
    float base = models[m - 1].zlast > 0 ? models[m - 1].zlast : models[m - 1].zn ? models[m - 1].zsum / models[m - 1].zn : mz;
    if (world_ground && base > 0) {
        /* (on the world map, one depth for the whole model: its mean the frame before, in steps of 2%. Its corners'
         * depths are found by their screen position, and as it animates a corner could take another's (the ground's
         * behind it): each corner's parallax jumped from frame to frame, the model's outline flickering in stereo (a
         * 3DS video, the gold chocobo and Zidane). A figure that far off has no depth of its own to see) */
        float q = expf(roundf(logf(base) * 50.0f) / 50.0f);
        for (unsigned i = 0; i < n; i++) out[i] = v[i], out[i].z = -q;
        return out;
    }
    for (unsigned i = 0; i < n; i++) {
        out[i] = v[i];
        if (out[i].z == 0 && mz > 0) out[i].z = mz;
        float z = fabsf(out[i].z), sign = out[i].z < 0 ? -1.0f : 1.0f;
        if (z <= 0) continue;
        int px = floor_int(v[i].x + 0.5f), py = floor_int(v[i].y + 0.5f);
        unsigned h = ((unsigned)px * 73856093u ^ (unsigned)py * 19349663u ^ (unsigned)m * 83492791u) & (MWELD_N - 1);
        for (unsigned k = 0; k < 8; k++, h = (h + 1) & (MWELD_N - 1)) {
            if (mweld[h].frame != frames_drawn + 1) {
                mweld[h].frame = frames_drawn + 1, mweld[h].x = (short)px, mweld[h].y = (short)py, mweld[h].m = (short)m;
                mweld[h].z = z;
                break;
            }
            if (mweld[h].x == px && mweld[h].y == py && mweld[h].m == m) {
                float d = mweld[h].z - z;
                if (d < 0) d = -d;
                if (d * 20 <= z) out[i].z = sign * mweld[h].z;
                break;
            }
        }
    }
    return out;
}
static int outline_init(void) {
    static int failed;
    if (outline_rt[0][0]) return 1;
    if (failed) return 0;
    /* (VRAM: render targets must be; half size: a framebuffer's 512x256 area each) */
    for (int k = 0; k < 6; k++) {
        C3D_Tex *t = k < 4 ? &outline_tex[k >> 1][k & 1] : &outline_edge_tex[k - 4];
        C3D_RenderTarget **rt = k < 4 ? &outline_rt[k >> 1][k & 1] : &outline_edge_rt[k - 4];
        if (!C3D_TexInitVRAM(t, 256, 128, GPU_RGBA4) || !(*rt = C3D_RenderTargetCreateFromTex(t, GPU_TEXFACE_2D, 0, -1))) {
            port_log("outline: no VRAM for its masks: off\n");
            outline_rt[0][0] = NULL;
            failed = 1;
            return 0;
        }
        C3D_RenderTargetClear(*rt, C3D_CLEAR_ALL, 0, 0);
    }
    port_log("outline: on\n");
    return 1;
}
/* the mask pass's projection: the 512x256 VRAM area from the framebuffer's origin (x0, y0) onto the half-size mask */
static void outline_proj(int x0, int y0) {
    C3D_Mtx m;
    Mtx_Zeros(&m);
    m.r[0].x = 2.0f / 512; m.r[0].w = -1.0f - 2.0f * x0 / 512;
    m.r[1].y = -2.0f / 256; m.r[1].w = 1.0f + 2.0f * y0 / 256;
    m.r[2].z = 1.0f; m.r[2].w = 0;
    m.r[3].w = 1.0f;
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, u_proj, &m);
}
/* the outline of the displayed part (dx, dy, w, h of the mirror) onto the top screen's eye, placed as draw_to_top */
static void outline_draw(int eye, int dx, int dy, int w, int h, int rows) {
    if (!outline_rt[0][0]) return;
    int e = stereo_on ? eye : 0, buf = outline_buf(dx, dy);
    /* the edge: the mask spread a texel each way (added), less the mask (subtracted); all in the half-size mask */
    int mx = 0, my = 0, mw = (w + 1) / 2, mh = (h + 1) / 2; /* (the mask holds the buffer from its origin) */
    C3D_RenderTargetClear(outline_edge_rt[e], C3D_CLEAR_ALL, 0, 0);
    C3D_FrameDrawOn(outline_edge_rt[e]);
    set_proj_ortho(256, 128, 0.0f);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, u_stereo, 0, 0, 1, 0.5f);
    set_tev(K_BLIT);
    tex_bind(0, &outline_tex[buf][e]);
    set_texscale(&outline_tex[buf][e]);
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    static const s8 off[5][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}, {0, 0}};
    for (int i = 0; i < 5; i++) {
        if (i < 4) C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ONE, GPU_ONE, GPU_ONE);
        else C3D_AlphaBlend(GPU_BLEND_REVERSE_SUBTRACT, GPU_BLEND_REVERSE_SUBTRACT, GPU_ONE, GPU_ONE, GPU_ONE, GPU_ONE);
        Vtx *v = alloc_vtx(6);
        quad(v, mx + off[i][0], my + off[i][1], mx + mw + off[i][0], my + mh + off[i][1], mx, my, mx + mw, my + mh, 0xFFFFFFFF);
        draw_tris(v - vb, 6);
    }
    /* the top screen darkened where the edge is: dst x (1 - edge) */
    float sc = top_scale(w, h);
    int dw = (int)(w * sc + 0.5f), dh = (int)(rows * sc + 0.5f); /* (rows: the scene's, as draw_to_top shows it) */
    float mrows = (float)mh * rows / h;
    C3D_FrameDrawOn(top_rt[eye]);
    C3D_Mtx pm;
    Mtx_OrthoTilt(&pm, 0.0f, 400.0f, 240.0f, 0.0f, 0.0f, 1.0f, true);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, u_proj, &pm);
    tex_bind(0, &outline_edge_tex[e]);
    set_texscale(&outline_edge_tex[e]);
    C3D_TexSetFilter(&outline_edge_tex[e], GPU_LINEAR, GPU_LINEAR);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ZERO, GPU_ONE_MINUS_SRC_COLOR, GPU_ZERO, GPU_ONE);
    Vtx *v = alloc_vtx(6);
    quad(v, (400 - dw) / 2, (240 - dh) / 2, (400 - dw) / 2 + dw, (240 - dh) / 2 + dh, mx, my, mx + mw, my + mrows, 0xFFFFFFFF);
    draw_tris(v - vb, 6);
    set_blend(NO_BLEND, 0);
}

/* the windows' packets that stay on the top screen: drawn at the screen plane in stereo (box and text together) */
static struct { const u8 *lo, *hi; unsigned frame; } flat[8];
static int nflat;
static void ui_packets(const void *lo, const void *hi, int mode);
void port_ui_packets(const void *lo, const void *hi) {
    const int mode = port_game_mode(); /* (the game's as the call is made, whenever a kept one is: psx_gpu_stage) */
    if (psx_gpu_stage(STG_UI, (const int[]){(int)lo, (int)hi, mode}, 3, NULL, 0)) return;
    ui_packets(lo, hi, mode);
}
static void ui_packets(const void *lo, const void *hi, int mode) {
    /* In a field or on the world map, windows are speech bubbles and prompts anchored to the scene: they stay on the
     * top screen with the speaker (docs/ui-design.md), at the screen plane. Menus go to the bottom whatever the mode. */
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
    if (psx_gpu_stage(STG_BOTTOM, (const int[]){(int)lo, (int)hi}, 2, NULL, 0)) return;
    for (int i = 0; i < nranges; i++) /* (the buffer was reused: a range there is a past frame's) */
        if ((const u8 *)lo < ranges[i].hi && (const u8 *)hi > ranges[i].lo) ranges[i] = ranges[--nranges], i--;
    if (nranges == 8) nranges--;
    ranges[nranges].lo = lo;
    ranges[nranges].hi = hi;
    ranges[nranges].hit = ranges[nranges].age = 0;
    nranges++;
    ranges_ver++;
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
/* (ui_packet's test, without marking the range drawn from. The last packet outside them leaves the addresses between the
 * ranges around it, which the next packets, from the same buffer, mostly fall in: each primitive tested every range,
 * some 0.8 us of the world map's ~3300 a frame on an Old 3DS) */
static int in_window_packets(const void *pv) {
    static const u8 *clo, *chi;
    static unsigned cver = ~0u;
    const u8 *p = (const u8 *)pv;
    if (p >= clo && p < chi && cver == ranges_ver) return 0;
    const u8 *lo = NULL, *hi = (const u8 *)~(uintptr_t)0;
    for (int i = 0; i < nranges; i++) {
        if (p >= ranges[i].lo && p < ranges[i].hi) return 1;
        if (ranges[i].hi <= p && ranges[i].hi > lo) lo = ranges[i].hi;
        if (ranges[i].lo > p && ranges[i].lo < hi) hi = ranges[i].lo;
    }
    clo = lo, chi = hi, cver = ranges_ver;
    return 0;
}
static void ranges_drawn(void) { /* (an ordering table is done: what it drew from is spent) */
    for (int i = 0; i < nranges; i++)
        if (ranges[i].hit) ranges[i] = ranges[--nranges], i--;
    ranges_ver++;
}
static void ranges_age(void) {
    for (int i = 0; i < nranges; i++)
        if (++ranges[i].age > 2) ranges[i] = ranges[--nranges], i--;
    ranges_ver++;
}

/* gpu.c: an ordering table drawn into a framebuffer starts that frame's UI layer afresh, once it has been shown */
void psx_gpu_ot_begin(const u32 *ot) {
    psx_gpu_sync();
    (void)ot;
    if (ui_mode || ctr_keep_layers) return;
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
    psx_gpu_sync();
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
/* (nothing submitted at its start or end: its batches are its layer's, in order there, and the layers are drawn after
 * the mirror in a submit. Submitted, the list, drawn from the VBlank often just after a present, waited for the GPU to
 * finish the frame before: on an Old 3DS some 7 ms of every battle frame) */
void psx_ui_list_begin(void) {
    psx_gpu_sync();
    PsxDisplayInfo d;
    psx_gpu_display_info(&d);
    ui_mode = 1;
    ui_ox = d.x;
    ui_oy = d.y;
    n_ui_cache = 0;
    ui_list_layer = ui_layer_for(d.x, d.y, 1);
}

/* the same for the framebuffer at (x, y), shown or not (the rewards under a battle: drawn into both buffers' layers,
 * as the battle's VBlank flip may come between the drawing and the present) */
void psx_ui_list_begin_at(int x, int y) {
    psx_gpu_sync();
    ui_mode = 1;
    ui_ox = x;
    ui_oy = y;
    n_ui_cache = 0;
    ui_list_layer = ui_layer_for(x, y, 1);
}

/* the same, drawing over what the layer has this frame (the battle screen's own drawing after the battle list) */
void psx_ui_list_begin_keep(void) {
    psx_gpu_sync();
    PsxDisplayInfo d;
    psx_gpu_display_info(&d);
    ui_mode = 1;
    ui_ox = d.x;
    ui_oy = d.y;
    ui_list_layer = ui_layer_for(d.x, d.y, 0);
}

void psx_ui_list_end(void) {
    psx_gpu_sync();
    ui_mode = 0;
}

/* (nothing submitted here: what an ordering table drew is drawn with the rest at the present, or before whatever
 * needs it (a readback, a texture rewritten). Submitted at each table's end, the next frame's first table, a few ms
 * after the present, waited for the GPU to finish the frame before: on an Old 3DS, whose GPU takes some 10 ms after a
 * battle's present, 7-8 ms of every frame) */
void psx_gpu_ot_done(void) {
    psx_gpu_sync();
    ranges_drawn();
}

/* gpu.c: whether the scene's walk may go to the other core now. An Old 3DS's (a New 3DS has the time, and its 3D reads
 * the GTE's depths as the game writes the next frame's), in a field, a battle or on the world map with no menu up and
 * no UI layer kept: the frames this was measured and compared on. (A battle's VBlank list, its UI, is drawn from the
 * game's VSync, after its DrawSync: the walk is over by then.) */
int ctr_defer = 1;
int psx_gpu_defer_ok(void) {
    if (!ctr_defer || ctr_is_new3ds() || stereo_on || ui_mode || menu_state || ctr_keep_layers || ctr_outline) return 0;
    const int mode = port_game_mode();
    return mode == 1 || mode == 2 || mode == 3; /* (field, battle, world map) */
}
/* gpu.c: the drawing goes to the other thread. What this one wrote for the GPU since the last flush (vertices, staging,
 * texture blocks) is written out of this core's data cache here, by this core: a flush asked later by the other
 * thread, on the other core, need not reach lines this core still holds, and the GPU reads memory. (An Old 3DS's
 * water on the world map, its blocks converted on the drawing thread every frame: black triangles and streaks in
 * it, which Azahar, with no caches, never drew.) */
void psx_gpu_handoff(void) { flush_pending(); }
/* a call kept while the walk was on the other core (psx_gpu_stage), made now */
void psx_gpu_staged(int kind, const int *a) {
    const void *lo = (const void *)a[0], *hi = (const void *)a[1];
    switch (kind) {
    case STG_TERRAIN: port_terrain_packets(lo, hi); break;
    case STG_SHIFT: port_bottom_shift(lo, hi, a[2], a[3]); break;
    case STG_ZOOM: port_bottom_zoom(lo, hi, a[2], a[3], a[4]); break;
    case STG_MODEL: port_model_packets(lo, hi); break;
    case STG_UI: ui_packets(lo, hi, a[2]); break;
    case STG_BOTTOM: port_bottom_packets(lo, hi); break;
    }
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
    tex_bind(0, src);
    set_texscale(src);
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    Vtx *v = alloc_vtx(6);
    quad(v, ox, oy, ox + ow, oy + oh, sx, sy, sx + w, sy + h, rgba);
    draw_tris(v - vb, 6);
}

/* The top screen shows the frame as large as it fits whole: scaled to the screen's 240 lines (a 320x224 field or
 * battle becomes 343x240), the same in both directions, centred; filtered, as the scale isn't whole. */
static float top_scale(int w, int h) {
    int port_setting_get(int k);
    if (port_setting_get(1)) return 1.0f; /* (the "sharp" option: the PS1's pixels 1:1) */
    float s = 240.0f / (float)h;
    if (w * s > 400.0f) s = 400.0f / (float)w;
    return s < 1.0f ? 1.0f : s;
}
static void draw_to_top(C3D_RenderTarget *rt, C3D_Tex *src, int sx, int sy, int w, int h, int fw, int fh, int edge) {
    float s = top_scale(fw, fh); /* (the frame's scale; h lines of it shown, in the middle: the ending's vignettes) */
    int dw = (int)(fw * s + 0.5f), dh = (int)(h * s + 0.5f), e = (int)(edge * s + 0.5f);
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
    draw_tris(v - vb, 6);
}


__attribute__((weak)) int port_game_mode(void) { return 0; }
__attribute__((weak)) int port_top_crop(void) { return 0; }
__attribute__((weak)) int port_top_fade(u8 rgb[3]) { (void)rgb; return 0; }
/* what is on a screen's render target (already drawn on), less a colour: the PS1's subtractive fade (port_top_fade) */
static void screen_subtract(C3D_RenderTarget *rt, const u8 rgb[3]) {
    C3D_Mtx p;
    Mtx_OrthoTilt(&p, 0.0f, rt == bottom_rt ? 320.0f : 400.0f, 240.0f, 0.0f, 0.0f, 1.0f, true);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, u_proj, &p);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, u_stereo, 0, 0, 1, 0.5f);
    set_tev(K_UNTEX);
    C3D_AlphaBlend(GPU_BLEND_REVERSE_SUBTRACT, GPU_BLEND_ADD, GPU_ONE, GPU_ONE, GPU_ZERO, GPU_ONE);
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    Vtx *v = alloc_vtx(6);
    quad(v, 0, 0, rt == bottom_rt ? 320 : 400, 240, 0, 0, 0, 0, 0xFF000000u | rgb[2] << 16 | rgb[1] << 8 | rgb[0]);
    draw_tris(v - vb, 6);
    set_blend(NO_BLEND, 0);
}
__attribute__((weak)) int port_fade_busy(void) { return 0; }

/* The bottom screen: the UI layer of the displayed buffer, on a plain backdrop (the scene is on the top screen only). */
int ctr_ui_oy;
static int bottom_layer(const PsxDisplayInfo *d) {
    int k = layers[0].x == d->x && layers[0].y == d->y ? 0 : layers[1].x == d->x && layers[1].y == d->y ? 1 : -1;
    if (menu_state == 3) return -1;
    /* (nothing drawn into it lately; unless its owner keeps it: the rewards under a battle draw at the battle's pace,
     * every 4th present) */
    if (k >= 0 && frames_drawn - layers[k].drawn > 2 && !ctr_keep_layers) k = -1;
    return k;
}
/* ---- the top screen's UI list: the port's own drawing (menus' detail views: port/game/field_hud.c), built with the
 * game's window and text code as the bottom screen's, decoded onto the top screen after its scene each present ---- */
static u32 *top_ui_head;
static const void *top_ui_buf;
static unsigned top_ui_size;
void ctr_top_ui(unsigned *head, const void *buf, unsigned size) {
    psx_gpu_sync();
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
    psx_gpu_sync();
    trace("hud_image", slot, (int)key);
    if (slot < 0 || slot >= HUD_IMAGES || w > 64 || h > 64) return;
    if (hud_img[slot].ok && hud_img[slot].key == key) return;
    if (!hud_img[slot].ok) {
        if (!C3D_TexInit(&hud_img[slot].tex, 64, 64, GPU_RGBA8)) return;
        C3D_TexSetFilter(&hud_img[slot].tex, GPU_NEAREST, GPU_NEAREST);
        hud_img[slot].ok = 1;
    }
    gpu_quiet(); /* (it may be drawn with in this frame's list, or the one in flight) */
    u32 *dst = hud_img[slot].tex.data;
    memset(dst, 0, 64 * 64 * 4);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) dst[tiled(x, y, 64)] = rgba_lut[clut[pix[y * w + x]]];
    mark_dirty(dst, 64 * 64 * 4);
    hud_img[slot].key = key;
}

/* ---- native models on the top screen (port/game/model_view.c: the Equip view's turning character) ----
 * A model's textures as 256x256 RGBA8 (from its TIMs, CLUT applied, texel 0 clear), and its triangles already posed
 * and projected (screen x, y in the top screen's 400x240, depth z, texel u, v) with a per-eye parallax: sorted back
 * to front and drawn over the top screen's scene, under its panels. */
#define MODEL_TEXS 12
#define MODEL_TRIS 4096
static C3D_Tex model_tex[MODEL_TEXS];
static int model_tex_ok[MODEL_TEXS];
static struct { float x[3], y[3], z, u[3], v[3], par[3]; u8 tex; } model_tri[MODEL_TRIS];
static int n_model_tris;
static u16 model_order[MODEL_TRIS];

/* Made when a menu first needs them, from the scene's texture cache if linear memory is short (late in a session
 * the cache holds nearly all of it: the Equip view's model drew nothing on hardware), and given back
 * MODEL_IDLE_FRAMES after the last model drawn, so play keeps the whole cache (holding them for good slowed fields
 * down). model_view.c reloads its models when the generation changes. */
#define MODEL_IDLE_FRAMES 300
static unsigned model_idle;
unsigned ctr_model_generation(void) { psx_gpu_sync(); return model_gen; }
/* (called by frame_begin once the GPU has finished what was sent: no draw can still read them) */
static void model_tex_free(void) {
    for (int i = 0; i < MODEL_TEXS; i++)
        if (model_tex_ok[i]) C3D_TexDelete(&model_tex[i]), model_tex_ok[i] = 0;
    model_gen++;
    model_free_pending = 0;
    slots_held_at = -1; /* (the cache may grow again) */
}

void ctr_model_texture(int slot, const u32 *rgba) { psx_gpu_sync(); /* rgba: 256x256, rows from the top, 0xRRGGBBAA */
    trace("model_texture", slot, 0);
    if (slot < 0 || slot >= MODEL_TEXS) return;
    model_idle = 0;
    if (!model_tex_ok[slot]) {
        gpu_quiet(); /* (the slots given up may be in this frame's draws) */
        while (linearSpaceFree() < 256 * 256 * 4 + SLOT_RESERVE && slot_shrink()) {}
        if (!C3D_TexInit(&model_tex[slot], 256, 256, GPU_RGBA8)) { port_log("model view: no memory for texture %d\n", slot); return; }
        C3D_TexSetFilter(&model_tex[slot], GPU_LINEAR, GPU_NEAREST);
        model_tex_ok[slot] = 1;
    }
    gpu_quiet();
    u32 *dst = model_tex[slot].data;
    for (int y = 0; y < 256; y++)
        for (int x = 0; x < 256; x++) dst[tiled(x, y, 256)] = rgba[y * 256 + x];
    mark_dirty(dst, 256 * 256 * 4);
}

void ctr_model_begin(void) { psx_gpu_sync(); n_model_tris = 0; }

/* one triangle: x, y, z (depth: larger farther), u, v per corner, its parallax per corner (pixels, + to the right
 * eye), texture slot */
void ctr_model_tri(int tex, const float *x, const float *y, const float *z, const float *u, const float *v,
                   const float *par) {
    psx_gpu_sync();
    if (n_model_tris == MODEL_TRIS || tex < 0 || tex >= MODEL_TEXS || !model_tex_ok[tex]) return;
    typeof(model_tri[0]) *t = &model_tri[n_model_tris++];
    for (int k = 0; k < 3; k++) t->x[k] = x[k], t->y[k] = y[k], t->u[k] = u[k], t->v[k] = v[k], t->par[k] = par[k];
    t->z = z[0] + z[1] + z[2];
    t->tex = (u8)tex;
}

static int model_far_first(const void *a, const void *b) {
    float za = model_tri[*(const u16 *)a].z, zb = model_tri[*(const u16 *)b].z;
    return za < zb ? 1 : za > zb ? -1 : 0;
}

static void draw_models(int eye) {
    if (!n_model_tris) return;
    C3D_Mtx p;
    Mtx_OrthoTilt(&p, 0.0f, 400.0f, 240.0f, 0.0f, 0.0f, 1.0f, true);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, u_proj, &p);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, u_stereo, 0, 0, 1, 0.5f);
    set_tev(K_TEX);
    set_blend(NO_BLEND, 0);
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    if (eye == 0 || !stereo_on) {
        for (int i = 0; i < n_model_tris; i++) model_order[i] = (u16)i;
        qsort(model_order, (size_t)n_model_tris, sizeof *model_order, model_far_first);
    }
    float side = stereo_on ? (eye ? 0.5f : -0.5f) * osGet3DSliderState() : 0.0f;
    int tex = -1;
    Vtx *run = NULL;
    unsigned nv = 0;
    for (int i = 0; i <= n_model_tris; i++) {
        int t = i < n_model_tris ? model_tri[model_order[i]].tex : -2;
        if (t != tex && run && nv) draw_tris(run - vb, (int)nv), run = NULL, nv = 0;
        if (t < 0) break;
        if (t != tex) {
            tex = t;
            tex_bind(0, &model_tex[t]);
            set_texscale(&model_tex[t]);
        }
        typeof(model_tri[0]) *m = &model_tri[model_order[i]];
        Vtx *v = alloc_vtx(3);
        if (!run) run = v;
        for (int k = 0; k < 3; k++) {
            Vtx c = {m->x[k] + side * m->par[k], m->y[k], 0, m->u[k], m->v[k], 0xFF808080};
            v[k] = c;
        }
        nv += 3;
    }
}

/* draw image slot at (x, y) of the bottom screen's UI (its 320x224 coordinates) this frame, w x h of it */
void ctr_hud_draw(int slot, int x, int y, int w, int h) {
    psx_gpu_sync();
    if (slot < 0 || slot >= HUD_IMAGES || !hud_img[slot].ok || n_hud_draw == 16) return;
    hud_draw[n_hud_draw].slot = slot;
    hud_draw[n_hud_draw].x = x; hud_draw[n_hud_draw].y = y;
    hud_draw[n_hud_draw].w = w; hud_draw[n_hud_draw].h = h;
    hud_draw[n_hud_draw].dw = w; hud_draw[n_hud_draw].dh = h;
    n_hud_draw++;
}

/* the same on the top screen (its 400x240), scaled to dw x dh */
void ctr_hud_draw_top(int slot, int x, int y, int w, int h, int dw, int dh) {
    psx_gpu_sync();
    ctr_hud_draw(slot, x + 1000, y, w, h);
    if (n_hud_draw && hud_draw[n_hud_draw - 1].x == x + 1000) hud_draw[n_hud_draw - 1].dw = dw, hud_draw[n_hud_draw - 1].dh = dh;
}

static void present_bottom(const PsxDisplayInfo *d, int w, int h) {
    int k = shown_layer;
    ctr_ui_oy = (240 - h) / 2;
    C3D_RenderTargetClear(bottom_rt, C3D_CLEAR_ALL, 0x000000FF, 0);
    C3D_FrameDrawOn(bottom_rt);
    if (d->rgb24 && k < 0) return; /* (a movie: black, unless the bottom screen drew for it: the boot card) */
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
    /* (the field's and the world map's bottom screen fades with the top: leaving a room it stayed lit as the scene
     * went to black, and the next room's map showed before the scene faded in) */
    int port_bottom_fade(void);
    int dark = menu_state ? 0 : port_bottom_fade();
    if (dark > 0) screen_dim(bottom_rt, 256 - (unsigned)dark);
}

/* A movie's frames in two textures of their own, used in turn: a frame changed is converted into the one the frame in
 * flight doesn't draw with, an unchanged one is drawn again as it is, no copy, no flush (on an Old 3DS the opening's
 * movie spent 11 ms of a VBlank presenting, copying and flushing 384 KB at each present). 768 KB of linear memory, freed
 * once no movie has been shown for 60 presents; without them, staging as before. */
static C3D_Tex movie_tex[2];
static int movie_ok, movie_cur = -1, movie_idle;
static u32 movie_gen;
static int movie_key[4];
static void movie_tex_free(void) {
    if (!movie_ok) return;
    gpu_sync(); /* (the frame in flight may draw with them) */
    C3D_TexDelete(&movie_tex[0]);
    C3D_TexDelete(&movie_tex[1]);
    movie_ok = 0, movie_cur = -1;
}
static void convert_rgb24(u8 *px, unsigned tw, const PsxDisplayInfo *d, int w, int h) {
    /* (the tiled address as a row's part and a column's, the column's looked up) */
    static unsigned col[400], col_tw;
    if (col_tw != tw) {
        for (unsigned x = 0; x < 400; x++) col[x] = ((x >> 3) * 64 + morton_x[x & 7]) * 3;
        col_tw = tw;
    }
    for (int y = 0; y < h; y++) {
        const u8 *src = (const u8 *)(psx_vram + ((d->y + y) & 511) * VW + d->x);
        u8 *row = px + ((unsigned)(y >> 3) * (tw >> 3) * 64 + morton_y[y & 7]) * 3;
        for (int x = 0; x < w; x++, src += 3) {
            u8 *o = row + col[x];
            o[0] = src[2]; o[1] = src[1]; o[2] = src[0];
        }
    }
}

static void present_rgb24(const PsxDisplayInfo *d, int w, int h, int ox, int oy) {
    trace("present_rgb24", w, h);
    cpu_reads(d->x, d->y, d->x + (w * 3 + 1) / 2, d->y + h);
    unsigned tw = pow2_at_least(w), th = pow2_at_least(h);
    movie_idle = 0;
    if (!movie_ok && tw == 512 && th == 256 && linearSpaceFree() > 2 * 512 * 256 * 3 + SLOT_RESERVE) {
        if (C3D_TexInit(&movie_tex[0], 512, 256, GPU_RGB8)) {
            if (C3D_TexInit(&movie_tex[1], 512, 256, GPU_RGB8)) {
                C3D_TexSetFilter(&movie_tex[0], GPU_NEAREST, GPU_NEAREST);
                C3D_TexSetFilter(&movie_tex[1], GPU_NEAREST, GPU_NEAREST);
                movie_ok = 1, movie_cur = -1;
            } else C3D_TexDelete(&movie_tex[0]);
        }
    }
    if (movie_ok && tw == 512 && th == 256) {
        int key[4] = {d->x, d->y, w, h};
        u32 newest = 0;
        int cx0, cy0, cx1, cy1;
        if (cell_range(d->x, d->y, d->x + (w * 3 + 1) / 2, d->y + h, &cx0, &cy0, &cx1, &cy1))
            for (int cy = cy0; cy < cy1; cy++)
                for (int cx = cx0; cx < cx1; cx++)
                    if (cell_gen[cy][cx] > newest) newest = cell_gen[cy][cx];
        if (movie_cur < 0 || newest > movie_gen || memcmp(key, movie_key, sizeof key)) {
            /* (the other one: drawn with last two presents ago at the latest, by a frame done once this one's began) */
            int next = movie_cur < 0 ? 0 : movie_cur ^ 1;
            convert_rgb24(movie_tex[next].data, 512, d, w, h);
            mark_dirty(movie_tex[next].data, movie_tex[next].size);
            movie_cur = next, movie_gen = gen_counter;
            __builtin_memcpy(movie_key, key, sizeof key);
        }
        HZ_TEX(&movie_tex[movie_cur], "movie frame");
        frame_begin(); /* (nothing else may have begun the frame: a movie's present draws alone) */
        for (int eye = 0; eye < (stereo_on ? 2 : 1); eye++) draw_to_top(top_rt[eye], &movie_tex[movie_cur], 0, 0, w, h, w, h, 0);
        (void)ox; (void)oy;
        return;
    }
    u8 *px = staging_alloc(tw * th * 3);
    /* (converted only when the frame's VRAM changed since: a movie's frames change 15 times a second and are presented
     * 60, and converting each present cost an Old 3DS 11 ms of a 16.7 ms VBlank in the opening's movie; else the last
     * conversion, kept, copied) */
    static u8 *kept;
    static u32 kept_gen;
    static int kept_key[4];
    int key[4] = {d->x, d->y, w, h};
    u32 newest = 0;
    int cx0, cy0, cx1, cy1;
    if (cell_range(d->x, d->y, d->x + (w * 3 + 1) / 2, d->y + h, &cx0, &cy0, &cx1, &cy1))
        for (int cy = cy0; cy < cy1; cy++)
            for (int cx = cx0; cx < cx1; cx++)
                if (cell_gen[cy][cx] > newest) newest = cell_gen[cy][cx];
    if (!kept) kept = malloc(512 * 256 * 3);
    if (kept && newest <= kept_gen && !memcmp(key, kept_key, sizeof key) && tw * th <= 512 * 256) {
        __builtin_memcpy(px, kept, tw * th * 3);
        C3D_Tex *t = staging_tex(px, tw, th, GPU_RGB8);
        frame_begin();
        for (int eye = 0; eye < (stereo_on ? 2 : 1); eye++) draw_to_top(top_rt[eye], t, 0, 0, w, h, w, h, 0);
        (void)ox; (void)oy;
        return;
    }
    convert_rgb24(px, tw, d, w, h);
    if (kept && tw * th <= 512 * 256) {
        __builtin_memcpy(kept, px, tw * th * 3);
        kept_gen = gen_counter;
        __builtin_memcpy(kept_key, key, sizeof key);
    }
    C3D_Tex *t = staging_tex(px, tw, th, GPU_RGB8);
    frame_begin();
    for (int eye = 0; eye < (stereo_on ? 2 : 1); eye++) draw_to_top(top_rt[eye], t, 0, 0, w, h, w, h, 0);
    (void)ox; (void)oy;
}

unsigned ctr_frames_presented(void) { return frames_drawn; }

void ctr_present(long frame) {
    psx_gpu_sync();
    trace("present", (int)frame, 0);
    extern volatile const char *ctr_where;
    ctr_where = "present";
    (void)frame;
    submit(); /* (with the parallax its runs were ordered for: stereo_k changes below) */
    button_icons_frame(); /* (for the next frame's sprites) */
    float slider = osGet3DSliderState();
    /* (3D on a New 3DS only: the second eye's drawing is more than an Old 3DS has time for in the busy scenes, and
     * its options page has no depth to set: settings.c port_setting_available) */
    int want_stereo = slider > 0.01f && ctr_is_new3ds();
    int port_setting_get(int k); /* (settings.c: 0 depth (50/100/150%), 1 sharp top screen) */
    slider *= 0.5f + 0.5f * (float)port_setting_get(0);
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
    if (psx_depth_on != stereo_on) { /* (the GTE's depths recorded in 3D only: make_vtx; from none when it comes on) */
        if (stereo_on) memset(psx_depth_tab, 0, sizeof psx_depth_tab), memset(psx_depth_sp_tab, 0, sizeof psx_depth_sp_tab),
            memset(psx_depth_val_tab, 0, sizeof psx_depth_val_tab);
        psx_depth_on = stereo_on;
    }

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
    /* Only UI layers drawn since the last present (the battle's list, drawn every VBlank, the scene every second one):
     * the bottom screen alone made again, the top left as it is. The whole present (the scene scaled to the top, its
     * copy kept for menus, the top's copy) took the GPU some 14 ms on an Old 3DS, and the next one, a VBlank later,
     * waited for it: ~8 ms of every battle VBlank. (Drawn: a cell under the displayed buffer drawn or written since the last
     * present; the back buffer's drawing and texture uploads leave the shown picture as it is) */
    int top_hud = 0;
    for (int i = 0; i < n_hud_draw; i++) top_hud |= hud_draw[i].x >= 1000;
    static int full_menu = -1;
    static float full_stereo_k;
    int light = !moved && !cells_newer(d.x, d.y, d.x + w, d.y + h, present_gen) && !d.rgb24 && !top_ui_head && !n_model_tris && !top_hud && !closing &&
                menu_state == full_menu && prev_menu == menu_state && stereo_k == full_stereo_k && !port_top_fade((u8[3]){0});
    present_gen = gen_counter;
    if (!light) full_menu = menu_state, full_stereo_k = stereo_k;
    ctr_render_stats[7] += light;
    shown_layer = bottom_layer(&d);
    if (light) {
        submit();
        frame_begin();
    } else if (d.rgb24) {
        submit();
        present_rgb24(&d, w, h, ox, oy);
    } else {
        if (movie_ok && ++movie_idle > 60) movie_tex_free();
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
        /* a menu closed: the game fades the scene in from black; the top keeps the paused scene instead, undimmed over
         * UNDIM_FRAMES, until the fade is done (the live scene then as it was: no flash to black) */
        if (prev_menu && !menu_state && frozen && port_game_mode() != 8 && prev_mode == port_game_mode()) closing = 1;
        prev_menu = menu_state, prev_mode = port_game_mode();
        static int settled; /* (presents since the fade was done: the displayed buffer was drawn a frame or two before) */
        settled = closing && !port_fade_busy() ? settled + 1 : 0;
        if (closing && (menu_state || (closing > UNDIM_FRAMES && settled > 3) || closing > 180)) closing = 0;
        int port_window_closing(void);
        static int unsettled; /* (frames the snapshot was held for a window animating: at most 30, a window kept small
                                * for good would otherwise hold an old scene) */
        int hold = have_freeze && port_window_closing() && unsettled < 30;
        unsettled = hold ? unsettled + 1 : 0;
        if (!menu_state && w >= 256 && !ctr_freeze_held && !closing && !hold) {
            /* (held: the battle's exit fade, battle_ui.c; a window shrinking: ui_split.c) */
            for (int eye = 0; eye < 2; eye++) {
                C3D_FrameDrawOn(freeze_rt[eye]);
                set_proj_ortho(UI_W, UI_H, 0.0f);
                C3D_FVUnifSet(GPU_VERTEX_SHADER, u_stereo, 0, 1, 1, -0.5f);
                set_tev(K_BLIT);
                set_blend(NO_BLEND, 0);
                C3D_Tex *src = &mirror_tex[stereo_on ? eye : 0];
                tex_bind(0, src);
                set_texscale(src);
                C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
                Vtx *v = alloc_vtx(6);
                quad(v, 0, 0, w, h, d.x, d.y, d.x + w, d.y + h, 0xFFFFFFFF);
                draw_tris(v - vb, 6);
            }
            freeze_w = w;
            freeze_h = h;
            have_freeze = 1;
            GSEG(GS_FREEZE);
        }
        if (want_menu && have_freeze) frozen = 1;
        else if ((!menu_state && !closing) || menu_state == 3) frozen = 0;
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
        static int field_after_battle;
        if (port_game_mode() == 2 || port_game_mode() == 8) field_after_battle = 1;
        else if (port_game_mode() != 1 || port_field_current()) field_after_battle = 0;
        for (int eye = 0; eye < (stereo_on ? 2 : 1); eye++) {
            if (menu_state == 3) { /* (the card game: its own top view, port/game/tm_ui.c, over black) */
                C3D_RenderTargetClear(top_rt[eye], C3D_CLEAR_ALL, 0x000000FF, 0);
                C3D_FrameDrawOn(top_rt[eye]);
                (void)mag_rx;
                (void)mag_ry;
            } else if (field_after_battle && port_game_mode() == 1) { /* (a field loading after a battle: black, as the battle left it. Its display
                * is 224 lines of buffers the battle drew 218 of: the rows below showed, scaled to the top's 240, as a dim
                * grey bar along its foot after the rewards; field_hud.c clears them once the field is in) */
                C3D_RenderTargetClear(top_rt[eye], C3D_CLEAR_ALL, 0x000000FF, 0);
                C3D_FrameDrawOn(top_rt[eye]);
            } else if (frozen && !port_rewards_skipped()) { /* a menu is up: the paused scene, dimmed to half (its panels
                                                              * are drawn over it after) */
                draw_to_top(top_rt[eye], &freeze_tex[eye], edge, 0, freeze_w - 2 * edge, freeze_h, freeze_w, freeze_h, edge);
                unsigned dim = closing ? 0x80 + 0x80 * (unsigned)closing / UNDIM_FRAMES : 0x80;
                if (port_game_mode() != 8 && dim < 0x100) screen_dim(top_rt[eye], dim); /* (the rewards: the battle as it ended) */
            }
            else {
                /* (the ending's vignettes: the scene without the black bar under it, at its size, in the middle; their
                 * subtitles are on the bottom screen: ui_split.c) */
                int port_top_crop(void), crop = port_top_crop(), th = crop > 0 && crop < h ? crop : h;
                draw_to_top(top_rt[eye], &mirror_tex[eye], d.x + edge, d.y, w - 2 * edge, th, w, h, edge);
                outline_draw(eye, d.x, d.y, w, h, th);
                int port_top_fade(u8 rgb[3]);
                u8 sub[3];
                if (port_top_fade(sub)) screen_subtract(top_rt[eye], sub);
            }
            if (closing && eye == (stereo_on ? 1 : 0)) closing++;
            /* (the display has moved to the other framebuffer: the one it left is drawn next, its mask cleared) */
            static int shown_buf = -1;
            if (eye == (stereo_on ? 1 : 0) && outline_rt[0][0] && outline_buf(d.x, d.y) != shown_buf) {
                shown_buf = outline_buf(d.x, d.y);
                for (int e = 0; e < 2; e++) C3D_RenderTargetClear(outline_rt[!shown_buf][e], C3D_CLEAR_ALL, 0, 0);
            }
        }
    }
    if (light) {
    } else if (n_model_tris) model_idle = 0;
    else if (++model_idle == MODEL_IDLE_FRAMES) model_free_pending = 1;
    for (int eye = 0; eye < (stereo_on ? 2 : 1); eye++) { /* (models over the scene, under the panels) */
        if (!n_model_tris) break;
        C3D_FrameDrawOn(top_rt[eye]);
        draw_models(eye);
    }
    GSEG(GS_TOP);
    n_model_tris = 0;
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
    GSEG(GS_BOTTOM);
    n_hud_draw = 0;
    if (!light) top_queue(stereo_on);
    frame_end();
    in_frame = 0;
    /* (runs' depth values from 1 again each frame, the depth buffer cleared at the next frame's begin, which waits for
     * this one anyway. Left to submit, which starts them again past half their range, the frame was ended and begun
     * there: the GPU waited for, every few frames) */
    if (mirror_depth && depth_next > 1) depth_next = depth_submitted = 1, depth_clear = 1;
#ifdef PORT_NO_OVERLAP
    frame_begin(); /* (test: the frame waited for at once, one half, as before the overlap) */
    half_restart();
#else
    gpu_busy = 1; /* (drawn while the next frame is recorded into the other half) */
    gs_presented();
    hz_use(vb, VB_MAX * sizeof(Vtx), "vertex half");
    hz_use(staging, STAGING_SIZE, "staging half");
    hz_sent();
    half ^= 1;
    half_restart();
#endif
    frames_drawn++;
    co_last = -1;
    world_ground = port_game_mode() == 3;
    slot_regroup();
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
#define TOP_FB_SIZE (240 * 400 * 3) /* (one eye, RGB8) */
static u8 *top_fb[3]; /* (each: the left eye, then the right, TOP_FB_SIZE on: ctr_video_init) */
static volatile int top_presented, top_queued;
static u32 px_top(int x, int y, const void *ctx) { /* (a top framebuffer: BGR8, by column, bottom row first) */
    const u8 *p = (const u8 *)ctx + (x * 240 + (239 - y)) * 3;
    return (u32)p[2] << 16 | (u32)p[1] << 8 | p[0];
}
/* DUMP_EVERY=N: the displayed buffer every N frames, as dump/f<frame>.ppm (a timeline of one run) */
static void dump_display_eye(long frame, int eye) {
    PsxDisplayInfo d;
    psx_gpu_display_info(&d);
    mkdir("sdmc:/3ds/ff9/dump", 0777);
    static u16 crop[320 * 240];
    int w = d.w > 320 ? 320 : d.w, h = d.h > 240 ? 240 : d.h;
    const u16 *m = mirror_tex[eye].data;
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
    snprintf(name, sizeof name, eye ? "f%06ld_r" : "f%06ld", frame);
    static u16 *src;
    src = crop;
    write_ppm(name, w, h, px_crop, src);
}

/* DUMP_EVERY's frame: the displayed buffer as the left eye's mirror has it, and in 3D the right eye's beside it
 * (f<frame>_r: for comparing stereo frames between builds) */
void ctr_render_dump_display(long frame) {
    psx_gpu_sync();
    gpu_sync();
    dump_display_eye(frame, 0);
    if (stereo_on) dump_display_eye(frame, 1);
    /* and the top screen as shown, each eye (f<frame>_top, _top_r): the world map's scene is drawn there from its own
     * buffers, while the PS1's displayed one holds the navigation map (the bottom screen's); its frames were compared
     * through that map alone */
    for (int i = 0; i < 200 && top_queued >= 0; i++) svcSleepThread(50000); /* (this frame's copy presented) */
    if (!top_fb[0] || top_presented < 0) return;
    const u8 *fb = top_fb[top_presented];
    GSPGPU_InvalidateDataCache(fb, 2 * TOP_FB_SIZE);
    for (int eye = 0; eye < (stereo_on ? 2 : 1); eye++) {
        char name[32];
        snprintf(name, sizeof name, eye ? "f%06ld_top_r" : "f%06ld_top", frame);
        write_ppm(name, 400, 240, px_top, fb + eye * TOP_FB_SIZE);
    }
}

void ctr_render_dump(void) {
    psx_gpu_sync();
    mkdir("sdmc:/3ds/ff9/dump", 0777);
    gpu_sync();
    write_ppm("mirror0", VW, VH, px_mirror, mirror_tex[0].data);
    write_ppm("mirror0_mask", VW, VH, px_mask, mirror_tex[0].data);
    if (stereo_on) write_ppm("mirror1", VW, VH, px_mirror, mirror_tex[1].data);
    write_ppm("shadow", VW, VH, px_shadow, psx_vram);
    for (int i = 0; i < nslots && i < 24; i++) {
        char name[48];
        snprintf(name, sizeof name, "slot%02d_%08lx", i, (unsigned long)slots[i].key);
        (void)name; /* (slots are atlas cells now: dumped with their atlas) */
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

/* The top screen in three buffers of our own (not citro3d's two). citro3d swaps a screen's two buffers as soon as the
 * GPU finishes a frame, and the GSP shows the new one only at the next VBlank: when two frames finished within one
 * refresh (busy fields on the 3DS: frames late, the GPU's end drifting against the VBlank), the second was copied into
 * the buffer on the screen, a tear (a vertical line: the LCDs scan by column). Waiting for the VBlank instead cost the
 * game a refresh most frames. With a third buffer a frame always has one that is neither shown nor about to be: the
 * copy is queued after the frame's drawing, into that one, and presented when the queue finishes (its callback, chained
 * after citro3d's), nothing waiting. */
static void (*c3d_queue_done)(gxCmdQueue_s *);
static volatile int top_shown = -1, top_presented = -1, top_queued = -1, top_queued_stereo;
void __real_GX_BindQueue(gxCmdQueue_s *queue);
void __wrap_GX_BindQueue(gxCmdQueue_s *queue) {
    c3d_queue = queue;
    __real_GX_BindQueue(queue);
}
static void top_present(int b, int stereo) {
    u32 mode = GSP_BGR8_OES | (stereo ? BIT(5) : BIT(6)) | 1 << 8; /* (3D or 2D; linear memory: as libctru's gfx.c) */
    const u8 *a = top_fb[b];
    /* (true: the present before was still pending, so replaced and never shown; else it is the one on the screen) */
    if (!gspPresentBuffer(GFX_TOP, (unsigned)b & 1, a, stereo ? a + TOP_FB_SIZE : a, 240 * 3, mode)) top_shown = top_presented;
    top_presented = b;
}
static void queue_done(gxCmdQueue_s *queue) { /* (the GSP's event thread) */
    if (c3d_queue_done) c3d_queue_done(queue);
    int b = top_queued;
    if (b >= 0) top_present(b, top_queued_stereo), top_queued = -1;
}
/* the frame's top copy, queued after its drawing; 0 without the buffers (citro3d's output then, as before) */
static int top_queue(int stereo) {
    if (!top_fb[0]) return 0;
    /* (the frame before presented: its queue was waited for at this frame's begin, its callback ends a moment later) */
    for (int i = 0; i < 200 && top_queued >= 0; i++) svcSleepThread(50000);
    if (top_queued >= 0) return 1; /* (never seen: skip the copy rather than write a shown buffer) */
    if (!gspIsPresentPending(GFX_TOP) && top_presented >= 0) top_shown = top_presented;
    int b = 0;
    while (b == top_shown || b == top_presented) b++;
    /* (the drawing first: its command list flushed from the CPU's data cache, as frame_end's is. Without the flag the
     * 3DS's GPU read stale memory as commands and hung at the third frame; Azahar has no cache, so it ran there). And what
     * the frame drew with, flushed before it starts as frame_end's flush would: else the GPU read vertices and texture
     * blocks written this frame from stale memory (a menu's top panels on the world map without their body) */
    flush_pending();
#ifdef PORT_PROFILE
    { /* (a list sent here is timed as the others) */
        u32 *buf, size;
        gs_take(GS_END);
        if (C3Di_SplitFrame(&buf, &size)) gq_sending(GS_END), GX_ProcessCommandList(buf, size * 4, GX_CMDLIST_FLUSH);
    }
#else
    C3D_FrameSplit(GX_CMDLIST_FLUSH);
#endif
    for (int eye = 0; eye < (stereo ? 2 : 1); eye++)
        GX_DisplayTransfer((u32 *)top_rt[eye]->frameBuf.colorBuf, GX_BUFFER_DIM(240, 400),
                           (u32 *)(top_fb[b] + eye * TOP_FB_SIZE), GX_BUFFER_DIM(240, 400), DISPLAY_TRANSFER_FLAGS);
    top_queued_stereo = stereo;
    top_queued = b;
    return 1;
}

void ctr_video_init(void) {
    C3D_Init(0x100000);
    /* (two of the three are libctru's own top framebuffers, sized for both eyes, the right one half way, and unused
     * once citro3d no longer copies to them: only the third is allocated. Three of our own took 1.7 MB of linear
     * memory from the texture cache: 63 pages instead of 71, and the texture-heavy scenes slowed on the 3DS) */
    u8 *extra = c3d_queue ? linearAlloc(2 * TOP_FB_SIZE) : NULL;
    if (extra) {
        top_fb[0] = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL); /* (the back buffer) */
        gfxSetDoubleBuffering(GFX_TOP, false);
        top_fb[1] = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL); /* (the front one) */
        gfxSetDoubleBuffering(GFX_TOP, true);
        top_fb[2] = extra;
        top_shown = 1; /* (gfxInit presented it) */
        memset(extra, 0, 2 * TOP_FB_SIZE);
        GSPGPU_FlushDataCache(extra, 2 * TOP_FB_SIZE);
        c3d_queue_done = c3d_queue->callback;
        gxCmdQueueSetCallback(c3d_queue, queue_done, c3d_queue->user);
    }
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

    /* the PS1's 4x4 dither pattern, as 128 + its offset (set_tev_for adds it signed), tiled to 8x8 */
    static const signed char dither[4][4] = {{-4, 0, -3, 1}, {2, -2, 3, -1}, {-3, 1, -4, 0}, {3, -1, 2, -2}};
    if (C3D_TexInit(&dither_tex, 8, 8, GPU_RGBA8)) {
        C3D_TexSetFilter(&dither_tex, GPU_NEAREST, GPU_NEAREST);
        C3D_TexSetWrap(&dither_tex, GPU_REPEAT, GPU_REPEAT);
        u32 *d = dither_tex.data;
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++) {
                u32 c = (u32)(128 + dither[y & 3][x & 3]);
                d[tiled(x, y, 8)] = c << 24 | c << 16 | c << 8 | 0xFF;
            }
        GSPGPU_FlushDataCache(d, 8 * 8 * 4);
    }
    for (int i = 0; i < 2; i++) {
        vb_half[i] = linearAlloc(VB_MAX * sizeof(Vtx));
        staging_half[i] = linearAlloc(STAGING_SIZE);
        if (!vb_half[i] || !staging_half[i]) port_fatal("no linear memory for the renderer");
    }
    vb = vb_half[0];
    staging = staging_half[0];
    for (int eye = 0; eye < 2; eye++) {
        if (!C3D_TexInitVRAM(&mirror_tex[eye], VW, VH, GPU_RGBA5551)) port_fatal("no VRAM for the PS1 VRAM mirror");
        C3D_TexSetFilter(&mirror_tex[eye], GPU_NEAREST, GPU_NEAREST);
        mirror_rt[eye] = C3D_RenderTargetCreateFromTex(&mirror_tex[eye], GPU_TEXFACE_2D, 0, -1);
        if (eye == 0) mirror_depth = vramAlloc(VW * VH * 2);
        if (mirror_depth) C3D_FrameBufDepth(&mirror_rt[eye]->frameBuf, mirror_depth, GPU_RB_DEPTH16);
        top_rt[eye] = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, -1);
        if (!top_fb[0]) C3D_RenderTargetSetOutput(top_rt[eye], GFX_TOP, eye ? GFX_RIGHT : GFX_LEFT, DISPLAY_TRANSFER_FLAGS);
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
    /* the page a 15-bit texture is copied into (mirror_page), cleared here once, before any frame: a fill in the middle
     * of a frame is what hung the GPU on hardware (the depth reset) */
    if (C3D_TexInitVRAM(&page_tex, 256, 256, GPU_RGBA5551) &&
        (page_rt = C3D_RenderTargetCreateFromTex(&page_tex, GPU_TEXFACE_2D, 0, -1))) {
        C3D_TexSetFilter(&page_tex, GPU_NEAREST, GPU_NEAREST);
        C3D_RenderTargetClear(page_rt, C3D_CLEAR_ALL, 0x000000FF, 0); /* (its alpha 1: the copies write colour) */
    }
    bottom_rt = C3D_RenderTargetCreate(240, 320, GPU_RB_RGBA8, -1);
    C3D_RenderTargetSetOutput(bottom_rt, GFX_BOTTOM, GFX_LEFT, DISPLAY_TRANSFER_FLAGS);
    if (!vb || !staging) port_fatal("no linear memory for the renderer");
    /* the mirror starts as the shadow does: black, and current */
    memset(shadow_ok, 1, sizeof shadow_ok);
    memset(mirror_ok, 0, sizeof mirror_ok);
    cells_ver++;
    memset(cell_cols, 0, sizeof cell_cols);
    gp0_backend = &pica_backend;
    if (mirror_depth) C3D_RenderTargetClear(mirror_rt[0], C3D_CLEAR_DEPTH, 0, 0);
    port_log("renderer: PICA200, %u KB linear, %u KB VRAM free%s%s\n", (unsigned)(linearSpaceFree() >> 10),
             (unsigned)(vramSpaceFree() >> 10), mirror_depth ? "" : " (no depth for the mirror: runs not reordered)",
             top_fb[0] ? "" : " (top screen double-buffered: may tear)");
}

void ctr_shutdown(void) {
    psx_gpu_sync();
    extern volatile const char *ctr_where;
    void ctr_audio_exit(void);
    ctr_where = "closing: worker";
    void ctr_core1_closing(void);
    ctr_core1_closing();
    ctr_async_exit();
    void ctr_spu_exit(void);
    ctr_where = "closing: audio thread";
    ctr_spu_exit();
    ctr_where = "closing: voices";
    ctr_voice_exit();
    ctr_where = "closing: audio";
    ctr_audio_exit();
    /* No graphics teardown: closing comes from the HOME menu, which holds the GPU by then, so ending the frame or
     * C3D_Fini waits for GPU work that never runs (the close hung on "Closing software" on hardware). The process's
     * end frees our GPU memory. But the GSP service's event thread is stopped (gspExit: it waits for no GPU work):
     * left running, it woke on an interrupt after the exit had freed the heap its stack is in, and the whole system
     * stopped (a crash in gspEventThreadMain on closing, 3DS). So is the watchdog thread, whose stack is there too. */
    void ctr_watchdog_stop(void);
    ctr_watchdog_stop();
    ctr_where = "closing: GSP";
    gspExit();
    ctr_where = "closing: done";
}
#endif
