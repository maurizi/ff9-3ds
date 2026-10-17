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

/* ---- VLC ---------------------------------------------------------------------------------------------------- */

void o64_mbg_vlcBuildTable(void *table) {
}

/* mbg_vlcDecode(bs, out, table): the frame starts at `bs` in its ring slot, which bounds how far it can extend. */
void o64_mbg_vlcDecode(u8 *bs, u32 *out, void *table) {
    u8 *base = g_mbgStream.unk78;
    u32 slot_bytes = g_mbgStream.unk5C * 0x8F4;
    u32 bytes = slot_bytes;
    if (slot_bytes != 0 && bs >= base) {
        u32 slot = (u32)(bs - base) / slot_bytes;
        bytes = (u32)(base + (slot + 1) * slot_bytes - bs);
    }
    if (bytes > 18336) {
        bytes = 18336;
    }
    Ff9MovieVlcInfo info;
    unsigned int t = port_profile_begin();
    int r = ff9_movie_vlc_v2(bs, bytes & ~1u, (const u8 *)g_mbgVlcPairs, (u16 *)out, 0x10000 / 2, &info);
    port_profile_detail_end(PORT_PROFILE_MOVIE_VLC, t);
    if (r != FF9_VLC_OK) {
        static int reported;
        if (!reported++) {
            port_log("movie: VLC error %d at frame %d\n", r, g_mbgFrame);
        }
    }
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
    const u8 *p = (const u8 *)c->unkC;
    u32 pending = c->unk2, on = c->unk0;
    s32 col = 0;

    if (pending != 0) {
        if (on != 0) {
            if (pending >= MASK_W) {
                c->unk0 = 1;
                c->unk2 = pending - MASK_W;
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
            c->unk0 = 0;
            c->unk2 = pending - MASK_W;
            return 1;
        }
        col = pending;
        c->unk2 = 0;
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
                    c->unk0 = 1;
                    c->unk2 = end - MASK_W;
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
                    c->unk0 = 0;
                    c->unk2 = end - MASK_W;
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
    c->unkC = (s32)p;
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

/* Fill one out request; returns 0 if the input ran out first. */
static int decode_request(u32 *dst, u32 words) {
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
    if (mode == 0) {
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
    while (out_queued) {
        out_queued = 0;
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
}

void o64_DecDCTinCallback(void (*func)()) {
}
