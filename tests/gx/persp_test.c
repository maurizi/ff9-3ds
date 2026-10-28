/* How the 3D engine takes the perspective projection gx_hybrid.c's fm_emit_hw builds (branch gx-models): one
 * triangle through the orthographic projection the hybrid uses (v16 units = PS1 pixels), red, and the same triangle
 * 160 pixels to the right as view coordinates through the perspective one (GTE H 401, OFX 160, OFY 112, Z 3000; a
 * scale matrix of 8, the coordinates / 8), green, as packed command words through GFX_FIFO. `frames` set when drawn
 * (tools/ds_profile.py --screenshot). */
#include <nds.h>

volatile u32 frames;

static void put(u32 w) { GFX_FIFO = w; }

int main(void) {
    videoSetMode(MODE_0_3D);
    glInit();
    glClearColor(0, 0, 0, 31);
    glClearPolyID(63);
    glClearDepth(0x7FFF);
    glViewport(0, 0, 255, 191);
    const int h = 224, H = 401, ofx = 160 << 16, ofy = 112 << 16, Z = 3000;
    for (;;) {
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glOrthof32(0, 320, h, 0, -32768, 32768);
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        /* red, orthographic: (40,40) (120,40) (80,120), depth 0 */
        put(0x29); put(3u << 6 | 31u << 16);                /* POLYATTR */
        put(0x20); put(31);                                  /* COLOR */
        put(0x40); put(0);                                   /* BEGIN triangles */
        const int px[3] = {40, 120, 80}, py[3] = {40, 40, 120};
        for (int i = 0; i < 3; ++i) { put(0x23); put((u32)(u16)px[i] | (u32)(u16)py[i] << 16); put(0); }
        /* green, perspective: the same + 160 in x */
        s32 P[16] = {0};
        P[0] = 2 * H * 4096 / 320;
        P[8] = (s32)(((long long)2 * ofx * 4096 / 320) >> 16) - 4096;
        P[5] = -2 * H * 4096 / h;
        P[9] = 4096 - (s32)(((long long)2 * ofy * 4096 / h) >> 16);
        P[10] = 0; P[14] = 0; P[11] = 4096;
        put(0x10); put(0);
        put(0x16); for (int i = 0; i < 16; ++i) put((u32)P[i]);
        put(0x10); put(2);
        put(0x17); for (int i = 0; i < 12; ++i) put((i == 0 || i == 4 || i == 8) ? (u32)(8 * 4096 * 4096) : 0);
        put(0x20); put(31 << 5);
        put(0x40); put(0);
        for (int i = 0; i < 3; ++i) {
            const int X = (px[i] + 160 - 160) * Z / H, Y = (py[i] - 112) * Z / H;
            put(0x23); put((u32)(u16)(X / 8) | (u32)(u16)(Y / 8) << 16); put((u32)(u16)(Z / 8));
        }
        put(0x10); put(0);
        put(0x15);
        put(0x10); put(2);
        put(0x15);
        glFlush(0);
        swiWaitForVBlank();
        frames++;
    }
}
