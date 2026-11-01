/* The world map's bottom screen (docs/ui-design.md): the full navigation map (the PS1's Select view: the continents,
 * the places found, the party and its vehicle) is always there, and the top screen keeps the world without the corner
 * minimap. wm_naviDraw is wrapped (config/renames.txt): it draws the full map's packets whatever mode the game's
 * navigation is in, and their range goes to the bottom screen (port_bottom_packets). The location title it also
 * draws stays in the scene. The game's own full-map mode (its place cursor) is shown the same way. While a question is
 * open (the moogle's, the chocobo's menu, the airship's) its choices take the map's place. A tap near a place
 * found names it (field_hud.c draws the name); elsewhere it zooms in 2x about the party, and out again. */
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
static s32 zoom_cx, zoom_cy; /* (the party's map position, the zoom's centre) */
static int named = -1, named_frames; /* the place a tap named, and for how long more */

s32 o0c_wm_naviIsPlaceKnown(u32);
/* whether place i of the page has been found (the game's own test, as its cursor uses it; page 1 has no 6-9 and 15) */
static int place_known(int page, int i) {
    if (page == 1 && ((unsigned)(i - 6) < 4 || i == 15)) return 0;
    const WmNaviPlace *p = &g_worldNaviPlaces[page][i];
    if (!p->mapX && !p->mapY) return 0;
    return (u8)o0c_wm_naviIsPlaceKnown((u32)i) != 0;
}

/* where on the bottom screen place i's marker is (the map draws it at (mapX + 40, mapY + 9); zoomed: about the party) */
static void place_pos(int page, int i, int *x, int *y) {
    const WmNaviPlace *p = &g_worldNaviPlaces[page][i];
    *x = p->mapX + 40;
    *y = p->mapY + 9;
    if (zoomed) *x = 160 + (*x - zoom_cx) * 2, *y = 112 + (*y - zoom_cy) * 2;
}

/* A tap on the bottom-screen map at (x, y) of its 320x224: near a place found, its name; elsewhere, the zoom. */
int port_world_tap(int x, int y) {
    int page = g_worldNaviPage, best = -1, bd = 14 * 14;
    for (int i = 0; i < 64; i++) {
        if (!place_known(page, i)) continue;
        int px, py;
        place_pos(page, i, &px, &py);
        int d = (px - x) * (px - x) + (py - y) * (py - y);
        if (d < bd) bd = d, best = i;
    }
    if (best >= 0) {
        named = best;
        named_frames = 180;
        return 1;
    }
    zoomed = !zoomed;
    named = -1;
    return 1;
}

/* the named place's name (the world's text bank 0: a count, u16 offsets from after it, names ending 0xF7; entry k is
 * place k - 1), as text the game draws; NULL when none */
static const u8 *port_world_place_name(int *x, int *y) {
    static u8 name[48];
    if (named < 0 || named_frames <= 0) return NULL;
    named_frames--;
    const u8 *bank = g_msgTextBanks[0];
    if (!bank || named + 1 >= bank[0]) return NULL;
    const u8 *offs = bank + 1;
    const u8 *t = bank + 1 + (offs[(named + 1) * 2] | offs[(named + 1) * 2 + 1] << 8);
    int n = 0;
    while (n < 46 && t[n] != 0xF7 && t[n] != 0xFF) name[n] = t[n], n++;
    name[n] = 0xFF;
    place_pos(g_worldNaviPage, named, x, y);
    return name;
}

