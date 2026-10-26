/* Full-screen movies from FastVideoDS transcodes (movie_fv.h, docs/movie-validation.md).
 *
 * The decoder is a blob (fv.bin in the data folder: tools/fv_blob.py, built from an external FastVideoDS checkout
 * that is not part of this repository), loaded into the movie player's VLC buffers and relocated there. Its
 * transcodes are 256x176, one .fv per STR (tools/fmv_transcode.py), video only: the STR still streams for its
 * audio.
 *
 * VRAM while a movie plays: banks A-C hold decoded frames and rotate as the one shown (main BG), the motion
 * compensation's reference (texture slot 0) and the target (LCD: the 3D engine's prediction is captured into it, the
 * residual added). The residual takes bank D and the start of E (192 bytes an 8x8 block, 132 KB); the display
 * list lives in E after it. MPU region 2 (normally the null-pointer guard at 0) makes that VRAM cacheable, as the
 * decoder needs to be fast.
 *
 * A bank can't be the reference and shown at once, so frame n shows once n+1 is decoded: while the player shows n,
 * up to n+2 decodes. A P-frame goes through: its vectors, which fill the display list's texcoords, and the list's
 * triangles sent to the geometry engine (start_frame); once its reference is complete, the swap (swap_frame), at
 * the next VBlank, and the capture of the prediction during the frame after (the VBlank interrupt steps through
 * these); then its residual added by the CPU (finish_frame). The texture is only read when the engine renders,
 * after the swap, so the next frame's vectors and triangles go in while the current one is captured and finished.
 * All of it runs from the player's frame and between the game's own work (movie_fv_idle). */
#include <nds.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "movie_fv.h"
#include "native_mesh.h"
#include "platform.h"
#include "psx_runtime.h"
#include "profile.h"

int psx_cd_disc(void);

#define MOVIE_W 256
#define MOVIE_H 176
#define BANK_LCD(b) ((u16 *)(0x06800000 + 0x20000 * (b)))
#define RESIDUAL ((u8 *)0x06860000)          /* bank D, on into E */
#define LIST ((u32 *)0x06884000)             /* bank E, after the residual's 138 KB */
#define LIST_MAX (0x06890000 - 0x06884000)
#define MAX_KEYS 512
/* (what only a playing movie runs goes in the player's overlay, ovl_64: the resident part is the VBlank, the idle
 * step and stopping) */
#define MOVIE_CODE PORT_OVERLAY_CODE(64)
static inline int player_resident(void) { return psx_ovl_resident[2] == 0x64; }
/* a VBlank waited for by the movie itself (profiled apart from the game's) */
static void wait_vblank(void) {
    unsigned int t = port_profile_begin();
    swiWaitForVBlank();
    port_profile_detail_end(PORT_PROFILE_MOVIE_FV_WAIT, t);
}

/* fv.bin (tools/fv_blob.py) */
typedef struct {
    char magic[4];
    u32 image, list, prefix, nrel, nlist;
} BlobHeader;
/* the blob's entry table (port/ds/fv/fv_blob_head.s) */
typedef struct {
    u32 magic;
    void (*init)(void *dec);
    void (*decode_i)(void *dec, const u16 *src, u8 *dst);
    const u16 *(*vectors)(void *dec, const u16 *src);
    void (*dcts)(void *dec, const u16 *src, u8 *residual);
    void (*finish)(void *dec, const u8 *residual, const u16 *prediction);
    u32 *list;   /* the display list's fixed head */
    u32 *list_a; /* its block part, which this file builds */
} Entry;
/* the decoder's state, as its asm reads it: 64 dequantization words, table pointers and constants, then these */
typedef struct {
    u32 tables[0x124 / 4];
    u32 y_blocks;
    u32 tex_y_offset;
    const u32 *list_b;
    u32 p_is_i[24];
} Decoder;
_Static_assert(offsetof(Decoder, list_b) == 0x12C, "decoder layout");
/* a .fv file */
typedef struct {
    u32 signature;
    u16 width, height;
    u32 fps_num, fps_den;
    u16 audio_rate, audio_channels;
    u32 frames, keys;
} FvHeader;
typedef struct {
    u32 frame, offset;
} FvKey;

static vu8 *const vram_cr = (vu8 *)0x04000240;

