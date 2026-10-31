/* libgpu: drawing state, a 1024x512 VRAM shadow, packet builders, and the ordering-table walker.
 *
 * VRAM: the game uploads textures and CLUTs (LoadImage), copies within VRAM (MoveImage, DR_MOVE packets), reads
 * back (StoreImage) and clears (ClearImage, isbg). All of that is kept exactly in the shadow, which the renderer
 * converts textures from (M2). Packet builders produce the GP0 words psx-spx documents, since game code may
 * build or patch packets by hand too.
 *
 * DrawOTag walks the ordering table in the PS1's order. OT links are 24-bit (P_TAG.addr): psx_ram lives in the
 * one 16 MB window both platforms share (DSi main RAM 0x02000000-0x02FFFFFF; the PC maps psx_ram there), so a
 * link rebuilds as (psx_ram & 0xFF000000) | addr24. The walker applies VRAM fills and copies to the shadow and
 * hands every packet to the GP0 decoder (gp0.c), which drives the active renderer backend.
 */
#include "psyq_port.h"
#include "profile.h"
#include "psx_display.h"

#define VRAM_W 1024
#define VRAM_H 512
#define OT_END 0xFFFFFF

/* Allocated by psx_mem_init: 1 MB doesn't fit a 4 MB DS next to everything else, so there it stays NULL and VRAM
 * operations are skipped (a headless run that still exercises the game logic). */
u16 *psx_vram;

static DRAWENV draw_env;
static DISPENV disp_env;
static void (*drawsync_cb)(void);
static int drawsync_owed;
static int graph_debug;

/* PC --trace-gpu: log every VRAM write and read (fills, uploads, copies, readbacks) */
int psx_trace_gpu;

/* per-frame statistics (debug overlay, perf.md) */
unsigned int psx_gpu_packets, psx_gpu_words;

/* Drawing: every packet goes through the GP0 decoder (gp0.c), which keeps the drawing state and hands primitives to
 * the active backend (software reference, or the DS 3D hardware). */
#include "gp0.h"

static inline u32 *link_ptr(u32 addr24) {
    return (u32 *)(((u32)psx_ram & 0xFF000000) | addr24);
}

/* ---- VRAM ----------------------------------------------------------------------------------------------- */

static inline u16 *vram_at(int x, int y) {
    return &psx_vram[(y & (VRAM_H - 1)) * VRAM_W + (x & (VRAM_W - 1))];
}

/* DS renderer: VRAM area written, so textures made from it get rebuilt (render_gx.c) */
__attribute__((weak)) void psx_vram_touch(int x, int y, int w, int h) {
}
/* softgpu.c: rows y..y+h-1 written (its cached palettes check their CLUT rows); weak for tests without it */
__attribute__((weak)) void psx_raster_rows_written(int y, int h) { (void)y; (void)h; }

/* Hardware backends materialise rendered pixels before CPU/GPU readback. */
__attribute__((weak)) void psx_vram_read(int x, int y, int w, int h) {
}
/* A small read the backend can serve without materialising the whole scene: 1 if it put the pixels in place, which
 * psx_vram_peek_end puts back after the copy (gx_hybrid.c). */
__attribute__((weak)) int psx_vram_peek_begin(int x, int y, int w, int h) { (void)x; (void)y; (void)w; (void)h; return 0; }
__attribute__((weak)) void psx_vram_peek_end(void) {}

__attribute__((weak)) unsigned int psx_vram_generation; /* also in gp0.c */
__attribute__((weak)) unsigned int psx_display_generation;
__attribute__((weak)) int psx_display_watch[4];

/* A renderer that keeps scenes outside VRAM learns their clearing fills here (gx_hybrid.c). */
__attribute__((weak)) void psx_vram_filled(int x, int y, int w, int h, unsigned int rgb15) {
    (void)x; (void)y; (void)w; (void)h; (void)rgb15;
}

#if defined(__arm__)
/* `groups` (> 0) runs of four words of v from q, as STMIA bursts; returns the word after them. Its own ARM function
 * with the registers named: as inline asm in vram_fill's loop the compiler lost track of the count (fills ran past
 * their span: the battle HP panel's cached digits turned into black boxes). */
