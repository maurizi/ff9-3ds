/* The world map's bottom screen (docs/ui-design.md): the full navigation map (the PS1's Select view: the continents,
 * the places found, the party and its vehicle) is always there, and the top screen keeps the world without the corner
 * minimap. wm_naviDraw is wrapped (config/renames.txt): it draws the full map's packets whatever mode the game's
 * navigation is in, and their range goes to the bottom screen (port_bottom_packets). The location title it also
 * draws stays in the scene. The game's own full-map mode (its place cursor) is shown the same way. A tap on it zooms in
 * 2x about the party, and out again. */
#include "psyq_port.h"
#include "ovl_0c.h"

void o0c_wm_naviDraw_game(void);
void o0c_wm_drawTitle(void);
__attribute__((weak)) void port_bottom_packets(const void *lo, const void *hi) { (void)lo; (void)hi; }
__attribute__((weak)) void port_bottom_zoom(const void *lo, const void *hi, int cx, int cy, int factor) {
    (void)lo; (void)hi; (void)cx; (void)cy; (void)factor;
}
void o0c_wm_naviWorldToMap(s32, s32, s32 *, s32 *, u8);

static int zoomed; /* the bottom-screen map zoomed 2x about the party (a tap toggles it: port_world_tap) */
int port_world_tap(void) {
    zoomed = !zoomed;
    return 1;
}

void o0c_wm_naviDraw(void) {
    if (g_worldTitleId >= 0) o0c_wm_drawTitle();
    if (!g_worldNaviActive) return;
    s8 title = g_worldTitleId;
    s32 mode = g_worldNaviMode;
    u8 visible = g_worldNaviVisible;
    g_worldTitleId = -1;
    if (mode != 2) g_worldNaviMode = 2, g_worldNaviVisible = 1;
    s32 lo = g_worldPrimPtr;
    o0c_wm_naviDraw_game();
    s32 hi = g_worldPrimPtr;
    g_worldTitleId = title;
    g_worldNaviMode = mode;
    g_worldNaviVisible = visible;
    if (hi != lo) {
        port_bottom_packets((const void *)lo, (const void *)hi);
        s32 x = 0, y = 0;
        if (zoomed && g_worldPlayer) o0c_wm_naviWorldToMap(g_worldPlayer->pos.vx, g_worldPlayer->pos.vz, &x, &y, g_worldNaviPage * 2 + 1);
        port_bottom_zoom((const void *)lo, (const void *)hi, x + 0x28, y + 9, zoomed ? 2 : 1); /* (where the arrow is) */
    }
}
