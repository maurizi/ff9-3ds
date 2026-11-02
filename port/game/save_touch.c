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

/* No memory card slots on a 3DS (the saves are files on the SD card, the port's slot 1): the screen goes straight to
 * the file list and leaves straight from it.
 *   - the slot window is never drawn (menuSave_drawSlotWindow, the slot and check states' draw);
 *   - at the slot state the first time, slot 1 is chosen with the game's own select as soon as its card answers;
 *   - back at the slot state after that (an error notice closed, or the list left), the screen closes as Circle
 *     on the slots would close it;
 *   - Circle in the file list closes the screen at once, instead of fading back to the slots.
 * (menuSave_init, _inputSlot, _inputFileList and _drawSlotWindow wrapped: config/renames.txt; the last three are
 * reached through the screen's state tables.) */
void o39_menuSave_init_game(void), o39_menuSave_inputSlot_game(void), o39_menuSave_inputFileList_game(void);
void o39_menuSave_selectSlot(void), o39_menuSave_close(void);
u8 o39_menuSave_isSlotReady(s32 port, u8 allowNew);
static int chose;

static void leave(void) { /* (Circle on the slots: the game's own way out) */
    snd_playSe(SE_CANCEL);
    if (g_menu.type != 0) o39_menuSave_close();
    else menuScreen_open(0);
}
void o39_menuSave_init(void) {
    o39_menuSave_init_game();
    chose = 0;
}
void o39_menuSave_drawSlotWindow(void) {}
void o39_menuSave_inputSlot(void) {
    if (fade_isBusy()) return;
    if (chose) { leave(); return; }
    s_menuSave.showCursor = 0; /* (no hand on the hidden slots) */
    if (!o39_menuSave_isSlotReady(0, 1)) return; /* (its card still being looked at) */
    chose = 1;
    g_menu.cardSlot = 0;
    s_menuSave.showCursor = 1;
    o39_menuSave_selectSlot();
}
void o39_menuSave_inputFileList(void) {
    if (!s_menuSave.list.scroll && (g_padRepeat & BTN_CIRCLE)) { leave(); return; }
    o39_menuSave_inputFileList_game();
}
