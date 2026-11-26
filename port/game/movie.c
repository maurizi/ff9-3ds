/* Native versions of the MBG / FMV player's hand-written code (decomp ovl_64), under the original C player.
 *
 * The player itself (MBG_init, mbg_play, mbg_update, the sector ring, camera and mask bookkeeping, end
 * conditions) is the decomp's C and runs unchanged. What remains in asm there is supplied here:
 *  - mbg_vlcBuildTable / mbg_vlcDecode: FF9's BS v2 expansion, by port/psx/movie_vlc.c from the original
 *    run/level pairs (g_mbgVlcPairs). The lookup table the original builds is not needed.
 *  - mbg_applyMask: sets the mask bit on the MBG pixels the field draws behind, column by column.
 *  - libpress (DecDCTReset ... DecDCToutCallback): the MDEC, by port/psx/mdec_decode.c.
 * See docs/movie-validation.md.
 *
 * On the DS a movie in its 24-bit (full-screen) mode plays from its FastVideoDS transcode when there is one
 * (movie_fv.h): its frames skip the VLC and the MDEC, the player's slice callbacks still run (their uploads dropped),
 * and the transcode's frame is shown instead. The decoder takes the player's two VLC buffers.
 *
 * The MDEC decodes synchronously. DecDCTout completes before it returns and runs the out callback, which queues
 * the next slice with another DecDCTout; that request is run by the outer call's loop rather than by recursion, so
 * the player's slice-by-slice LoadImage chain keeps its original order. DecDCTinSync(1) reports the command busy
 * bit while macroblocks remain; trailing FE00 padding does not keep it busy. An out request the input cannot fill
 * stays busy without a callback, as a stalled DMA would.
 */
#define M2CTX
#include "ovl_64.h"
#include "psx_mem.h"
#include "mdec_decode.h"
#include "movie_vlc.h"
#include "profile.h"
#include "movie_fv.h"

extern int psx_load_image_skip;

__attribute__((weak)) int plat_async(void (*fn)(void *), void *arg) { (void)fn; (void)arg; return 0; }
__attribute__((weak)) void plat_async_wait(void) {}
__attribute__((weak)) int plat_async_busy(void) { return 0; }
static void ahead_start(u32 *out);
static int ahead_vlc_start(const u8 *bs, u32 bytes, u32 *out);
static void vlc_flush(void);
static void vlc_report(int r) {
    if (r != FF9_VLC_OK) {
        static int reported;
        if (!reported++) {
            port_log("movie: VLC error %d at frame %d\n", r, g_mbgFrame);
        }
    }
}

/* ---- transcoded frames -------------------------------------------------------------------------------------- */

/* per VLC buffer: the STR frame it stands for when the transcode plays it (0: VLC data) */
static u32 fv_frame_of[2];
static int fv_unavailable; /* this movie has no transcode (or decoder) */
static int fv_now;         /* the frame being decoded comes from the transcode */
static int fv_logged;      /* (the movie's start reported) */
static int fv_mbg_unavailable; /* this movie has no transcode to play behind the field */
static int fv_mbg;         /* the frame being decoded is a movie behind the field's (slices filled from the transcode) */
static int fv_col;         /* its next slice's first column */
static unsigned int fv_stp; /* its pixels' bit 15 (DecDCTin's mode) */

static int in_buffer(const void *p) {
    return (const u8 *)PSX_PTR(p) == (const u8 *)PSX_PTR(g_mbgPlayer.mdecIn1);
}

/* The STR frame number at `bs` (in its ring slot): the sector header the slot keeps. */
static u32 slot_frame(const u8 *bs) {
    u8 *base = g_mbgStream.ringData;
    u32 slot_bytes = g_mbgStream.slotSectors * 0x8F4;
    if (!slot_bytes || bs < base) return 0;
    u32 slot = (u32)(bs - base) / slot_bytes;
    if (slot >= g_mbgStream.slotCount) return 0;
    const u8 *hdr = (const u8 *)PSX_PTR(g_mbgStream.ringHdrs) + slot * 0x20;
    return *(const u32 *)(hdr + 8);
}

