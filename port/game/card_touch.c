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