__attribute__((target("arm"), noinline)) u32 *fill_bursts(u32 *q, u32 v, unsigned groups) { /* (also gx_hybrid.c's) */
    __asm__ volatile("mov r4, %2\n\tmov r5, %2\n\tmov r6, %2\n\tmov r7, %2\n"
                     "1:\tstmia %0!, {r4-r7}\n\tsubs %1, %1, #1\n\tbne 1b"
                     : "+r"(q), "+r"(groups) : "r"(v) : "r4", "r5", "r6", "r7", "memory", "cc");
    return q;
}
#endif
static void vram_fill(int x, int y, int w, int h, u16 c) {
    if (!psx_vram) {
        return;
    }
    psx_vram_filled(x, y, w, h, c);
    psx_vram_generation++;
    psx_raster_rows_written(y, h);
    psx_display_written(x, y, w, h);
    psx_vram_touch(x, y, w, h);
    /* Row segments up to the VRAM edge (wrapping as vram_at does), with paired 32-bit stores. */
    const u32 pair = c | (u32)c << 16;
    for (int j = 0; j < h; j++) {
        u16 *row = psx_vram + ((y + j) & (VRAM_H - 1)) * VRAM_W;
        int xs = x & (VRAM_W - 1), left = w;
        while (left > 0) {
            int span = left < VRAM_W - xs ? left : VRAM_W - xs;
            u16 *p = row + xs;
            int n = span;
            if (((uintptr_t)p & 2) && n) *p++ = c, n--;
            u32 *q = (u32 *)p;
#if defined(__arm__)
            /* burst stores (STMIA of four words): faster on the DS's main RAM than single ones */
            if (n >= 8) {
                unsigned groups = (unsigned)n / 8;
                q = fill_bursts(q, pair, groups);
                n -= (int)groups * 8;
            }
#endif
            for (; n >= 2; n -= 2) *q++ = pair;
            if (n) *(u16 *)q = c;
            left -= span;
            xs = 0;
        }
    }
}

static void vram_copy(int sx, int sy, int dx, int dy, int w, int h) {
    if (!psx_vram || w <= 0 || h <= 0) {
        return;
    }
    /* Battle texture animation emits identity moves while its offset is zero.
     * They change no pixels and must not dirty immutable texture generations
     * or force deferred draws to rasterise. Preserve the ordinary path when
     * force-mask could make an otherwise identical copy change destination bits. */
    if (!gp0_state.set_mask && !((sx ^ dx) & 1023) && !((sy ^ dy) & 511)) return;
    const int check_mask=gp0_state.check_mask;
    const u16 set_mask=gp0_state.set_mask?0x8000:0;
    psx_vram_read(sx, sy, w, h);
    if (check_mask && (((sx ^ dx) & 1023) || ((sy ^ dy) & 511)))
        psx_vram_read(dx, dy, w, h);
    psx_vram_touch(dx, dy, w, h);
    psx_vram_generation++;
    psx_raster_rows_written(dy, h);
    psx_display_written(dx, dy, w, h);
    /* Snapshot each source row. Non-wrapping overlaps match the published
     * ps1-tests reference; cross-boundary overlap behavior remains unverified. */
    if (!check_mask && !set_mask && (sx & 1023) + w <= VRAM_W && (dx & 1023) + w <= VRAM_W) {
        /* no mask and no wrap across the edge: each row as one move (the same snapshot-then-write per row) */
        for (int j = 0; j < h; j++)
            __builtin_memmove(vram_at(dx, dy + j), vram_at(sx, sy + j), (unsigned)w * 2);
        return;
    }
    static u16 row[VRAM_W];
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            row[i] = *vram_at(sx + i, sy + j);
        }
        for (int i = 0; i < w; i++) {
            u16 *dst=vram_at(dx+i,dy+j);
            if (!check_mask || !(*dst&0x8000)) *dst=row[i]|set_mask;
        }
    }
}

static u16 rgb15(int r, int g, int b) {
    return (u16)(((r >> 3) & 0x1F) | (((g >> 3) & 0x1F) << 5) | (((b >> 3) & 0x1F) << 10));
}

/* set while a movie's frames come from its transcode (port/game/movie.c): the player's uploads of its (unused) MDEC
 * output are dropped */
int psx_load_image_skip;