static int active;
static const Entry *fv;
static Decoder *dec;
static FvKey *keys;
static u8 *data;
static u32 data_max, list_bytes;
static FILE *file;
static FvHeader header;
static int file_next; /* the frame the file is positioned at */

/* banks 0-2: the frame each holds; shown (main BG) and the reference (the latest decoded) */
static int bank_frame[3];
static volatile int shown = -1, flip_to = -1;
static int ref = -1, latest = -1;
/* the P-frame in the 3D engine: its bank, frame, and where its residual data starts */
enum { IDLE, LISTED, CAPTURING, CAPTURED };
static volatile int phase;
static int target = -1, target_frame;
static const u16 *target_rest;
static volatile unsigned int vblanks_since_frame;
static u8 saved_cr[5];
/* (for post-mortems: frames shown, P-frames decoded, VBlanks while playing, jumps, I-frames, the last frame asked
 * for) */
unsigned int movie_fv_stats[6];
/* (post-mortems: waits in advance, in finish_frame, for an I-frame's flip; then latest - frame + 4 at each call) */
unsigned int movie_fv_waits[3], movie_fv_ahead[8];
static u16 saved_bldcnt;

/* ---- MPU region 2 over LCD VRAM (A-I at 0x06800000, 1 MB): cacheable, write-buffered, read/write ------------- */

static u32 saved_region2, saved_dperm, saved_iperm;
__attribute__((target("arm"), noinline)) static void vram_cache(int on) {
    u32 r;
    DC_FlushAll(); /* (whatever was cached through it goes to VRAM before the region changes) */
    if (on) {
        __asm__ volatile("mrc p15, 0, %0, c6, c2, 0" : "=r"(saved_region2));
        __asm__ volatile("mrc p15, 0, %0, c5, c0, 2" : "=r"(saved_dperm));
        __asm__ volatile("mrc p15, 0, %0, c5, c0, 3" : "=r"(saved_iperm));
        r = 0x06800000u | 19u << 1 | 1u; /* 2^(19+1) bytes */
        __asm__ volatile("mcr p15, 0, %0, c6, c2, 0" : : "r"(r));
        r = (saved_dperm & ~0xF00u) | 0x300u; /* privileged and user read/write */
        __asm__ volatile("mcr p15, 0, %0, c5, c0, 2" : : "r"(r));
        r = saved_iperm & ~0xF00u; /* no instructions there */
        __asm__ volatile("mcr p15, 0, %0, c5, c0, 3" : : "r"(r));
        __asm__ volatile("mrc p15, 0, %0, c2, c0, 0" : "=r"(r));
        r |= 1 << 2;
        __asm__ volatile("mcr p15, 0, %0, c2, c0, 0" : : "r"(r));
        __asm__ volatile("mrc p15, 0, %0, c3, c0, 0" : "=r"(r));
        r |= 1 << 2;
        __asm__ volatile("mcr p15, 0, %0, c3, c0, 0" : : "r"(r));
    } else {
        __asm__ volatile("mrc p15, 0, %0, c2, c0, 0" : "=r"(r));
        r &= ~(1u << 2);
        __asm__ volatile("mcr p15, 0, %0, c2, c0, 0" : : "r"(r));
        __asm__ volatile("mrc p15, 0, %0, c3, c0, 0" : "=r"(r));
        r &= ~(1u << 2);
        __asm__ volatile("mcr p15, 0, %0, c3, c0, 0" : : "r"(r));
        __asm__ volatile("mcr p15, 0, %0, c6, c2, 0" : : "r"(saved_region2));
        __asm__ volatile("mcr p15, 0, %0, c5, c0, 2" : : "r"(saved_dperm));
        __asm__ volatile("mcr p15, 0, %0, c5, c0, 3" : : "r"(saved_iperm));
    }
}

/* ---- setup ------------------------------------------------------------------------------------------------ */

#define BLOCK_VTX(x, y, z) (((x) & 0x3FF) | ((((y) >> 3) & 0x3FF) << 10) | ((z) << 20))

/* the motion compensation's triangles, one per 8x8 block: a texcoord the vector decoder fills, then three VTX_10.
 * The bottom half samples the reference through a texture that starts above it, so that vectors past the bottom
 * clamp to the frame's last row, as they clamp to its first at the top. */
