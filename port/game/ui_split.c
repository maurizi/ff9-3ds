/* Which packets are UI: the window system's (windows, their text, list cursors, titles), whatever mode draws them.
 * win_drawAll is wrapped (config/renames.txt): the packets it adds to the frame's buffer lie between the buffer's
 * free pointer before and after it, and the renderer is told that range (port/3ds/render.c draws such packets on
 * the bottom screen; the PC ignores it). */
#define M2CTX
#include "ff9.h"
#include "psx_mem.h"

void win_drawAll_game(void);
__attribute__((weak)) void port_ui_packets(const void *lo, const void *hi) { (void)lo; (void)hi; }

void win_drawAll(void) {
    u8 *lo = g_primPtr;
    win_drawAll_game();
    if (g_primPtr != lo) port_ui_packets((const void *)PSX_PTR(lo), (const void *)PSX_PTR(g_primPtr));
}