int LoadImage(RECT *r, u32 *p) {
    if (psx_trace_gpu) {
        port_log("gpu: LoadImage %d,%d %dx%d\n", r->x, r->y, r->w, r->h);
    }
    if (!psx_vram || psx_load_image_skip) {
        return 0;
    }
    const int check_mask=gp0_state.check_mask;
    const u16 set_mask=gp0_state.set_mask?0x8000:0;
    if (check_mask) psx_vram_read(r->x,r->y,r->w,r->h);
    psx_vram_touch(r->x, r->y, r->w, r->h);
    psx_vram_generation++;
    psx_raster_rows_written(r->y, r->h);
    psx_display_written(r->x, r->y, r->w, r->h);
    /* the PS1 moves these by DMA, which ignores an address's low two bits (FF9 passes odd ones: field 1403) */
    const u16 *src = (const u16 *)((uintptr_t)p & ~(uintptr_t)3);
    if (!check_mask && !set_mask && r->w > 0 && (r->x & 1023) + r->w <= VRAM_W) { /* (rows within the edge) */
        for (int j = 0; j < r->h; j++, src += r->w) __builtin_memcpy(vram_at(r->x, r->y + j), src, (unsigned)r->w * 2);
        return 0;
    }
    for (int j = 0; j < r->h; j++) {
        for (int i = 0; i < r->w; i++) {
            u16 *dst=vram_at(r->x+i,r->y+j);
            if (!check_mask || !(*dst&0x8000)) *dst=*src|set_mask;
            src++;
        }
    }
    return 0;
}

int StoreImage(RECT *r, u32 *p) {
    if (psx_trace_gpu) {
        port_log("gpu: StoreImage %d,%d %dx%d\n", r->x, r->y, r->w, r->h);
    }
    if (!psx_vram) {
        return 0;
    }
    int peek = psx_vram_peek_begin(r->x, r->y, r->w, r->h);
    if (!peek) psx_vram_read(r->x, r->y, r->w, r->h);
    /* by DMA, word-aligned: FF9 stores a single pixel to an odd address (Fossil Roo, field 1403): unaligned, the
     * pixel and the transfer's padding halfword landed two bytes past the PS1's word, over a pointer */
    u16 *dst = (u16 *)((uintptr_t)p & ~(uintptr_t)3);
    if (r->w > 0 && (r->x & 1023) + r->w <= VRAM_W) /* (rows within the edge) */
        for (int j = 0; j < r->h; j++, dst += r->w) __builtin_memcpy(dst, vram_at(r->x, r->y + j), (unsigned)r->w * 2);
    else
        for (int j = 0; j < r->h; j++) {
            for (int i = 0; i < r->w; i++) {
                *dst++ = *vram_at(r->x + i, r->y + j);
            }
        }
    /* The PSX transfer is word-sized: ceil(w*h/2) GPUREAD words. An odd
     * final pixel has a zero high halfword, not the caller's previous data. */
    if (r->w > 0 && r->h > 0 && ((r->w * r->h) & 1)) *dst = 0;
    if (peek) psx_vram_peek_end();
    return 0;
}

int MoveImage(RECT *r, int x, int y) {
    if (psx_trace_gpu) {
        port_log("gpu: MoveImage %d,%d %dx%d -> %d,%d\n", r->x, r->y, r->w, r->h, x, y);
    }
    vram_copy(r->x, r->y, x, y, r->w, r->h);
    return 0;
}

void ClearImage(RECT *r, s32 cr, s32 cg, s32 cb) {
    if (psx_trace_gpu) {
        port_log("gpu: ClearImage %d,%d %dx%d rgb %ld,%ld,%ld\n", r->x, r->y, r->w, r->h, (long)cr, (long)cg, (long)cb);
    }
    vram_fill(r->x, r->y, r->w, r->h, rgb15(cr, cg, cb));
}

u16 GetTPage(int tp, int abr, int x, int y) {
    return (u16)(((tp & 3) << 7) | ((abr & 3) << 5) | ((y & 0x100) >> 4) | ((x & 0x3FF) >> 6) | ((y & 0x200) << 2));
}

u16 GetClut(int x, int y) {
    return (u16)((y << 6) | ((x >> 4) & 0x3F));
}

u16 LoadTPage(u32 *pix, s32 tp, s32 abr, s32 x, s32 y, s32 w, s32 h) {
    RECT r = {(s16)x, (s16)y, (s16)(tp == 0 ? w / 4 : tp == 1 ? w / 2 : w), (s16)h};
    LoadImage(&r, pix);
    return GetTPage(tp, abr, x, y);
}

u16 LoadClut(u32 *clut, s32 x, s32 y) {
    RECT r = {(s16)x, (s16)y, 256, 1};
    LoadImage(&r, clut);
    return GetClut(x, y);
}