MOVIE_CODE static u32 *block_list(u32 *dl, u32 tex_param) {
    int y = 0;
    for (; y < MOVIE_H / 16; y++)
        for (int x = 0; x < MOVIE_W / 8; x++) {
            *dl++ = FIFO_COMMAND_PACK(FIFO_TEX_COORD, FIFO_VERTEX10, FIFO_VERTEX10, FIFO_VERTEX10);
            *dl++ = 0;
            *dl++ = BLOCK_VTX(x * 8, y * 8 - 8, 33 - x);
            *dl++ = BLOCK_VTX(x * 8, y * 8 + 8, 32 - x);
            *dl++ = BLOCK_VTX(x * 8 + 16, y * 8 + 8, 32 - x);
        }
    *dl++ = FIFO_COMMAND_PACK(FIFO_END, FIFO_TEX_FORMAT, FIFO_BEGIN, FIFO_NOP);
    *dl++ = tex_param;
    *dl++ = GL_TRIANGLES;
    for (; y < MOVIE_H / 8; y++)
        for (int x = 0; x < MOVIE_W / 8; x++) {
            *dl++ = FIFO_COMMAND_PACK(FIFO_TEX_COORD, FIFO_VERTEX10, FIFO_VERTEX10, FIFO_VERTEX10);
            *dl++ = 0;
            *dl++ = BLOCK_VTX(x * 8, y * 8 - 8, 33 - x);
            *dl++ = BLOCK_VTX(x * 8, y * 8 + 8, 32 - x);
            *dl++ = BLOCK_VTX(x * 8 + 16, y * 8 + 8, 32 - x);
        }
    return dl;
}

/* screen pixels: x in 1/64ths, y in 1/8ths (the vertices' units); texcoords from the vertices (texture matrix) */
MOVIE_CODE static void setup_3d(void) {
    powerOn(POWER_3D_CORE | POWER_MATRIX);
    while (GFX_BUSY) {}
    GFX_STATUS |= 1 << 29;
    glResetMatrixStack();
    glFlush(1);
    GFX_CONTROL = 0x9;
    glClearColor(0, 0, 0, 31);
    glClearPolyID(63);
    glClearDepth(0x7FFF);
    glFlush(0);
    glMatrixMode(GL_PROJECTION);
    MATRIX_LOAD4x3 = divf32(inttof32(2), 256 << 6); MATRIX_LOAD4x3 = 0; MATRIX_LOAD4x3 = 0;
    MATRIX_LOAD4x3 = 0; MATRIX_LOAD4x3 = -21845; MATRIX_LOAD4x3 = 0;
    MATRIX_LOAD4x3 = 0; MATRIX_LOAD4x3 = 0; MATRIX_LOAD4x3 = 4096 >> 5;
    MATRIX_LOAD4x3 = -inttof32(1); MATRIX_LOAD4x3 = inttof32(1); MATRIX_LOAD4x3 = 0;
    glMatrixMode(GL_TEXTURE);
    MATRIX_LOAD4x3 = 4096 * 64 * 16; MATRIX_LOAD4x3 = 0; MATRIX_LOAD4x3 = 0;
    MATRIX_LOAD4x3 = 0; MATRIX_LOAD4x3 = 4096 * 64 * 8 * 16; MATRIX_LOAD4x3 = 0;
    MATRIX_LOAD4x3 = 0; MATRIX_LOAD4x3 = 0; MATRIX_LOAD4x3 = 0;
    MATRIX_LOAD4x3 = 0; MATRIX_LOAD4x3 = 0; MATRIX_LOAD4x3 = 0;
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glViewport(0, 0, 255, 191);
}

/* Load and relocate the blob at `work`; returns its size, 0 if there is none. */
MOVIE_CODE static u32 load_blob(u8 *work, u32 size) {
    char path[64];
    snprintf(path, sizeof path, "%s/fv.bin", plat_data_dir());
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    BlobHeader h;
    u32 image = 0;
    if (fread(&h, sizeof h, 1, f) == 1 && !memcmp(h.magic, "FVBL", 4) && h.list <= LIST_MAX &&
        h.image + (h.nrel + h.nlist) * 4 <= size) {
        /* (the relocations and the list's head read in at the end, where frames go later; the head copied to VRAM
         * by words: VRAM ignores byte writes) */
        u32 *rel = (u32 *)(work + size) - h.nrel - h.nlist;
        u32 *head = rel - (h.prefix + 3) / 4;
        if ((u8 *)head >= work + h.image && fread(rel, 4, h.nrel + h.nlist, f) == h.nrel + h.nlist &&
            fread(work, 1, h.image, f) == h.image && fread(head, 1, h.prefix, f) == h.prefix) {
            for (u32 i = 0; i < (h.prefix + 3) / 4; i++) LIST[i] = head[i];
            for (u32 i = 0; i < h.nrel + h.nlist; i++) {
                if (rel[i] > h.image - 4) break;
                u32 *w = (u32 *)(work + rel[i]);
                *w += i < h.nrel ? (u32)work : (u32)LIST - 0x10000000u;
            }
            image = h.image;
            list_bytes = h.list;
        }
    }
    fclose(f);
    if (!image) return 0;
    DC_FlushRange(work, image);
    IC_InvalidateRange(work, image);
    fv = (const Entry *)work;
    return fv->magic == 0x31425646 ? image : 0;
}