/* Whether frame `bs`, about to be expanded into `out`, plays from the transcode. */
static int fv_take(u8 *bs, u32 *out) {
    u8 *in0 = PSX_PTR(g_mbgPlayer.mdecIn0), *in1 = PSX_PTR(g_mbgPlayer.mdecIn1);
    int idx = in_buffer(out);
    if (!fv_logged++)
        port_log("movie: LBA %d, %s\n", (int)g_mbgStream.baseSector, g_mbgRgb24 == 1 ? "24-bit" : "15-bit");
    fv_frame_of[idx] = 0;
    if (in1 != in0 + 0x10000) {
        movie_fv_stop();
        movie_fv_mbg_stop();
        return 0;
    }
    if (g_mbgRgb24 != 1) { /* behind the field: the CPU decodes it into the MDEC's slices */
        movie_fv_stop();
        if (!movie_fv_mbg_active() && !fv_mbg_unavailable &&
            !movie_fv_mbg_start(in0, 0x20000, (unsigned int)g_mbgStream.baseSector))
            fv_mbg_unavailable = 1; /* (-1, not yet: tried again at the next frame) */
        if (!movie_fv_mbg_active()) return 0;
        fv_frame_of[idx] = slot_frame(bs);
        return fv_frame_of[idx] != 0;
    }
    movie_fv_mbg_stop();
    if (!movie_fv_active() && !fv_unavailable &&
        !movie_fv_start(in0, 0x20000, (unsigned int)g_mbgStream.baseSector)) {
        fv_unavailable = 1;
    }
    if (!movie_fv_active()) return 0;
    fv_frame_of[idx] = slot_frame(bs);
    return fv_frame_of[idx] != 0;
}

/* ---- stream ------------------------------------------------------------------------------------------------- */

/* mbg_streamGetFrame, as the decomp's (ovl_64/mbg_stream.c), except that looking for a frame not there yet is a
 * poll of the VBlank counter. The player looks 1000 times before it seeks the stream again (dropping the ring); on
 * the PS1 the drive goes on delivering meanwhile, here virtual time only passes at frames, which polls advance
 * (VSync(-1)). Without them the player spun through its 1000 looks with the drive standing still, then seeked. */
s32 o64_mbg_streamGetFrame(u8 **data, Ovl64RingHdr **hdr, Ovl64FrameInfo **arg2, Ovl64Pair **arg3, MbgRumble **arg4) {
    Ovl64RingHdr *h = &g_mbgStream.ringHdrs[g_mbgStream.readSlot];

    if (!(h->status & 0x8000)) {
        if (g_mbgStream.endState == 0) {
            VSync(-1);
            return 3;
        }
        return 5;
    }
    *hdr = h;
    *data = g_mbgStream.ringData + (h->firstSector + g_mbgStream.readSlot * g_mbgStream.slotSectors) * 0x8F4;
    if (g_mbgStream.hasCamera != 0) {
        *arg2 = &g_mbgSlotCameras[g_mbgStream.readSlot];
        *arg3 = &g_mbgSlotMasks[g_mbgStream.readSlot];
    } else {
        *arg2 = NULL;
        *arg3 = NULL;
    }
    *arg4 = &g_mbgSlotRumble[g_mbgStream.readSlot];
    if (g_mbgStream.readSlot == 0) {
        g_mbgStream.readSlot = g_mbgStream.slotCount;
    }
    g_mbgStream.readSlot--;
    return 0;
}

/* ---- VLC ---------------------------------------------------------------------------------------------------- */

void o64_mbg_vlcBuildTable(void *table) {
}