static const u8 *port_world_place_name(int *x, int *y);
void o0c_wm_naviDraw(void) {
    if (g_worldTitleId >= 0) o0c_wm_drawTitle();
    if (!g_worldNaviActive) return;
    { /* a question open (the moogle's, the chocobo's menu, the airship's): its choices as buttons on the bottom screen
       * in the map's place (field_hud.c draws them), in the world's own frame as the map would be */
        int port_world_choices(void);
        u32 *saved_ot = g_ot;
        u8 *saved_prim = g_primPtr;
        g_ot = (u32 *)g_worldOt[*g_worldBufIndex];
        g_primPtr = (u8 *)g_worldPrimPtr;
        s32 lo2 = g_worldPrimPtr;
        int drew = port_world_choices();
        g_worldPrimPtr = (s32)g_primPtr;
        g_ot = saved_ot;
        g_primPtr = saved_prim;
        if (drew) {
            port_bottom_packets((const void *)lo2, (const void *)g_worldPrimPtr);
            return;
        }
    }
    s8 title = g_worldTitleId;
    s32 mode = g_worldNaviMode;
    u8 visible = g_worldNaviVisible;
    g_worldTitleId = -1;
    if (mode != 2) g_worldNaviMode = 2, g_worldNaviVisible = 1;
    /* (its place cursor is drawn by the text pen into the main ordering table, not the map's packets: in the scene on
     * the top screen. Not drawn by the game: in its own full-map mode (Select) it is drawn below into the map's
     * packets, on the bottom with the map; walking, the game leaves its flag set though no cursor is shown) */
    u8 cursor = g_worldNaviCursorDraw;
    g_worldNaviCursorDraw = 0;
    s32 lo = g_worldPrimPtr;
    o0c_wm_naviDraw_game();
    g_worldNaviCursorDraw = cursor;
    if (mode == 2 && visible && cursor) {
        u32 *saved_ot = g_ot;
        u8 *saved_prim = g_primPtr;
        g_ot = (u32 *)g_worldOt[*g_worldBufIndex];
        g_primPtr = (u8 *)g_worldPrimPtr;
        msg_setOtz(1); /* (in front of the map) */
        pen_begin();
        pen_setPos((g_worldNaviCursorX >> 12) + 18, (g_worldNaviCursorY >> 12) + 6);
        pen_drawSprite(1);
        pen_endSmall();
        msg_setOtz(2);
        g_worldPrimPtr = (s32)g_primPtr;
        g_ot = saved_ot;
        g_primPtr = saved_prim;
    }
    s32 hi = g_worldPrimPtr;
    g_worldTitleId = title;
    g_worldNaviMode = mode;
    g_worldNaviVisible = visible;
    if (hi != lo) {
        port_bottom_packets((const void *)lo, (const void *)hi);
        s32 x = 0, y = 0;
        if (zoomed && g_worldPlayer) o0c_wm_naviWorldToMap(g_worldPlayer->pos.vx, g_worldPlayer->pos.vz, &x, &y, g_worldNaviPage * 2 + 1);
        zoom_cx = x + 0x28;
        zoom_cy = y + 9;
        port_bottom_zoom((const void *)lo, (const void *)hi, zoom_cx, zoom_cy, zoomed ? 2 : 1); /* (where the arrow is) */
        /* a place a tap named: its name in a box by it, drawn with the game's text code into the world's own frame
         * (its packets a range of their own: not zoomed) */
        int px, py;
        const u8 *name = port_world_place_name(&px, &py);
        if (name) {
            u32 *saved_ot = g_ot;
            u8 *saved_prim = g_primPtr;
            g_ot = (u32 *)g_worldOt[*g_worldBufIndex];
            g_primPtr = (u8 *)g_worldPrimPtr;
            int w = text_measure((u8 *)name).w + 16;
            RECT r = {(s16)(px - w / 2), (s16)(py - 30), (s16)w, 22};
            if (r.x < 4) r.x = 4;
            if (r.x + r.w > 316) r.x = (s16)(316 - r.w);
            if (r.y < 4) r.y = (s16)(py + 10);
            text_drawBlock(r.x + 8, r.y + 5, 1, (u8 *)name, 0);
            win_drawBox(&r, 2, 2);
            s32 lo2 = g_worldPrimPtr;
            g_worldPrimPtr = (s32)g_primPtr;
            g_ot = saved_ot;
            g_primPtr = saved_prim;
            port_bottom_packets((const void *)lo2, (const void *)g_worldPrimPtr);
        }
    }
}