/* ---- frames ----------------------------------------------------------------------------------------------- */

/* Frames' data: two halves, so that one frame's residual data waits while the next frame's vectors are read; a frame
 * larger than a half takes the whole buffer when nothing else is pending. */
static u8 *data_half[2];
static u32 data_half_max;

/* Read frame `n` (its data into the half `n` picks); NULL at the end, on a read error, or (with the file left at
 * the frame) when it needs the whole buffer and another frame's data is still pending. */
MOVIE_CODE static const u16 *read_frame(int n, int pending) {
    if (n < 0 || (u32)n >= header.frames) return NULL;
    if (n < file_next || n > file_next + 30) { /* back, or far ahead: from the last key frame at or before it */
        u32 k = 0;
        for (u32 i = 0; i < header.keys && i < MAX_KEYS; i++)
            if ((int)keys[i].frame <= n) k = i;
        if (fseek(file, (long)keys[k].offset, SEEK_SET)) return NULL;
        file_next = (int)keys[k].frame;
    }
    for (;;) {
        u32 len;
        if (fread(&len, 4, 1, file) != 1) return NULL;
        u32 bytes = len & 0x1FFFF;
        if (bytes > data_max) return NULL;
        if (file_next == n) {
            u8 *dst = bytes <= data_half_max ? data_half[n & 1] : pending ? NULL : data;
            if (!dst) {
                fseek(file, -4, SEEK_CUR);
                return NULL;
            }
            file_next++;
            return fread(dst, 1, bytes, file) == bytes ? (const u16 *)dst : NULL;
        }
        file_next++;
        if (fseek(file, (long)bytes, SEEK_CUR)) return NULL;
    }
}

static int show_want = -1; /* the frame the player shows */
/* the P-frame whose triangles are in the geometry engine, held back from the swap until its reference is complete */
static int geo_frame = -1;
static const u16 *geo_rest;

/* A bank for the next frame: not the reference, not about to be shown; the bank shown is fine for a P-frame once a
 * flip away from it is queued (the flip comes at the next VBlank, the capture after). (show() runs first, so a frame
 * still to show is about to be.) */
MOVIE_CODE static int free_bank(int p) {
    for (int b = 0; b < 3; b++) {
        if (b == ref || b == flip_to) continue;
        if (b == shown && !(p && flip_to >= 0)) continue;
        return b;
    }
    return -1;
}

/* the frame whose residual is decoded into the residual buffer, waiting for its prediction (-1: the buffer's free) */
static int res_frame = -1;

/* Decode frame k's residual (the coefficients after its vectors) into the residual buffer: it needs neither the
 * prediction nor the reference, so it runs while the 3D engine swaps and captures. */
MOVIE_CODE static void residual(int k, const u16 *rest) {
    unsigned int t = port_profile_begin();
    fv->dcts(dec, rest, RESIDUAL);
    res_frame = k;
    port_profile_detail_end(PORT_PROFILE_MOVIE_MDEC, t); /* (the residual decoding alone) */
    port_profile_detail_end(PORT_PROFILE_MOVIE_FV, t);
}

/* The P-frame captured: add its residual; it becomes the reference. */
MOVIE_CODE static void finish_frame(void) {
    while (phase != CAPTURED) {
        movie_fv_waits[1]++;
        wait_vblank();
    }
    if (res_frame != target_frame) residual(target_frame, target_rest);
    unsigned int t = port_profile_begin();
    DC_FlushAll(); /* (the capture wrote the prediction behind the cache) */
    fv->finish(dec, RESIDUAL, BANK_LCD(target));
    res_frame = -1;
    movie_fv_stats[1]++;
    DC_FlushAll();
    port_profile_detail_end(PORT_PROFILE_MOVIE_FV, t);
    bank_frame[target] = target_frame;
    if (ref >= 0) vram_cr[ref] = 0x80;
    ref = target;
    latest = target_frame;
    target = -1;
    phase = IDLE;
}

