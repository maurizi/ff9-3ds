/* Touch on the name-entry screen (menu type 1, overlay 3A), drawn 1:1 on the bottom screen: a tap on a letter enters
 * that letter, a tap on a side-menu row (Alphabet, Symbol, Delete, Confirm, Default) does it, at once. The tap puts
 * the game's cursor there (s_menuName's focus and grid or menu cursor, as its own d-pad code would leave them) and runs
 * the game's own confirm (menuName_confirm: the letter, the sound, the name's length rules), so the screen behaves as
 * if the cursor had always been there. Layout (decomp src/ovl_3a, include/ovl_3a.h): the grid is NAME_GRID_COLS letters
 * per row from the name window's (x + NAME_GRID_X, y + NAME_GRID_Y), JP's kana in three groups of five; the side
 * menu's character-set rows (two, JP three: katakana, hiragana, alphabet), then its action rows. */
#define M2CTX
#include "ovl_3a.h"

void o3a_menuName_confirm(void); /* (overlay 3A's, compiled natively) */

/* a tap at (x, y) in the frame's coordinates: -1 handled, 0 not name entry */
int port_name_tap(int x, int y, unsigned *seq, int max) {
    (void)seq;
    (void)max;
    if (!g_gameState || g_menu.type != 1 || g_menu.state != 3) return 0;
    if (fade_isBusy()) return -1; /* (the game takes no input while fading either) */
    NameEntry *s = &s_menuName;
    const RECT *g = &s->windows[4], *m = &s->windows[2];
    int gx0 = g->x + NAME_GRID_X - 2, gy0 = g->y + NAME_GRID_Y - 4;
    int rows = s->gridSize[s->charSet] / NAME_GRID_COLS;
#if VERSION_JP
    /* JP's kana sets: three groups of 5 per row, NAME_GROUP_W apart, NAME_GROUP_CELL_W inside a group */
    int grouped = s->charSet != NAME_MENU_ALPHABET;
#else
    int grouped = 0;
#endif
    int gw = grouped ? (NAME_GRID_COLS / NAME_GROUP_COLS) * NAME_GROUP_W : NAME_GRID_COLS * NAME_GRID_CELL_W;
    if (x >= gx0 && x < gx0 + gw && y >= gy0 && y < gy0 + rows * NAME_GRID_ROW_H) { /* a letter */
        int col;
        if (grouped) {
            int grp = (x - gx0) / NAME_GROUP_W, sub = (x - gx0 - grp * NAME_GROUP_W) / NAME_GROUP_CELL_W;
            col = grp * NAME_GROUP_COLS + (sub < NAME_GROUP_COLS ? sub : NAME_GROUP_COLS - 1);
        } else {
            col = (x - gx0) / NAME_GRID_CELL_W;
        }
        int cell = ((y - gy0) / NAME_GRID_ROW_H) * NAME_GRID_COLS + col;
        if (cell >= s->gridSize[s->charSet]) return -1;
        s->focus = NAME_FOCUS_GRID;
        s->gridCursor = (u8)cell;
        help_setBounds(&s_menuNameHelpBounds);
        o3a_menuName_confirm();
        return -1;
    }
    if (x >= m->x && x < m->x + m->w && y >= m->y && y < m->y + m->h) { /* a side-menu row */
        for (int i = 0; i <= NAME_MENU_DEFAULT; i++) {
            int ry = m->y + (i < NAME_CHARSETS ? NAME_CMD_SET_Y + i * NAME_CMD_ROW_H
                                               : NAME_CMD_ACTION_Y + (i - NAME_CHARSETS) * NAME_CMD_ROW_H) - 3;
            if (y < ry || y >= ry + NAME_CMD_ROW_H) continue;
            s->focus = NAME_FOCUS_MENU;
            s->menuCursor = (u8)i;
            help_setBounds(NULL);
            o3a_menuName_confirm();
            break;
        }
    }
    return -1; /* (elsewhere on the screen: nothing) */
}