/* mbg_vlcDecode(bs, out, table): the frame starts at `bs` in its ring slot, which bounds how far it can extend. */
void o64_mbg_vlcDecode(u8 *bs, u32 *out, void *table) {
    if (fv_take(bs, out)) return;
    u8 *base = g_mbgStream.ringData;
    u32 slot_bytes = g_mbgStream.slotSectors * 0x8F4;
    u32 bytes = slot_bytes;
    if (slot_bytes != 0 && bs >= base) {
        u32 slot = (u32)(bs - base) / slot_bytes;
        bytes = (u32)(base + (slot + 1) * slot_bytes - bs);
    }
    if (bytes > 18336) {
        bytes = 18336;
    }
    /* (the VLC too on the other core, ahead_vlc_start, for a movie behind a field: the game's core has the field to
     * run. A full-screen movie's is expanded here: this core has nothing else to do then, and the worker shares its core
     * with the sound, which with a movie's many voices leaves it little: field 2752's movie on an Old 3DS, a frame's
     * expansion 7 ms here and 16 or more there, this core waiting for it at the frame's DecDCTin: a beat of 2440 ms
     * there, 2130 here, 88 audio underruns a beat against 18) */
    if (g_mbgRgb24 != 1 && ahead_vlc_start(bs, bytes, out)) return;
    Ff9MovieVlcInfo info;
    unsigned int t = port_profile_begin();
    int r = ff9_movie_vlc_v2(bs, bytes & ~1u, (const u8 *)g_mbgVlcPairs, (u16 *)out, 0x10000 / 2, &info);
    port_profile_detail_end(PORT_PROFILE_MOVIE_VLC, t);
    vlc_report(r);
    ahead_start(out);
}

/* ---- MBG mask ---------------------------------------------------------------------------------------------- */

/* One 16-pixel-wide, 224-row RGB555 slice, row-major. The mask stream gives, per column, the rows whose pixels get
 * `bit`: FE n skips or fills n whole columns, a byte below E0 is one run from the top (after FF) or to the bottom,
 * E0..FC (n = byte - DE) or FD n give n alternating run boundaries. FF in front starts with a filled run. A count
 * that reaches past this slice continues in the next one through the cursor's state/pending fields. */
#define MASK_ROWS 224
#define MASK_W 16

static inline void mask_px(u16 *px, s32 col, s32 row, u16 bit) {
    if ((u32)col < MASK_W && (u32)row < MASK_ROWS) {
        px[row * MASK_W + col] |= bit;
    }
}

static void mask_rows(u16 *px, s32 col, s32 from, s32 to, u16 bit) {
    s32 row = from;
    do { /* do-while, as the original: at least one row */
        mask_px(px, col, row, bit);
    } while (++row < to);
}