/* The held P-frame's swap, once its reference is complete: the reference as the texture, a bank to capture into. */
MOVIE_CODE static int swap_frame(void) {
    if (geo_frame < 0 || target >= 0 || latest != geo_frame - 1 || ref < 0 || (DMA_CR(1) & DMA_BUSY)) return 0;
    int b = free_bank(1);
    if (b < 0) return 0;
    vram_cr[ref] = 0x83; /* texture slot 0 */
    if (b != shown) vram_cr[b] = 0x80;
    target = b;
    target_frame = geo_frame;
    target_rest = geo_rest;
    geo_frame = -1;
    GFX_FLUSH = 0; /* the swap at the next VBlank; the capture at the first VBlank that finds the engine idle */
    phase = LISTED;
    return 1;
}

/* Frame n, the next after everything decoded or under way: an I-frame decoded at once (when nothing else is under
 * way), a P-frame's vectors decoded and its triangles sent to the geometry engine, without the swap. */
MOVIE_CODE static int start_frame(int n) {
    if (geo_frame >= 0 || phase == LISTED) return 0; /* (one batch of triangles at a time; the busy flag tells the swap) */
    int pending = target >= 0;
    unsigned int t = port_profile_begin();
    const u16 *src = read_frame(n, pending);
    port_profile_detail_end(PORT_PROFILE_MOVIE_FV_READ, t);
    if (!src) return 0;
    t = port_profile_begin();
    int ok = 0;
    if (*src >> 15) {
        if (ref >= 0 || pending) {
            unsigned int ta = port_profile_begin();
            geo_rest = fv->vectors(dec, src);
            port_profile_detail_end(PORT_PROFILE_MOVIE_VLC, ta); /* (the vectors alone) */
            DC_FlushRange(fv->list, list_bytes);
            DMA_SRC(1) = (u32)fv->list;
            DMA_DEST(1) = (u32)&GFX_FIFO;
            DMA_CR(1) = DMA_FIFO | (list_bytes >> 2);
            geo_frame = n;
            ok = 1;
        }
    } else if (!pending) {
        while (flip_to >= 0) { /* (written by the CPU: off screen first) */
            movie_fv_waits[2]++;
            wait_vblank();
        }
        int b = free_bank(0);
        if (b >= 0) {
            vram_cr[b] = 0x80;
            fv->decode_i(dec, src, (u8 *)BANK_LCD(b));
            movie_fv_stats[4]++;
            DC_FlushAll();
            bank_frame[b] = n;
            if (ref >= 0) vram_cr[ref] = 0x80;
            ref = b;
            latest = n;
            ok = 1;
        }
    }
    if (!ok) file_next = 1 << 30; /* (read again later: from its key frame) */
    port_profile_detail_end(PORT_PROFILE_MOVIE_FV, t);
    return ok;
}

/* Queue for the next VBlank the newest decoded frame up to n that can be shown: not the reference (unless it's no
 * longer needed: the movie's last frame), and newer than the one shown. */
MOVIE_CODE static void show(int n) {
    if (flip_to >= 0) return; /* (the next VBlank's flip first) */
    int best = -1;
    for (int b = 0; b < 3; b++)
        if (bank_frame[b] >= 0 && bank_frame[b] <= n && b != shown &&
            (shown < 0 || bank_frame[b] > bank_frame[shown]) &&
            (b != ref || (target < 0 && geo_frame < 0 && bank_frame[b] == (int)header.frames - 1)) &&
            (best < 0 || bank_frame[b] > bank_frame[best]))
            best = b;
    if (best < 0) return;
    if (best == ref) vram_cr[best] = 0x80;
    flip_to = best;
}

/* Whatever can be done now: finish the captured P-frame, swap the next one in, show the player's frame, send the
 * triangles of the one after (up to two ahead of the one shown). Waits for the 3D engine only if `block` and the
 * frame to show isn't decoded at all: one decoded but still the reference shows a frame late rather than stopping
 * the game, whose work (sound above all) fills the time the 3D engine takes. */
