/* file_copyViaVram (decomp src/main/file.c): the PS1 copies RAM to RAM through VRAM, each piece uploaded (LoadImage)
 * and read back (StoreImage), faster there than a CPU copy. Given no rectangle it uses the draw buffer, 320x218, as
 * its scratch, which the next frame draws over. Here VRAM is the renderer's: an upload into the GPU's frame first read
 * that frame back to fill its partly covered cells, a GPU sync and a frame's readback for every piece (the swirl into
 * a battle, while the battle's data was copied in: 25-31 ms a frame in Azahar, the transition's lag on a 3DS). With no
 * rectangle it is a plain copy, in the same pieces (the return value and the slots as the game's); with one, the
 * game's own (its rectangle may be where the data is meant to be). */
#define M2CTX
#include "ff9.h"

s32 file_copyViaVram_game(s32 dst, s32 src, s32 size, RECT *rc);

s32 file_copyViaVram(s32 dst, s32 src, s32 size, RECT *rc) {
    if (rc != NULL) return file_copyViaVram_game(dst, src, size, rc);
    s32 done = 0;
    while (done < size) {
        u32 left = (u32)(size - done), i = g_vramCopyRectIdx, h, cols;
        g_vramCopyRectIdx = (u8)(i + 1);
        RECT *rect = &g_vramCopyRects[i];
        if (i >= 5) g_vramCopyRectIdx = 0;
        rect->x = g_gameState->drawEnv[g_gameState->gameData->drawBuf].clip.x;
        rect->y = g_gameState->drawEnv[g_gameState->gameData->drawBuf].clip.y;
        rect->w = 320;
        rect->h = 218;
        h = left >> 1;
        cols = h / (u32)rect->h;
        if (cols == 0 && left != 0) {
            cols = 1;
            rect->h = (s16)h;
            if ((s16)h == 0) rect->h = (s16)(h + 1);
        } else if ((u32)rect->w < cols) {
            cols = (u32)rect->w;
        }
        rect->w = (s16)cols;
        s32 n = rect->w * rect->h * 2;
        /* (the source as the upload took it, then the CD idle, then the destination written: the game's order) */
        static u8 piece[320 * 218 * 2];
        __builtin_memcpy(piece, (const void *)src + done, (unsigned)n);
        while (cd_getState() != 0) {
        }
        __builtin_memcpy((void *)dst + done, piece, (unsigned)n);
        done += n;
    }
    return done;
}