/* ---- environments --------------------------------------------------------------------------------------- */

int ResetGraph(int mode) {
    return 0;
}

int SetGraphDebug(int level) {
    int old = graph_debug;
    graph_debug = level;
    return old;
}

void SetDispMask(int mask) {
}

DRAWENV *SetDefDrawEnv(DRAWENV *env, int x, int y, int w, int h) {
    env->clip.x = (s16)x, env->clip.y = (s16)y, env->clip.w = (s16)w, env->clip.h = (s16)h;
    env->ofs[0] = (s16)x, env->ofs[1] = (s16)y;
    env->tw.x = env->tw.y = env->tw.w = env->tw.h = 0;
    env->tpage = GetTPage(0, 0, 640, 0);
    env->dtd = 1;
    env->dfe = 1;
    env->isbg = 0;
    env->r0 = env->g0 = env->b0 = 0;
    return env;
}

DISPENV *SetDefDispEnv(DISPENV *env, int x, int y, int w, int h) {
    env->disp.x = (s16)x, env->disp.y = (s16)y, env->disp.w = (s16)w, env->disp.h = (s16)h;
    env->screen.x = env->screen.y = env->screen.w = env->screen.h = 0;
    env->isinter = 0;
    env->isrgb24 = 0;
    env->pad0 = env->pad1 = 0;
    return env;
}

static int env_limit(int value, int limit) {
    value = (s16)value;
    return value < 0 ? 0 : value >= limit ? limit - 1 : value;
}

static u32 area_tl(const RECT *r) {
    return 0xE3000000 | (env_limit(r->y, VRAM_H) << 10) | env_limit(r->x, VRAM_W);
}

static u32 area_br(const RECT *r) {
    return 0xE4000000 | (env_limit(r->y + r->h - 1, VRAM_H) << 10)
        | env_limit(r->x + r->w - 1, VRAM_W);
}

static u32 offset_word(int x, int y) {
    return 0xE5000000 | ((y & 0x7FF) << 11) | (x & 0x7FF);
}

static inline void set_len(void *p, unsigned int len) /* via P_TAG: psx_ram is accessed through any type */ {
    ((P_TAG *)p)->len = len;
}

static void make_draw_env(DR_ENV *dr, DRAWENV *env, int immediate) {
    unsigned int n = 0;
    dr->code[n++] = area_tl(&env->clip);
    dr->code[n++] = area_br(&env->clip);
    dr->code[n++] = offset_word(env->ofs[0], env->ofs[1]);
    dr->code[n++] = get_mode(env->dfe, env->dtd, env->tpage);
    dr->code[n++] = get_tw(&env->tw);
    dr->code[n++] = 0xE6000000;
    if (env->isbg) {
        int w = env_limit(env->clip.w, VRAM_W), h = env_limit(env->clip.h, VRAM_H);
        /* PsyQ SetDrawEnv always clears with a clipped TILE. PutDrawEnv's
         * internal builder uses an absolute fill only for 64-pixel alignment.
         * In particular, an unaligned saved environment must never fill VRAM
         * outside its drawing area and overwrite resident texture pages. */
        int fill = immediate && !(env->clip.x & 63) && !(w & 63);
        int x = env->clip.x - (fill ? 0 : env->ofs[0]);
        int y = env->clip.y - (fill ? 0 : env->ofs[1]);
        dr->code[n++] = (fill ? 0x02000000 : 0x60000000)
            | (env->b0 << 16) | (env->g0 << 8) | env->r0;
        dr->code[n++] = ((u32)(u16)y << 16) | (u16)x;
        dr->code[n++] = ((u32)h << 16) | (u16)w;
    }
    set_len(dr, n);
}

void SetDrawEnv(DR_ENV *dr, DRAWENV *env) {
    make_draw_env(dr, env, 0);
}

static void draw_packet(u32 *p);

/* bumped when the drawing or display area may change (the hybrid's target caches its lookups by it) */
unsigned int psx_gpu_env_version;

DRAWENV *PutDrawEnv(DRAWENV *env) {
    psx_gpu_env_version++;
    make_draw_env(&env->dr_env, env, 1);
    ((P_TAG *)&env->dr_env)->addr = OT_END;
    __builtin_memcpy(&draw_env, env, sizeof draw_env);
    draw_packet((u32 *)&env->dr_env);
    return env;
}