MOVIE_CODE static void advance(int block) {
    int last = (int)header.frames - 1;
    for (;;) {
        int progress = 0;
        if (target >= 0 && (phase == CAPTURED || (block && latest < show_want && phase != LISTED))) {
            finish_frame();
            progress = 1;
        }
        show(show_want);
        progress |= swap_frame();
        /* (the residual of the frame to finish next, while the engine works on it) */
        if (res_frame < 0 && (target >= 0 || geo_frame >= 0)) {
            if (target >= 0) residual(target_frame, target_rest);
            else residual(geo_frame, geo_rest);
            progress = 1;
        }
        int next = (geo_frame >= 0 ? geo_frame : target >= 0 ? target_frame : latest) + 1;
        int want = show_want + 2 < last ? show_want + 2 : last;
        if (next <= want) progress |= start_frame(next);
        if (!progress) {
            /* (blocking: the frame to show isn't decoded; the 3D engine's swap and capture come at VBlanks) */
            if (!block || latest >= show_want || (target < 0 && geo_frame < 0)) return;
            movie_fv_waits[0]++;
            wait_vblank();
        }
    }
}

void movie_fv_vblank(void) {
    if (!active) return;
    vblanks_since_frame++;
    movie_fv_stats[2]++;
    if (flip_to >= 0) {
        if (shown >= 0 && shown != flip_to) vram_cr[shown] = 0x80;
        vram_cr[flip_to] = 0x81; /* main BG, 0x06000000 */
        shown = flip_to;
        flip_to = -1;
    }
    if (phase == LISTED && !(GFX_STATUS & (1 << 27))) {
        REG_DISPCAPCNT = DCAP_ENABLE | DCAP_MODE(DCAP_MODE_A) | DCAP_BANK(target) | DCAP_SIZE(DCAP_SIZE_256x192) |
                         DCAP_SRC_A(DCAP_SRC_A_3DONLY);
        phase = CAPTURING;
    } else if (phase == CAPTURING && !(REG_DISPCAPCNT & DCAP_ENABLE)) {
        phase = CAPTURED;
    }
}

void movie_fv_idle(void) {
    if (!active) return;
    if (player_resident()) advance(0);
    if (vblanks_since_frame > 180) movie_fv_stop(); /* the player stopped without saying so */
}

/* (everything under way finished or dropped: for a jump) */
MOVIE_CODE static void settle(void) {
    while (DMA_CR(1) & DMA_BUSY) {}
    if (geo_frame >= 0) { /* (its triangles swapped in and dropped) */
        GFX_FLUSH = 0;
        geo_frame = -1;
        if (res_frame >= 0 && target < 0) res_frame = -1;
    }
    if (target >= 0) {
        while (phase == LISTED) wait_vblank();
        finish_frame();
    }
}

MOVIE_CODE void movie_fv_frame(unsigned int frame) {
    if (!active) return;
    vblanks_since_frame = 0;
    int n = (int)frame - 1;
    movie_fv_stats[0]++;
    movie_fv_stats[5] = frame;
    int ahead = geo_frame >= 0 ? geo_frame : target >= 0 ? target_frame : latest;
    if (ahead > n + 2 || n > latest + 16) { /* a jump (back, or far ahead): decode again from its key frame */
        settle();
        movie_fv_stats[3]++;
        u32 k = 0;
        for (u32 i = 0; i < header.keys && i < MAX_KEYS; i++)
            if ((int)keys[i].frame <= n) k = i;
        if (ref >= 0) vram_cr[ref] = 0x80;
        ref = -1;
        latest = (int)keys[k].frame - 1;
        for (int b = 0; b < 3; b++)
            if (b != shown) bank_frame[b] = -1;
    }
    int d = latest - n + 4;
    movie_fv_ahead[d < 0 ? 0 : d > 7 ? 7 : d]++;
    show_want = n;
    advance(1);
}

int movie_fv_active(void) {
    return active;
}

