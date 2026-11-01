/* The Chocograph screen (menu type 5: the chocographs owned, the one being dug for, each one's picture and clue) by
 * touch, directly, and its top-screen view.
 *   tap a tab (Select / Cancel): that tab, confirmed (Cross)
 *   tap a list row: the list's cursor there (its hint and picture show); the row already under the cursor: chosen
 *   (Cross: the game's own check of what is owned and the chocobo's ability)
 *   tap the title: back from the list to the tabs, or out of the screen (Circle)
 * Top screen: the highlighted chocograph (in the list) or the one chosen (on the tabs) large: its picture in the
 * game's frame, name, clue and the ability it needs, drawn with the screen's own code. */
#define M2CTX
#include "ovl_32.h"

void o32_menuChocograph_drawPicture(s32, s32, s32, s32, u8);
void o32_menuChocograph_drawFrame(s32, s32, s32, u8);
void o32_menuChocograph_drawIcon(s32, s32, s32, s32, u8);
s32 o32_menuChocograph_getRequiredAbility(s32);

static int open_now(void) {
    return g_gameState && g_menu.state == 3 && g_menu.type == MENU_CHOCOGRAPH && s_menuChocograph.mode != CHOCO_MODE_NONE;
}

static int in(const RECT *r, int x, int y) { return x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h; }

/* a tap at (x, y) in the frame's coordinates: 1 with a press in *press, -1 taken, 0 not here */
int port_chocograph_tap(int x, int y, unsigned *press) {
    if (!open_now() || fade_isBusy()) return 0;
    ChocographMenu *st = &s_menuChocograph;
    MenuList *l = &st->list;
    if (in(&st->titleRect, x, y)) {
        *press = 0x2000; /* (Circle) */
        return 1;
    }
    if (in(&st->tabRect, x, y)) {
        st->mode = CHOCO_MODE_TABS;
        st->tab = x < st->tabRect.x + 0x60 ? CHOCO_TAB_SELECT : CHOCO_TAB_CANCEL;
        *press = 0x4000; /* (Cross) */
        return 1;
    }
    if (in(&st->listRect, x, y)) {
        if (l->scroll || !l->visibleRows) return -1;
        int row_h = (l->height - 8) / l->visibleRows;
        if (row_h < 8) row_h = 15;
        int row = (y - st->listRect.y - 6) / row_h;
        if (row < 0) row = 0;
        if (row >= l->visibleRows) row = l->visibleRows - 1;
        if (l->top + row >= l->count) return -1;
        if (st->mode == CHOCO_MODE_LIST && row == l->cursor) {
            *press = 0x4000;
            return 1;
        }
        if (st->mode != CHOCO_MODE_LIST || row != l->cursor) snd_playSe(SE_CURSOR);
        st->mode = CHOCO_MODE_LIST;
        l->cursor = (u8)row;
        return -1;
    }
    return -1;
}

/* the top screen's view (menu_top.c): 1 if drawn */
int port_chocograph_top(void) {
    if (!open_now()) return 0;
    ChocographMenu *st = &s_menuChocograph;
    int id = st->mode == CHOCO_MODE_LIST ? st->list.top + st->list.cursor : g_gameState->gameData->chocograph - 1;
    static RECT box = {8, 8, 384, 224};
    if (id < 0 || id >= CHOCOGRAPH_COUNT || !(st->owned & (1u << id))) {
        win_drawBox(&box, 4, WIN_STYLE_BEVEL); /* (none owned there: the panel, empty) */
        return 1;
    }
    o32_menuChocograph_drawPicture(28, 28, 2, id, 0);
    o32_menuChocograph_drawFrame(24, 24, 2, 0);
    text_drawBlock(112, 28, 2, text_get(id + CHOCO_TEXT_NAME), TEXT_PAL_WHITE);
    if (st->found & (1u << id)) ui_drawLabel(112, 46, 2, (s32) "TREASURE FOUND", 0);
    text_drawBlock(112, 64, 2, text_get(CHOCO_TEXT_ABILITY), TEXT_PAL_WHITE);
    int need = o32_menuChocograph_getRequiredAbility(id);
    for (int i = 0; i <= need; i++)
        o32_menuChocograph_drawIcon(112 + i * 0x13, 80, 2, i < st->ability ? i : CHOCO_ICON_LOCKED, i >= st->ability);
    text_drawBlock(28, 112, 2, text_get(id + CHOCO_TEXT_CLUE), TEXT_PAL_WHITE);
    win_drawBox(&box, 4, WIN_STYLE_BEVEL);
    return 1;
}