s32 o64_mbg_applyMask(Ovl64Vlc4C8 *c, u32 *out, u16 bit) {
    u16 *px = (u16 *)out;
    const u8 *p = (const u8 *)c->cursor;
    u32 pending = c->runLength, on = c->runMasked;
    s32 col = 0;

    if (pending != 0) {
        if (on != 0) {
            if (pending >= MASK_W) {
                c->runMasked = 1;
                c->runLength = pending - MASK_W;
                for (s32 row = 0; row < MASK_ROWS; row++) {
                    for (s32 x = 0; x < MASK_W; x++) {
                        mask_px(px, x, row, bit);
                    }
                }
                return 1;
            }
            for (s32 row = 0; row < MASK_ROWS; row++) {
                s32 x = 0;
                do {
                    mask_px(px, x, row, bit);
                } while (++x < (s32)pending);
            }
        } else if (pending >= MASK_W) {
            c->runMasked = 0;
            c->runLength = pending - MASK_W;
            return 1;
        }
        col = pending;
        c->runLength = 0;
    }
    do {
        u32 b = *p;
        on = 0;
        if (b == 0xFF) {
            on = 1;
            b = p[1];
            p++;
        }
        if (b == 0xFE) {
            u32 n = p[1];
            u32 end = col + n;
            p += 2;
            if (on) {
                if (end >= MASK_W) {
                    c->runMasked = 1;
                    c->runLength = end - MASK_W;
                    do {
                        mask_rows(px, col, 0, MASK_ROWS, bit);
                    } while (++col < MASK_W);
                    break;
                }
                do { /* the original compares with n, not col + n */
                    mask_rows(px, col, 0, MASK_ROWS, bit);
                } while (++col < (s32)n);
            } else {
                if (end >= MASK_W) {
                    c->runMasked = 0;
                    c->runLength = end - MASK_W;
                    break;
                }
                col = end;
            }
        } else if (b < 0xE0) {
            if (on) {
                u32 left = b;
                s32 row = 0;
                do {
                    mask_px(px, col, row++, bit);
                } while (--left != 0 && row < MASK_ROWS);
            } else {
                mask_rows(px, col, b, MASK_ROWS, bit);
            }
            col++;
            p++;
        } else {
            u32 n;
            if (b == 0xFD) {
                n = p[1];
                p += 2;
            } else {
                n = b - 0xDE;
                p += 1;
            }
            s32 row = 0;
            u32 k = 0;
            do {
                if (on) {
                    s32 to = *p;
                    mask_rows(px, col, row, to, bit);
                    row = row < to ? to : row + 1;
                } else {
                    row = *p;
                }
                p++;
                on ^= 1;
            } while (++k < n);
            if (on) {
                mask_rows(px, col, row, MASK_ROWS, bit);
            }
            col++;
        }
    } while (col < MASK_W);
    c->cursor = (s32)p;
    return 1;
}

/* ---- libpress (MDEC) --------------------------------------------------------------------------------------- */

extern u8 D_8019DEC8[128]; /* luma + chroma quantization, uploaded after command 40000001h */
extern u8 D_8019DF4C[128]; /* scale matrix, uploaded after command 60000000h */

MDEC_DTCM_BSS static PsxMdecEnv env;
static const u16 *in_rl;
static u32 in_left; /* halfwords */
static u32 in_cmd;
static int in_busy;
static u32 *out_buf;
static u32 out_words;
static int out_busy, out_queued, out_running;
static void (*out_cb)(void);

static void in_skip_padding(void) {
    while (in_left != 0 && *in_rl == 0xFE00) {
        in_rl++;
        in_left--;
    }
    if (in_left == 0) {
        in_busy = 0;
    }
}

/* ---- decoding ahead (plat_async) --------------------------------------------------------------------------
 * The PS1's MDEC decodes frame N while the CPU reads and VLC-decodes frame N+1; here the MDEC is the CPU. So as
 * soon as a frame's run-length data is complete (mbg_vlcDecode), another core decodes all of it into `pixels`; the
 * frame's DecDCTin then finds it there, and DecDCTout's slices are copies (waiting only for macroblocks not done
 * yet). The command word is the one DecDCTin will build (its mode is the player's rgb24 flag, set before the VLC);
 * a DecDCTin whose buffer or command differs decodes as before. The input buffer isn't written again before then:
 * the player VLC-decodes the next frame into its other buffer. */
#define AHEAD_BYTES (24 * 15 * 768) /* (320x240: 20 x 15 macroblocks of 16x16 RGB24) */
#define AHEAD_MBS (AHEAD_BYTES / 512)
/* Both cores decode it: the worker, and the game's core whenever DecDCTout needs a macroblock not done yet (it waited
 * for the worker there, some 15 ms a VBlank of a movie on an Old 3DS, whose worker shares its core with the sound).
 * The worker first finds where each macroblock starts (psx_mdec_macroblock_extent: what decoding would consume,
 * without decoding); then each core takes the next macroblock not yet taken. A macroblock decodes the same from the
 * same input on either core, into its own place: the pixels are the same as decoded in order. */