/* primitives drawn since the display last moved (gp0.c), and the most a scene had (sizing the DS's hybrid scene
 * buffers) */
extern unsigned int psx_gpu_scene_prims;
unsigned int psx_gpu_scene_prims_max;

DISPENV *PutDispEnv(DISPENV *env) {
    if (env->disp.x != disp_env.disp.x || env->disp.y != disp_env.disp.y) {
        if (psx_gpu_scene_prims > psx_gpu_scene_prims_max) psx_gpu_scene_prims_max = psx_gpu_scene_prims;
        psx_gpu_scene_prims = 0;
    }
    __builtin_memcpy(&disp_env, env, sizeof disp_env);
    psx_gpu_env_version++;
    return env;
}

/* the display area the game last set (PutDispEnv): what the TV shows */
const DISPENV *psx_gpu_dispenv(void) {
    return &disp_env;
}

void psx_gpu_display_info(PsxDisplayInfo *info) {
    *info = (PsxDisplayInfo){disp_env.disp.x, disp_env.disp.y,
                           disp_env.disp.w, disp_env.disp.h, !!disp_env.isrgb24};
}

/* The framebuffer selected by PutDrawEnv, independent of subsequent GP0 clipping commands. */
void psx_gpu_draw_area(int *x, int *y, int *w, int *h) {
    *x = draw_env.clip.x;
    *y = draw_env.clip.y;
    *w = draw_env.clip.w;
    *h = draw_env.clip.h;
}

void psx_gpu_display_area(int *x, int *y, int *w, int *h) {
    *x = disp_env.disp.x;
    *y = disp_env.disp.y;
    *w = disp_env.disp.w;
    *h = disp_env.disp.h;
}

void GetDrawEnv2(DR_ENV *p) {
    /* GPU readback captures the current GP0 registers, including state changed
     * by packets after PutDrawEnv. Battle inserts this packet after its UI OT
     * to restore the interrupted scene's drawing state. No framebuffer clear. */
    const Gp0State *s = &gp0_state;
    p->code[0] = 0xE3000000 | ((s->area_y0 & 0x3FF) << 10) | (s->area_x0 & 0x3FF);
    p->code[1] = 0xE4000000 | ((s->area_y1 & 0x3FF) << 10) | (s->area_x1 & 0x3FF);
    p->code[2] = offset_word(s->ofs_x, s->ofs_y);
    p->code[3] = 0xE1000000 | (s->texpage & 0xFFF);
    p->code[4] = 0xE2000000 | ((s->twin_mask_x >> 3) & 31)
        | (((s->twin_mask_y >> 3) & 31) << 5)
        | (((s->twin_off_x >> 3) & 31) << 10)
        | (((s->twin_off_y >> 3) & 31) << 15);
    p->code[5] = 0xE6000000 | (s->set_mask != 0) | ((s->check_mask != 0) << 1);
    set_len(p, 6);
}

/* ---- packet builders ------------------------------------------------------------------------------------ */

void SetDrawArea(DR_AREA *p, RECT *r) {
    ((u32 *)p)[1] = area_tl(r);
    ((u32 *)p)[2] = area_br(r);
    set_len(p, 2);
}

void SetDrawOffset(DR_OFFSET *p, s16 *ofs) {
    ((u32 *)p)[1] = offset_word(ofs[0], ofs[1]);
    ((u32 *)p)[2] = 0;
    set_len(p, 2);
}

void SetTexWindow(void *p, RECT *r) {
    ((u32 *)p)[1] = get_tw(r);
    ((u32 *)p)[2] = 0;
    set_len(p, 2);
}

void SetDrawMove(void *p, RECT *r, int x, int y) {
    u32 *w = p;
    set_len(p, (r->w && r->h) ? 5 : 0);
    w[1] = 0x01000000;
    w[2] = 0x80000000;
    w[3] = ((u32)r->y << 16) | (u16)r->x;
    w[4] = ((u32)y << 16) | (u16)x;
    w[5] = ((u32)r->h << 16) | (u16)r->w;
}

void SetDrawStp(DR_STP *p, int pbw) {
    ((u32 *)p)[1] = 0xE6000000 | (pbw & 3);
    ((u32 *)p)[2] = 0;
    set_len(p, 2);
}

void SetDrawTPage(void *p, int dfe, int dtd, int tpage) {
    ((u32 *)p)[1] = get_mode(dfe, dtd, tpage);
    set_len(p, 1);
}

