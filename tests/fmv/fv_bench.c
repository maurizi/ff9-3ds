/* FastVideoDS decode benchmark on the DS (FMV transcode, docs/movie-validation.md): fat:/testVideo.fv decoded frame
 * by frame with FastVideoDS's decoder (an external checkout at fastvideo/, not part of this repository), its code and
 * tables in main RAM as the port would run them, and shown. Bottom screen: the decode time per frame (the CPU's,
 * not the 3D engine's two VBlanks for a P-frame's motion compensation).
 *
 * Synchronous, display one frame behind: banks A-C rotate as the frame shown, the reference (a P-frame's motion
 * compensation samples it as a texture) and the target (the 3D engine's prediction captured into it, the residual
 * added); banks D and E (contiguous as LCD) hold the residual, 192 bytes an 8x8 block: 144 KB at 192 rows, so
 * bank D alone limits the height to 170 (the rest is read back as black and spreads up from the bottom). */
#include <nds.h>
#include <fat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fvDecoder.h"
#include "fvMcData.h"
#include "fv_mc.h"

static fv_decoder_asm_t dec;
volatile u32 frames; /* (frames decoded: tools/ds_profile.py waits on it) */
static fv_header_t header;
static u8 *data;
static u32 dl_length;
#ifdef FV_BENCH_CPU_MC
static u16 mc[256 * 192];
static u32 mc_bad_frames, mc_bad_pixels, mc_ticks, mc_rows[24], mc_example[6];
#endif

/* the frame from bank (and screen) row 8: the top row of triangles starts on screen, unclipped (clipped, its texcoords
 * sample off the encoder's prediction); its first row repeated above it, as the texture's top clamp then reads */
#define TOP 8
#define BLOCK_VTX(x, y, z) (((x) & 0x3FF) | ((((y) >> 3) & 0x3FF) << 10) | ((z) << 20))

/* the motion compensation's quads: a texcoord the vector decoder fills, then three VTX_10 per 16x16 block */
static u32 *mb_list(u32 *dl, int height, u32 tex_param) {
    int y = 0;
    for (; y < height / 16; y++)
        for (int x = 0; x < 32; x++) {
            *dl++ = FIFO_COMMAND_PACK(FIFO_TEX_COORD, FIFO_VERTEX10, FIFO_VERTEX10, FIFO_VERTEX10);
            *dl++ = 0;
            *dl++ = BLOCK_VTX(x * 8, y * 8 - 8 + TOP, 33 - x);
            *dl++ = BLOCK_VTX(x * 8, y * 8 + 8 + TOP, 32 - x);
            *dl++ = BLOCK_VTX(x * 8 + 16, y * 8 + 8 + TOP, 32 - x);
        }
    *dl++ = FIFO_COMMAND_PACK(FIFO_END, FIFO_TEX_FORMAT, FIFO_BEGIN, FIFO_NOP);
    *dl++ = tex_param;
    *dl++ = GL_TRIANGLES;
    for (; y < height / 8; y++)
        for (int x = 0; x < 32; x++) {
            *dl++ = FIFO_COMMAND_PACK(FIFO_TEX_COORD, FIFO_VERTEX10, FIFO_VERTEX10, FIFO_VERTEX10);
            *dl++ = 0;
            *dl++ = BLOCK_VTX(x * 8, y * 8 - 8 + TOP, 33 - x);
            *dl++ = BLOCK_VTX(x * 8, y * 8 + 8 + TOP, 32 - x);
            *dl++ = BLOCK_VTX(x * 8 + 16, y * 8 + 8 + TOP, 32 - x);
        }
    return dl;
}

static void setup_3d(int height) {
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
    glMatrixMode(GL_PROJECTION); /* screen pixels, x in 1/64ths, y in 1/8ths (the vertices' units) */
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
    (void)height;
}

static vu8 *const vram_cr = (vu8 *)0x04000240;
static u16 *bank_lcd(int b) { return (u16 *)(0x06800000 + 0x20000 * b); }

/* MPU region 2 over the LCD-mapped VRAM (1 MB at 0x06800000), cached and write-buffered: the decoder reads and
 * writes the frames there */
static void vram_cache(void) {
    u32 r = (0x13u << 1) | 0x06800000u | 1;
    __asm__ volatile("mcr p15, 0, %0, c6, c3, 0" : : "r"(r)); /* (region 3: 2 is the vectors at 0) */
    __asm__ volatile("mrc p15, 0, %0, c3, c0, 0" : "=r"(r)); r |= 1 << 3;
    __asm__ volatile("mcr p15, 0, %0, c3, c0, 0" : : "r"(r));
    __asm__ volatile("mrc p15, 0, %0, c2, c0, 0" : "=r"(r)); r |= 1 << 3;
    __asm__ volatile("mcr p15, 0, %0, c2, c0, 0" : : "r"(r));
}

static u32 ticks(void) { return TIMER_DATA(0) | (u32)TIMER_DATA(1) << 16; }