typedef struct {
    const u32 *buf;      /* the input buffer (VLC output: command word, then the run-length halfwords) */
    const u32 *src;      /* where its data is read: buf, or the VLC's own buffer (ahead_vlc_start) */
    u32 cmd, mb;         /* the command; bytes a macroblock */
    u8 *pixels;
    u32 start[AHEAD_MBS]; /* each macroblock's first halfword */
    u32 halfwords;       /* the run-length data's */
    u32 count;           /* macroblocks (before the input ran out, an error, or the buffer's end) */
    volatile int scanned; /* start and count set */
    volatile u32 next;   /* the next macroblock not yet taken by a core */
    volatile u8 ready[AHEAD_MBS];
} Ahead;
static Ahead ahead;
static int ahead_on;      /* the current DecDCTin's input is being decoded ahead */
static u32 ahead_taken;   /* macroblocks DecDCTout has copied */

static u32 movie_cmd(u32 word0, int mode) {
    word0 = mode & 1 ? word0 & ~0x08000000u : word0 | 0x08000000u;
    return mode & 2 ? word0 | 0x02000000u : word0 & ~0x02000000u;
}

/* the next macroblock not yet taken, decoded with `scratch`; 0 if none was left */
static int ahead_take(Ahead *a, u8 *scratch) {
    const u32 i = __atomic_fetch_add(&a->next, 1, __ATOMIC_ACQ_REL);
    if (i >= a->count) return 0;
    const u16 *rl = (const u16 *)(a->src + 1) + a->start[i];
    size_t used = 0;
    psx_mdec_macroblock_with(&env, rl, a->halfwords - a->start[i], a->cmd, a->pixels + i * a->mb, a->mb, &used, scratch);
    __atomic_store_n(&a->ready[i], 1, __ATOMIC_RELEASE); /* (the pixels before the flag) */
    return 1;
}

/* One macroblock of the frame being decoded ahead, not yet taken, decoded on the game's core: 0 if none (no frame
 * ahead, or all taken). For the game's waits for a VBlank (the 3DS's plat_wait_frame): a movie behind a field (15 frames
 * a second, the game 30) put the game core's share of a frame's macroblocks into every other game frame, which then ran
 * late, while the one between finished early and waited (an Old 3DS, field 1464). A macroblock is some 0.1 ms there */
int port_idle_step(void) {
    static u8 scratch[768] __attribute__((aligned(4)));
    if (!ahead.buf || !__atomic_load_n(&ahead.scanned, __ATOMIC_ACQUIRE)) return 0;
    return ahead_take(&ahead, scratch);
}

static void ahead_job(void *arg) {
    static u8 scratch[768] __attribute__((aligned(4)));
    Ahead *a = arg;
    const u16 *const base = (const u16 *)(a->src + 1), *rl = base;
    u32 left = a->halfwords, n = 0;
    while (left) { /* (as the macroblocks were decoded in order: the padding between them skipped, the same stops) */
        while (left && *rl == 0xFE00) rl++, left--;
        if (!left || (n + 1) * a->mb > AHEAD_BYTES) break;
        size_t used = 0;
        if (psx_mdec_macroblock_extent(rl, left, a->cmd, a->mb, &used) != PSX_MDEC_OK) break;
        a->start[n++] = (u32)(rl - base);
        rl += used;
        left -= used;
    }
    a->count = n;
    __atomic_store_n(&a->scanned, 1, __ATOMIC_RELEASE);
    while (ahead_take(a, scratch)) {}
}