MOVIE_CODE int movie_fv_start(void *work, unsigned int size, unsigned int lba) {
    if (active) movie_fv_stop();
    char path[64];
    snprintf(path, sizeof path, "%s/fmv/d%d/%u.fv", plat_data_dir(), psx_cd_disc(), lba);
    file = fopen(path, "rb");
    if (!file) return 0;
    if (fread(&header, sizeof header, 1, file) != 1 || header.signature != 0x53445646 || header.width != MOVIE_W ||
        header.height != MOVIE_H || !header.keys) {
        fclose(file);
        file = NULL;
        return 0;
    }
    display_movie_take();
    for (int b = 0; b < 5; b++) saved_cr[b] = vram_cr[b];
    saved_bldcnt = REG_BLDCNT;
    vram_cr[3] = 0x80; /* D and E as LCD: the residual and the display list */
    vram_cr[4] = 0x80;
    u8 *w = (u8 *)(((u32)work + 31) & ~31u);
    size -= (u32)(w - (u8 *)work);
    u32 image = load_blob(w, size);
    u32 nkeys = header.keys < MAX_KEYS ? header.keys : MAX_KEYS;
    u32 used = (image + 31) & ~31u;
    if (!image || used + sizeof(Decoder) + nkeys * sizeof(FvKey) + 72 * 1024 > size ||
        fread(w + used + sizeof(Decoder), sizeof(FvKey), nkeys, file) != nkeys) {
        port_log("movie: no FastVideoDS decoder for %s\n", path);
        fclose(file);
        file = NULL;
        for (int b = 0; b < 5; b++) vram_cr[b] = saved_cr[b];
        display_movie_release();
        return 0;
    }
    dec = (Decoder *)(w + used);
    keys = (FvKey *)(w + used + sizeof(Decoder));
    data = (u8 *)(((u32)(keys + nkeys) + 31) & ~31u);
    data_max = (u32)(w + size - data);
    data_half_max = (data_max / 2) & ~31u;
    data_half[0] = data;
    data_half[1] = data + data_half_max;
    file_next = 1 << 30;
    fv->init(dec);
    dec->y_blocks = MOVIE_H / 8;
    dec->tex_y_offset = (256 - MOVIE_H) << 1;
    u32 tex_param = 0xDED00000 + (((-(256 - MOVIE_H) * 256 * 2) >> 3) & 0xFFFF);
    u32 *dl = block_list(fv->list_a, tex_param);
    *dl++ = FIFO_COMMAND_PACK(FIFO_END, FIFO_POLY_FORMAT, FIFO_TEX_FORMAT, FIFO_BEGIN);
    *dl++ = 0x0F4080;
    *dl++ = 0xDED00000;
    *dl++ = GL_TRIANGLES;
    dec->list_b = dl + 1;
    dl = block_list(dl, tex_param);
    *dl++ = FIFO_COMMAND_PACK(FIFO_END, FIFO_NOP, FIFO_NOP, FIFO_NOP); /* (the swap apart: swap_frame) */
    list_bytes = (u32)dl - (u32)fv->list;

    for (int b = 0; b < 3; b++) {
        vram_cr[b] = 0x80;
        memset(BANK_LCD(b), 0, 0x20000);
        bank_frame[b] = -1;
    }
    shown = flip_to = ref = latest = target = show_want = geo_frame = res_frame = -1;
    phase = IDLE;
    vram_cache(1);
    setup_3d();
    videoSetMode(MODE_5_2D | DISPLAY_BG2_ACTIVE);
    REG_BLDCNT = 0;
    REG_BG2CNT = (u16)BgSize_B16_256x256;
    REG_BG2PA = 256; REG_BG2PB = 0; REG_BG2PC = 0; REG_BG2PD = 256;
    REG_BG2X = 0;
    REG_BG2Y = -(((192 - MOVIE_H) >> 1) << 8);
    vblanks_since_frame = 0;
    active = 1;
    port_log("movie: %s, %u frames\n", path, (unsigned int)header.frames);
    return 1;
}

void movie_fv_stop(void) {
    if (!active) return;
    while (DMA_CR(1) & DMA_BUSY) {}
    if (geo_frame >= 0) {
        GFX_FLUSH = 0;
        geo_frame = -1;
    }
    if (target >= 0) {
        while (phase != CAPTURED) swiWaitForVBlank();
        target = -1;
    }
    while (flip_to >= 0) swiWaitForVBlank();
    active = 0;
    phase = IDLE;
    REG_DISPCAPCNT = 0;
    vram_cache(0);
    fclose(file);
    file = NULL;
    for (int b = 0; b < 5; b++) vram_cr[b] = saved_cr[b];
    REG_BLDCNT = saved_bldcnt;
    display_movie_release();
}