static inline void init_prim(void *p, unsigned int len, u8 code) {
    set_len(p, len);
    ((P_TAG *)p)->code = code;
}

void SetPolyF3(POLY_F3 *p) { init_prim(p, 4, 0x20); }
void SetPolyF4(POLY_F4 *p) { init_prim(p, 5, 0x28); }
void SetPolyG3(POLY_G3 *p) { init_prim(p, 6, 0x30); }
void SetPolyG4(POLY_G4 *p) { init_prim(p, 8, 0x38); }
void SetPolyFT4(POLY_FT4 *p) { init_prim(p, 9, 0x2C); }
void SetSprt(SPRT *p) { init_prim(p, 4, 0x64); }
void SetSprt16(SPRT_16 *p) { init_prim(p, 3, 0x7C); }
void SetTile(TILE *p) { init_prim(p, 3, 0x60); }
void SetLineF2(LINE_F2 *p) { init_prim(p, 3, 0x40); }
void SetLineG2(LINE_G2 *p) { init_prim(p, 4, 0x50); }

void SetSemiTrans(void *p, int abe) {
    if (abe) {
        ((P_TAG *)p)->code |= 2;
    } else {
        ((P_TAG *)p)->code &= ~2;
    }
}

void AddPrim(void *ot, void *p) {
    ((P_TAG *)p)->addr = ((P_TAG *)ot)->addr;
    ((P_TAG *)ot)->addr = (u32)p & 0xFFFFFF;
}

int MargePrim(void *p0, void *p1) {
    unsigned int n = ((P_TAG *)p0)->len + ((P_TAG *)p1)->len + 1;
    if (n > 16) {
        return -1;
    }
    set_len(p0, n);
    *(u32 *)p1 = 0;
    return 0;
}

u32 *ClearOTag(u32 *ot, int n) {
    for (int i = 0; i < n - 1; i++) {
        ot[i] = (u32)&ot[i + 1] & 0xFFFFFF;
    }
    ot[n - 1] = OT_END;
    return ot;
}

/* The scene's main OT (the largest ClearOTagR'd, 0x1000 entries and up: battle's 0x1010) and the entry of it the
 * walk last passed (draw_ot): the hybrid renderer places native meshes among the packets by it (gx_hybrid.c). */
unsigned int *psx_main_ot;
unsigned int psx_main_ot_n, psx_ot_slot, psx_ot_generation; /* generation: bumped when the main OT is cleared */

u32 *ClearOTagR(u32 *ot, int n) {
    if (n >= 0x1000) { psx_main_ot = (unsigned int *)ot; psx_main_ot_n = (unsigned int)n; psx_ot_slot = (unsigned int)n; ++psx_ot_generation; }
    ot[0] = OT_END;
    int i = 1;
    for (u32 a = (u32)(uintptr_t)ot; i + 4 <= n; i += 4) { /* (four a step: the battle's 0x1010 every frame) */
        u32 e = (a + (u32)(i - 1) * 4) & 0xFFFFFF;
        ot[i] = e; ot[i + 1] = (e + 4) & 0xFFFFFF; ot[i + 2] = (e + 8) & 0xFFFFFF; ot[i + 3] = (e + 12) & 0xFFFFFF;
    }
    for (; i < n; i++) {
        ot[i] = (u32)&ot[i - 1] & 0xFFFFFF;
    }
    return ot;
}

/* ---- drawing -------------------------------------------------------------------------------------------- */

/* Words used by the GP0 command at w[0] (psx-spx "GPU Command Summary"); `avail` caps it. */
unsigned int psx_gp0_len(const u32 *w, unsigned int avail) {
    u32 cmd = w[0] >> 24;
    unsigned int n = 1;
    if (cmd >= 0x20 && cmd < 0x40) { /* polygon */
        int gouraud = (cmd >> 4) & 1, nv = (cmd & 8) ? 4 : 3, tex = (cmd >> 2) & 1;
        n = 1 + nv * (1 + tex) + (gouraud ? nv - 1 : 0);
    } else if (cmd >= 0x40 && cmd < 0x60) { /* line / polyline (terminated by 0x5xxx5xxx) */
        int gouraud = (cmd >> 4) & 1;
        if (cmd & 8) {
            n = 1;
            while (n < avail && (w[n] & 0xF000F000) != 0x50005000) {
                n++;
            }
            n++;
        } else {
            n = gouraud ? 4 : 3;
        }
    } else if (cmd >= 0x60 && cmd < 0x80) { /* rectangle */
        n = 2 + ((cmd >> 2) & 1) + (((cmd >> 3) & 3) == 0);
    } else if (cmd == 0x02) { /* fill */
        n = 3;
    } else if (cmd >= 0x80 && cmd < 0xA0) { /* VRAM to VRAM copy */
        n = 4;
    } else if (cmd >= 0xA0 && cmd < 0xC0) { /* CPU to VRAM: header + data */
        n = 3;
        if (avail >= 3) {
            n += ((w[2] & 0xFFFF) * (w[2] >> 16) + 1) / 2;
        }
    } else if (cmd >= 0xC0 && cmd < 0xE0) { /* VRAM to CPU */
        n = 3;
    }
    return n < avail ? n : avail;
}

