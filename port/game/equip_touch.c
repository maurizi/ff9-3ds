/* The Equip screen's slots by touch, directly (its slot column isn't a list box): a tap on a slot puts the game's
 * slot cursor there (s_menuEquip.equipSlot, picking slots) and confirms, so the slot's candidates open in one tap; the
 * candidates are a list box (list_touch.c). Rows 17 px apart from equipRect.y (menuEquip_drawCursor). */
#define M2CTX
#include "ovl_33.h"

/* a tap at (x, y) in the frame's coordinates: 1 with the confirm press in *press, -1 taken, 0 not here */
int port_equip_tap(int x, int y, unsigned *press) {
    if (!g_menuScreen.active || g_menuScreen.screen != MENU_SCREEN_EQUIP) return 0;
    RECT *r = &s_menuEquip.equipRect;
    if (x < r->x || x >= r->x + r->w || y < r->y + 4 || y >= r->y + 4 + EQUIP_SLOT_COUNT * 17) return 0;
    if (s_menuEquip.state != EQUIP_STATE_SLOT && s_menuEquip.state != EQUIP_STATE_TABS) return 0;
    s_menuEquip.state = EQUIP_STATE_SLOT;
    s_menuEquip.equipSlot = (u8)((y - r->y - 4) / 17);
    *press = 0x4000;
    return 1;
}