int main(void) {
    videoSetModeSub(MODE_0_2D);
    vramSetBankH(VRAM_H_SUB_BG); /* (the console off banks A-D, which the decoder takes) */
    consoleInit(NULL, 2, BgType_Text4bpp, BgSize_T_256x256, 2, 1, false, true);
    if (getenv("NOCACHE") == NULL) {}
    DC_FlushAll();
    vram_cache();
    if (!fatInitDefault()) { printf("no FAT\n"); for (;;) swiWaitForVBlank(); }
    FILE *f = fopen("fat:/testVideo.fv", "rb");
    if (!f || fread(&header, sizeof header, 1, f) != 1 || header.signature != FV_SIGNATURE) {
        printf("no video\n"); for (;;) swiWaitForVBlank();
    }
    fv_keyframe_t key0;
    fread(&key0, sizeof key0, 1, f);
    fseek(f, (long)key0.offset, SEEK_SET);
    data = aligned_alloc(32, 64 * 1024);
    int height = header.height;
    printf("%dx%d %lu frames %lu/%lu fps\n", header.width, height, header.nrFrames, header.fpsNum, header.fpsDen);

    fv_initDecoderAsm(&dec);
    dec.yBlocks = height / 8;
    dec.texYOffset = (256 - height - TOP) << 1;
    u32 tex_param = 0xDED00000 + (((-(256 - height - TOP) * 256 * 2) >> 3) & 0xFFFF);
    u32 *dl = mb_list(fv_gMcGxCmdBufA, height, tex_param);
    *dl++ = FIFO_COMMAND_PACK(FIFO_END, FIFO_POLY_FORMAT, FIFO_TEX_FORMAT, FIFO_BEGIN);
    *dl++ = 0x0F4080;
    *dl++ = 0xDED00000;
    *dl++ = GL_TRIANGLES;
    dec.dlB = dl + 1;
    dl = mb_list(dl, height, tex_param);
    *dl++ = FIFO_COMMAND_PACK(FIFO_END, FIFO_FLUSH, FIFO_NOP, FIFO_NOP);
    *dl++ = 0;
    dl_length = (u32)dl - (u32)fv_gMcGxCmdBuf;
    DC_FlushRange(fv_gMcGxCmdBuf, dl_length);

    vramSetBankA(VRAM_A_LCD); vramSetBankB(VRAM_B_LCD); vramSetBankC(VRAM_C_LCD); vramSetBankD(VRAM_D_LCD);
    vramSetBankE(VRAM_E_LCD); /* (the residual: 192 bytes an 8x8 block, 144 KB at 192 rows, runs on past D) */
    for (int i = 0; i < 3 * 128 * 1024; i += 4) *(vu32 *)(0x06800000 + i) = 0x80008000;
    setup_3d(height);
    videoSetMode(MODE_5_3D | DISPLAY_BG2_ACTIVE);
    REG_BG2CNT = (u16)BgSize_B16_256x256;
    REG_BG2PA = 256; REG_BG2PB = 0; REG_BG2PC = 0; REG_BG2PD = 256;
    REG_BG2X = 0; REG_BG2Y = (TOP - ((192 - height) >> 1)) << 8;
    REG_DISPCNT = MODE_5_2D | DISPLAY_BG2_ACTIVE;
    TIMER_DATA(0) = 0; TIMER_DATA(1) = 0;
    TIMER_CR(1) = TIMER_ENABLE | TIMER_CASCADE;
    TIMER_CR(0) = TIMER_ENABLE | TIMER_DIV_1;

    int shown = -1, ref = -1;
    u32 total = 0, worst = 0, pframes = 0, iframes = 0;
    for (u32 n = 0; n < header.nrFrames; n++) {
        u32 len;
        if (fread(&len, 4, 1, f) != 1) break;
        u32 bytes = len & 0x1FFFF, audio = len >> 17;
        if (bytes > 64 * 1024 || fread(data, 1, bytes, f) != bytes) break;
        if (audio) fseek(f, (long)(audio * 2 * FV_AUDIO_FRAME_SIZE), SEEK_CUR);
        int tgt = 0;
        while (tgt == shown || tgt == ref) tgt++;
        vram_cr[tgt] = 0x80;
        DC_FlushRange(data, bytes);
        u32 t0 = ticks(), cpu = 0;
        if (fv_frameIsP((const u16 *)data)) {
            const u16 *p;
#ifdef FV_BENCH_CPU_FULL
            /* the whole P-frame by the CPU: its prediction into the target bank (as movies behind a field do) */
            p = fv_decodePFrameVectors(&dec, (const u16 *)data);
            fv_mc_predict(bank_lcd(ref) + TOP * 256, bank_lcd(tgt) + TOP * 256, (const u32 *)fv_gMcGxCmdBufA + 1,
                          dec.dlB, height, 256 - height - TOP);
            cpu += ticks() - t0;
            if (0) {
#else
            if (1) {
#endif
            vram_cr[ref] = 0x83; /* the reference as texture slot 0 */
            p = fv_decodePFrameVectors(&dec, (const u16 *)data);
            cpu += ticks() - t0;
            DC_FlushRange(fv_gMcGxCmdBuf, dl_length);
            /* (from the blocks' polygon setup: the list's head first draws a black strip over the top rows, which the
             * encoder's prediction doesn't have) */
            DMA3_SRC = (u32)fv_gMcGxCmdBuf + 172 * 4; DMA3_DEST = (u32)&GFX_FIFO;
            DMA3_CR = DMA_FIFO | ((dl_length - 172 * 4) >> 2);
            /* the whole list in before the VBlank that swaps (a swap the geometry engine reaches late is a frame
             * late: the capture would take the last prediction) */
            while (DMA3_CR & DMA_BUSY) {}
            while (GFX_STATUS & (1 << 27)) {}
            swiWaitForVBlank();
            REG_DISPCAPCNT = DCAP_ENABLE | DCAP_MODE(DCAP_MODE_A) | DCAP_BANK(tgt) | DCAP_SIZE(DCAP_SIZE_256x192) |
                             DCAP_SRC_A(DCAP_SRC_A_3DONLY);
            swiWaitForVBlank();
            while (REG_DISPCAPCNT & DCAP_ENABLE) {}
#ifdef FV_BENCH_CPU_MC
            /* the CPU's prediction against the 3D engine's capture */
            vram_cr[ref] = 0x80;
            DC_FlushAll();
            u32 tm = ticks();
            fv_mc_predict(bank_lcd(ref) + TOP * 256, mc, (const u32 *)fv_gMcGxCmdBufA + 1, dec.dlB, height,
                          256 - height - TOP);
            mc_ticks += ticks() - tm;
            const u16 *cap = bank_lcd(tgt) + TOP * 256;
            u32 bad = 0;
            for (int i = 0; i < 256 * height; ++i)
                if ((cap[i] | 0x8000) != mc[i]) {
                    bad++;
                    mc_rows[i / 256 / 8]++;
                    if (!mc_example[0]) {
                        int bx = (i % 256) / 8, by = i / 256 / 8, k = by * 32 + bx + (by >= height / 16 ? 0 : 0);
                        const u32 *la = (const u32 *)fv_gMcGxCmdBufA + 1 + k * 5 + (by >= height / 16 ? 3 : 0);
                        const u32 *lb = dec.dlB + k * 5 + (by >= height / 16 ? 3 : 0);
                        mc_example[0] = i | 0x80000000u; mc_example[1] = cap[i]; mc_example[2] = mc[i];
                        mc_example[3] = *la; mc_example[4] = *lb; mc_example[5] = n;
                    }
                }
            if (bad) { mc_bad_frames++; mc_bad_pixels += bad; }
#endif
            }
            u32 t1 = ticks();
            fv_decodePFrameDcts(&dec, p, (u8 *)VRAM_D);
            DC_FlushAll();
            fv_finishPBlock(&dec, (u8 *)VRAM_D, bank_lcd(tgt) + TOP * 256);
            cpu += ticks() - t1;
            vram_cr[ref] = 0x80;
            pframes++;
        } else {
            fv_decodeFrame_asm(&dec, (const u16 *)data, (u8 *)(bank_lcd(tgt) + TOP * 256));
            cpu += ticks() - t0;
            iframes++;
        }
        for (int r = 0; r < TOP; ++r) memcpy(bank_lcd(tgt) + r * 256, bank_lcd(tgt) + TOP * 256, 512);
        DC_FlushAll();
        swiWaitForVBlank();
        if (shown >= 0) vram_cr[shown] = 0x80;
        if (ref >= 0) { vram_cr[ref] = 0x81; shown = ref; } /* (one frame behind: the reference is shown now) */
        ref = tgt;
        frames = n + 1;
        total += cpu;
        if (cpu > worst) worst = cpu;
        if (n % 30 == 29)
            printf("\x1b[4;0H%lu: avg %lu us worst %lu us\nP %lu I %lu   ", n + 1, total / (n + 1) * 1000 / 33514,
                   worst * 1000 / 33514, pframes, iframes);
#ifdef FV_BENCH_CPU_MC
        if (n % 30 == 29)
            printf("\nMC bad %lu f %lu px, %lu us   ", mc_bad_frames, mc_bad_pixels,
                   pframes ? mc_ticks / pframes * 1000 / 33514 : 0);
#endif
    }
#ifdef FV_BENCH_CPU_MC
    printf("\x1b[2J");
    for (int r = 0; r < 22; ++r) printf("%lu ", mc_rows[r]);
    printf("\nMC %lu us/frame", pframes ? mc_ticks / pframes * 1000 / 33514 : 0);
    printf("\nex px %lu,%lu frame %lu\ncap %04lx mc %04lx\nA %08lx B %08lx\n", (mc_example[0] & 0xFFFF) % 256,
           (mc_example[0] & 0xFFFFF) / 256, mc_example[5], mc_example[1], mc_example[2], mc_example[3], mc_example[4]);
#endif
    printf("\ndone: avg %lu us/frame\n", total / (pframes + iframes) * 1000 / 33514);
    for (;;) swiWaitForVBlank();
}