/* One packet: walked command by command; VRAM fills and copies go to the shadow, everything goes to the
 * renderer. */
static void draw_packet(u32 *p) {
    unsigned int len = ((P_TAG *)p)->len;
    u32 *w = p + 1;
    psx_gpu_packets++;
    psx_gpu_words += len;
    if (len == 4 && w[0] >> 24 == 0xE1 && w[1] >> 26 == 0x7C >> 2 && gp0_tpage_sprite16(w)) return;
    if (len == 5 && w[0] >> 24 == 0xE1 && !w[1] && w[2] >> 26 == 0x7C >> 2) { /* (MargePrim's: the sprite's tag is a NOP between) */
        const unsigned int t[4] = {w[0], w[2], w[3], w[4]};
        if (gp0_tpage_sprite16(t)) return;
    }
    for (unsigned int i = 0; i < len;) {
        i += gp0_run_plain(w + i, len - i); /* (primitives and drawing state: nearly all of them) */
        if (i >= len) break;
        u32 cmd = w[i] >> 24;
        unsigned int n = psx_gp0_len(w + i, len - i);
        if (cmd == 0x02 && n == 3) { /* fill rectangle */
            u32 c = w[i];
            if (psx_trace_gpu) {
                port_log("gpu: fill %d,%d %dx%d color %06X\n", (int)(w[i + 1] & 0x3F0), (int)((w[i + 1] >> 16) & 0x1FF),
                         (int)(((w[i + 2] & 0x3FF) + 15) & ~15), (int)((w[i + 2] >> 16) & 0x1FF), (unsigned)(c & 0xFFFFFF));
            }
            vram_fill(w[i + 1] & 0x3F0, (w[i + 1] >> 16) & 0x1FF, ((w[i + 2] & 0x3FF) + 15) & ~15,
                      (w[i + 2] >> 16) & 0x1FF, rgb15(c & 0xFF, (c >> 8) & 0xFF, (c >> 16) & 0xFF));
            gp0_run(w+i,n); /* hardware backends also submit the visible fill */
        } else if (cmd >= 0x80 && cmd < 0xA0 && n == 4) { /* VRAM to VRAM copy */
            int width=((w[i+3]-1)&0x3ff)+1;
            int height=(((w[i+3]>>16)-1)&0x1ff)+1;
            if (psx_trace_gpu) {
                port_log("gpu: copy %d,%d -> %d,%d %dx%d\n", (int)(w[i + 1] & 0x3FF), (int)((w[i + 1] >> 16) & 0x1FF),
                         (int)(w[i + 2] & 0x3FF), (int)((w[i + 2] >> 16) & 0x1FF), width, height);
            }
            vram_copy(w[i + 1] & 0x3FF, (w[i + 1] >> 16) & 0x1FF, w[i + 2] & 0x3FF, (w[i + 2] >> 16) & 0x1FF,
                      width, height);
        } else gp0_run(w+i,n);
        i += n;
    }
}

void DrawPrim(void *p) {
    draw_packet(p);
}

/* Port markers: native memory an OT may link into (registered by port code, port/game/field_bg.c), each packet there
 * handed to port_ot_marker instead of being drawn. Their links are 24-bit as the game's: the range shares the
 * ordering tables' high byte. */
const unsigned char *psx_ot_marker_lo, *psx_ot_marker_hi;
__attribute__((weak)) void port_ot_marker(unsigned int *m) { (void)m; }

/* A hardware renderer submits what an ordering table recorded (port/3ds/render.c). */
__attribute__((weak)) void psx_gpu_ot_done(void) {}