static u8 ahead_pixels[AHEAD_BYTES] __attribute__((aligned(32)));
static void ahead_start(u32 *out) {
    if (fv_now || fv_mbg) return;
    plat_async_wait(); /* (the previous frame's job, done long ago) */
    ahead.buf = ahead.src = PSX_PTR(out);
    ahead.cmd = movie_cmd(ahead.buf[0], (s8)g_mbgPlayer.rgb24);
    ahead.mb = (ahead.cmd & 0x08000000) ? 512 : 768;
    ahead.halfwords = (ahead.cmd & 0xFFFF) * 2;
    ahead.pixels = ahead_pixels;
    ahead.count = 0;
    ahead.scanned = 0;
    ahead.next = 0;
    __builtin_memset((void *)ahead.ready, 0, sizeof ahead.ready);
    __sync_synchronize();
    if (!plat_async(ahead_job, &ahead)) ahead.buf = NULL;
}

/* The frame's VLC on the other core as well, before its macroblocks (ahead_job): the player's mbg_vlcDecode returns at
 * once. On the PS1 the CPU expands frame N+1 while the MDEC decodes frame N; here the game's core did it (2.2 ms of a
 * VBlank of field 1464's movie background on an Old 3DS) while the worker, done with frame N, waited for it: that core
 * had as much time to spare. Its input is copied first (the player frees the frame's ring slot when the call returns),
 * and it expands into a buffer of its own: the frame's DecDCTin waits for it and copies what it wrote into the player's
 * buffer, so the game's memory is only written on the game's core, before anything reads it (a movie stopped before
 * then leaves it as it was). */
#define VLC_SRC_MAX 18336 /* (o64_mbg_vlcDecode's bound) */
static struct {
    u8 src[VLC_SRC_MAX] __attribute__((aligned(4)));
    u32 bytes;
    u32 *out;
    int rgb24;
    volatile int done, r;
    u32 dst[0x10000 / 4]; /* (the VLC's output: o64_mbg_vlcDecode's capacity) */
    u32 halfwords;        /* ... as much as it wrote */
} vlc_ahead;
static void ahead_vlc_job(void *arg) {
    Ff9MovieVlcInfo info;
    unsigned int t = port_profile_begin();
    info.halfwords = 0;
    vlc_ahead.r = ff9_movie_vlc_v2(vlc_ahead.src, vlc_ahead.bytes & ~1u, (const u8 *)g_mbgVlcPairs, (u16 *)vlc_ahead.dst,
                                   0x10000 / 2, &info);
    /* (on an error, its partial output: all of the buffer, as written in place it would be partly there) */
    vlc_ahead.halfwords = vlc_ahead.r == FF9_VLC_OK && info.halfwords <= 0x10000 / 2 ? info.halfwords : 0x10000 / 2;
    port_profile_detail_end(PORT_PROFILE_MOVIE_VLC, t);
    /* (ahead_start's, from the data now there) */
    ahead.cmd = movie_cmd(ahead.src[0], vlc_ahead.rgb24);
    ahead.mb = (ahead.cmd & 0x08000000) ? 512 : 768;
    ahead.halfwords = (ahead.cmd & 0xFFFF) * 2;
    __atomic_store_n(&vlc_ahead.done, 1, __ATOMIC_RELEASE);
    ahead_job(arg);
}
static int ahead_vlc_start(const u8 *bs, u32 bytes, u32 *out) {
    if (fv_now || fv_mbg || bytes > VLC_SRC_MAX) return 0;
    vlc_flush(); /* (the previous frame's job, done long ago; its data DecDCTin'd: nothing to copy) */
    __builtin_memcpy(vlc_ahead.src, bs, bytes);
    vlc_ahead.bytes = bytes;
    vlc_ahead.out = out;
    vlc_ahead.rgb24 = (s8)g_mbgPlayer.rgb24;
    vlc_ahead.done = 0;
    ahead.buf = PSX_PTR(out);
    ahead.src = vlc_ahead.dst;
    ahead.cmd = ~0u; /* (until the VLC is done: no DecDCTin matches it before it waits) */
    ahead.pixels = ahead_pixels;
    ahead.count = 0;
    ahead.scanned = 0;
    ahead.next = 0;
    __builtin_memset((void *)ahead.ready, 0, sizeof ahead.ready);
    __sync_synchronize();
    if (plat_async(ahead_vlc_job, &ahead)) return 1;
    /* (refused: no worker (the PC; a 3DS whose second core runs nothing: async.c). Nothing will expand into this buffer:
     * vlc_wait would wait for it forever, as the PC did when Cross was pressed in the opening movie) */
    vlc_ahead.out = NULL;
    ahead.buf = NULL;
    return 0;
}
/* a DecDCTin of `buf`: the VLC decoding into it on the other core done first and copied there (its error, if any,
 * reported) */
