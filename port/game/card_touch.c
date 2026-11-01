/* The main menu's Card screen (overlay 36) by touch, directly: a tap on a cell of its 10x10 grid shows that card
 * (cursor set, the game's menuCard_selectCard, as the d-pad does); Cross there is Discard, so a tap doesn't confirm.
 * Cell of card id col*10 + row at (gridRect.x + col*15 + 4, gridRect.y + row*15 + 10), 13x13 (ff9-decomp
 * docs/r32/3ds-requests-4.md). */
#define M2CTX
#include "ovl_36.h"

void o36_menuCard_selectCard(s32 id);

/* a tap at (x, y) in the frame's coordinates: -1 taken, 0 not here */
int port_card_tap(int x, int y) {
    if (!g_menuScreen.active || g_menuScreen.screen != MENU_SCREEN_CARD) return 0;
    if (s_menuCard.state != CARD_ST_GRID || fade_isBusy()) return -1;
    RECT *g = &s_menuCard.gridRect;
    int col = (x - g->x - 3) / 15, row = (y - g->y - 9) / 15;
    if (x < g->x + 3 || y < g->y + 9 || col >= CARD_GRID_COLS || row >= CARD_GRID_ROWS) return -1;
    if (col == s_menuCard.col && row == s_menuCard.row) return -1;
    s_menuCard.col = (u8)col;
    s_menuCard.row = (u8)row;
    snd_playSe(SE_CURSOR);
    o36_menuCard_selectCard(col * CARD_GRID_ROWS + row);
    return -1;
}

/* The Card screen's top view: the card under the cursor large (its picture once the screen has loaded it, its arrows,
 * name and "1P2A" string, of the copy CARD INFO shows), and how many copies are owned; nothing owned there: the empty
 * boxes. tm_ui.c draws it (port_card_panel), as the card game's focus. */
void port_card_panel(int id, int arrows, int power, int type, int pdef, int mdef, int menu);
int port_card_top(void) {
    if (!g_menuScreen.active || g_menuScreen.screen != MENU_SCREEN_CARD) return 0;
    static RECT card_box = {16, 12, 150, 176}, info = {174, 12, 210, 176};
    int id = s_menuCard.col * CARD_GRID_ROWS + s_menuCard.row;
    if (id >= 0 && id < CARD_COUNT && s_menuCard.count[id]) {
        CardEntry *e = card_find(id, s_menuCard.shown[id] % s_menuCard.count[id]);
        if (e && e != (CardEntry *)-1)
            port_card_panel(id, e->arrows, e->power, e->type, e->physDef, e->magicDef, s_menuCard.cache.shown == id ? 1 : 2);
        u8 a[8];
        ui_drawLabel(info.x + 10, info.y + 64, 2, (s32) "OWNED", 0);
        text_formatNumber(s_menuCard.count[id], a, 2, TEXT_NUM_ASCII);
        ui_drawDigits(info.x + 56, info.y + 63, 2, a, 0);
    }
    win_drawBox(&card_box, 5, 2);
    win_drawBox(&info, 5, 2);
    return 1;
}