static void draw_ot(u32 *p, const void *native_buffer, unsigned int native_size) {
    unsigned int profile_start = port_profile_begin();
    /* Explicit bounds disambiguate stack links whose low 24 bits alias arena RAM. */
    u32 head_window = (u32)p & 0xFF000000;
    unsigned int guard = 0;
    /* ClearOTagR's empty entries each link to the word before: a run of them is stepped over by comparison
     * (both branches below would go to p - 1 for these) */
    const int ram_window = head_window == ((u32)psx_ram & 0xFF000000);
    /* (the markers' range, set before the walk: a marker is a packet of no words) */
    const uintptr_t marker_lo = (uintptr_t)psx_ot_marker_lo, marker_size = (uintptr_t)psx_ot_marker_hi - marker_lo;
    while (((u32)p & 0xFFFFFF) != OT_END) {
        if (ram_window && (unsigned char *)p > psx_ram && (unsigned char *)p < psx_ram + PSX_RAM_SIZE)
            while ((unsigned char *)p > psx_ram && *p == (((u32)(p - 1)) & 0xFFFFFF)) --p;
        if ((unsigned int)((unsigned int *)p - psx_main_ot) < psx_main_ot_n) psx_ot_slot = (unsigned int)((unsigned int *)p - psx_main_ot);
        u32 addr = *p & 0xFFFFFF;
        if (((P_TAG *)p)->len)
            draw_packet(p);
        else if ((uintptr_t)p - marker_lo < marker_size)
            port_ot_marker(p);
        if (addr == OT_END) {
            break;
        }
        u32 candidate = head_window | addr;
        if (native_size && candidate - (u32)native_buffer < native_size)
            p = (u32 *)candidate;
        else {
            p = link_ptr(addr);
            if (((unsigned char *)p < psx_ram || (unsigned char *)p >= psx_ram + PSX_RAM_SIZE) &&
                (uintptr_t)p - marker_lo >= marker_size)
                port_fatal("DrawOTag: link 0x%06X outside known buffers", addr);
        }
        if (++guard > 1000000) {
            port_fatal("DrawOTag: ordering table loops");
        }
    }
    if (drawsync_cb) {
        drawsync_owed = 1;
    }
    psx_gpu_ot_done();
    port_profile_detail_end(PORT_PROFILE_OT_WALK, profile_start);
}
void DrawOTag(u32 *p) { draw_ot(p, NULL, 0); }
void port_draw_ot_buffer(u32 *p, const void *buffer, unsigned int size) {
    draw_ot(p, buffer, size);
}

void DrawOTagEnv(u32 *p, DRAWENV *env) {
    PutDrawEnv(env);
    DrawOTag(p);
}

int DrawSyncCallback(void (*func)(void)) {
    void (*old)(void) = drawsync_cb;
    drawsync_cb = func;
    return (int)(u32)old;
}

/* Deliver an owed DrawSync callback (the GPU "finished"): at DrawSync and once per frame. */
void psx_drawsync_pending(void) {
    if (drawsync_owed && drawsync_cb) {
        drawsync_owed = 0;
        drawsync_cb();
    }
}

int DrawSync(int mode) {
    if (mode == 0) {
        psx_drawsync_pending();
    }
    return 0;
}

s32 IsIdleGPU(s32 max) {
    return 0;
}

s32 BreakDraw(void) {
    /* DrawOTag finishes synchronously, so there is no outstanding DMA chain.
     * Zero means idle; -1 would mean DMA is busy in a non-list transfer. */
    return 0;
}

/* The platform may draw the list the game inserts at VBlank less often than every VBlank (DS: display.c). */
__attribute__((weak)) int psx_skip_vblank_list(void) { return 0; }
/* ... or draw it itself (the battle UI's native renderer: port/game/btlui_native.c); 1: drawn */
__attribute__((weak)) int psx_vblank_list_native(u32 *list) { (void)list; return 0; }

void ContinueDraw(u32 *insaddr, s32 contaddr) {
    /* Even when BreakDraw returns zero, the inserted list must execute. FF9
     * uses this at VBlank for battle commands, status bars and text. */
    if (!contaddr && psx_skip_vblank_list()) return;
    if (insaddr && !(!contaddr && psx_vblank_list_native(insaddr))) DrawOTag(insaddr);
    if (contaddr && ((u32)contaddr & 0xFFFFFF) != OT_END)
        DrawOTag(link_ptr((u32)contaddr & 0xFFFFFF));
}