static void vlc_wait(const u32 *buf) {
    if (!vlc_ahead.out || (const u32 *)PSX_PTR(vlc_ahead.out) != buf) return;
    while (!__atomic_load_n(&vlc_ahead.done, __ATOMIC_ACQUIRE)) {}
    __builtin_memcpy((u32 *)buf, vlc_ahead.dst, vlc_ahead.halfwords * 2);
    vlc_report(vlc_ahead.r);
    vlc_ahead.out = NULL;
}
/* the other core's job done, and a frame it expanded copied into the player's buffer if no DecDCTin took it */
static void vlc_flush(void) {
    plat_async_wait();
    if (vlc_ahead.out) vlc_wait((const u32 *)PSX_PTR(vlc_ahead.out));
}

/* An out request from the frame decoded ahead: 0 if it has fewer macroblocks than asked (input ran out). */
static int ahead_request(u32 *dst, u32 words) {
    static u8 scratch[768] __attribute__((aligned(4)));
    u32 mb = (in_cmd & 0x08000000) ? 512 : 768, n = words * 4 / mb;
    while (!__atomic_load_n(&ahead.scanned, __ATOMIC_ACQUIRE)) {}
    u32 want = ahead_taken + n;
    if (want > ahead.count) want = ahead.count;
    for (u32 i = ahead_taken; i < want; i++) /* (each not done yet: another one taken and decoded here meanwhile) */
        while (!__atomic_load_n(&ahead.ready[i], __ATOMIC_ACQUIRE))
            if (!ahead_take(&ahead, scratch)) {}
    u32 have = want - ahead_taken;
    __builtin_memcpy(dst, ahead.pixels + ahead_taken * mb, have * mb);
    ahead_taken += have;
    if (have < n) {
        in_busy = 0;
        return 0;
    }
    /* busy while macroblocks remain */
    in_busy = ahead_taken < ahead.count;
    return 1;
}

/* Fill one out request; returns 0 if the input ran out first. */
static int decode_request(u32 *dst, u32 words) {
    if (ahead_on) return ahead_request(dst, words);
    u32 mb = (in_cmd & 0x08000000) ? 512 : 768; /* bit 27 set: 15-bit, clear: 24-bit */
    u8 *o = (u8 *)dst;
    u32 bytes = words * 4;
    while (bytes >= mb) {
        in_skip_padding();
        if (!in_busy) {
            return 0;
        }
        size_t used = 0;
        int r = psx_mdec_macroblock(&env, in_rl, in_left, in_cmd, o, mb, &used);
        if (r != PSX_MDEC_OK) {
            in_busy = 0;
            in_left = 0;
            return 0;
        }
        in_rl += used;
        in_left -= used;
        o += mb;
        bytes -= mb;
    }
    in_skip_padding();
    return 1;
}

int o64_DecDCTReset(int mode) {
    plat_async_wait();
    if (mode == 0) vlc_ahead.out = NULL; /* (a new movie: the last one's buffers may be anything's now) */
    else vlc_flush();
    ahead.buf = NULL;
    ahead_on = 0;
    if (mode == 0) {
        movie_fv_stop(); /* (a new movie) */
        movie_fv_mbg_stop();
        fv_logged = 0;
        fv_unavailable = 0;
        fv_mbg_unavailable = 0;
        fv_frame_of[0] = fv_frame_of[1] = 0;
        fv_now = 0;
        psx_mdec_env_init(&env, D_8019DEC8, D_8019DF4C);
    }
    in_busy = 0;
    in_left = 0;
    out_busy = 0;
    out_queued = 0;
    return 0;
}

