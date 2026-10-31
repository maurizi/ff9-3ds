/* The save screen (menu type 4, overlay 39) by touch, directly: a tap on a memory card slot, a file, or Yes/No in the
 * confirm window puts the game's cursor there (g_menu.cardSlot, s_menuSave.list, s_menuSaveConfirm.choice) and
 * confirms with its own Cross handling; overwriting still asks the game's Yes/No first. Rows as menuSave_drawCursor
 * places its hand: slots 0x12 apart from y 0x6D, files SAVE_ROW_H apart from y 0x42, Yes/No 15 apart. */
#define M2CTX
#include "ovl_39.h"

/* a tap at (x, y) in the frame's coordinates: 1 with the confirm press in *press, -1 taken, 0 not here */
int port_save_tap(int x, int y, unsigned *press) {
    if (!g_gameState || g_menu.state != 3 || s_menuSave.phase != SAVE_PH_READY || fade_isBusy()) return 0;
    if (g_menu.type != 4) return 0; /* (the moogle's save screen) */
    switch (s_menuSave.state) {
    case SAVE_ST_SLOT:
        if (x < 0x74 || x >= 0x74 + 0x90 || y < 0x66 || y >= 0x66 + 2 * 0x12) return -1;
        g_menu.cardSlot = (u8)((y - 0x66) / 0x12);
        break;
    case SAVE_ST_LIST: {
        MenuList *l = &s_menuSave.list;
        int row = (y - 0x38) / SAVE_ROW_H;
        if (l->scroll || y < 0x38 || row >= l->visibleRows || l->top + row >= l->count) return -1;
        l->cursor = (u8)row;
        break;
    }
    case SAVE_ST_CONFIRM: {
        Window *w = s_menuSaveConfirm.win;
        if (!w || w->state != WIN_STATE_OPEN || x < w->x || x >= w->x + w->w || y < w->y || y >= w->y + w->h) return -1;
        int y0 = w->y + w->h - 0x23;
        s_menuSaveConfirm.choice = y >= y0 + 15 - 4;
        break;
    }
    default:
        return -1;
    }
    *press = 0x4000;
    return 1;
}