int o64_DecDCTin(u32 *buf, int mode) {
    buf = PSX_PTR(buf);
    vlc_wait(buf);
    u32 frame = fv_frame_of[in_buffer(buf)];
    fv_mbg = frame && movie_fv_mbg_active();
    fv_now = frame && (movie_fv_active() || fv_mbg);
    if (fv_now) { /* busy until the player's slices are done, without data */
        if (fv_mbg) {
            unsigned int t = port_profile_begin();
            movie_fv_mbg_frame(frame); /* (one it can't decode shows the last again) */
            port_profile_detail_end(PORT_PROFILE_MOVIE_MDEC, t);
            fv_col = 0;
            fv_stp = mode & 2 ? 0x8000u : 0;
        } else {
            movie_fv_frame(frame);
        }
        in_rl = NULL;
        in_left = 0;
        in_busy = 1;
        return 0;
    }
    if (mode & 1) {
        buf[0] &= ~0x08000000u;
    } else {
        buf[0] |= 0x08000000u;
    }
    if (mode & 2) {
        buf[0] |= 0x02000000u;
    } else {
        buf[0] &= ~0x02000000u;
    }
    in_cmd = buf[0];
    in_rl = (const u16 *)(buf + 1);
    in_left = (in_cmd & 0xFFFF) * 2;
    in_busy = in_left != 0;
    ahead_on = ahead.buf == buf && ahead.cmd == in_cmd;
    ahead_taken = 0;
    if (!ahead_on && ahead.buf) plat_async_wait(); /* (not the frame decoded ahead: it decodes here, with env) */
    return 0;
}

int o64_DecDCTout(u32 *buf, int words) {
    out_buf = PSX_PTR(buf);
    out_words = (u32)words;
    out_busy = 1;
    out_queued = 1;
    if (out_running) {
        return 0; /* from the out callback: the loop below runs it */
    }
    out_running = 1;
    psx_load_image_skip = fv_now && !fv_mbg;
    while (out_queued) {
        out_queued = 0;
        if (fv_now) { /* the transcode's frame: only the slice chain (behind a field, its slices from it) */
            if (fv_mbg) {
                int w = g_mbgDecode.slice.w > 0 ? g_mbgDecode.slice.w : 16, h = (int)(out_words * 2) / w;
                unsigned int t = port_profile_begin();
                movie_fv_mbg_slice((u16 *)out_buf, fv_col, w, h, fv_stp);
                port_profile_detail_end(PORT_PROFILE_MOVIE_FV, t);
                fv_col += w;
            }
            out_busy = 0;
            if (out_cb) out_cb();
            continue;
        }
        unsigned int t = port_profile_begin();
        int filled = decode_request(out_buf, out_words);
        port_profile_detail_end(PORT_PROFILE_MOVIE_MDEC, t);
        if (!filled) {
            break; /* stalled: stays busy, no callback */
        }
        out_busy = 0;
        if (out_cb) {
            out_cb();
        }
    }
    psx_load_image_skip = 0;
    out_running = 0;
    return 0;
}

int o64_DecDCTinSync(int mode) {
    return mode ? in_busy : 0;
}

s32 o64_DecDCToutSync(s32 mode) {
    return mode ? out_busy : 0;
}

void o64_DecDCToutCallback(void (*func)()) {
    out_cb = (void (*)(void))func;
    /* (mbg_stop: a frame expanded on the other core and not yet DecDCTin'd copied into the player's buffer now, while
     * it is still the player's, as the VLC in place had written it) */
    if (!func) { vlc_flush(); movie_fv_stop(); movie_fv_mbg_stop(); }
}

void o64_DecDCTinCallback(void (*func)()) {
}
