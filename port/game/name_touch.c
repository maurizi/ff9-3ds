/* Touch on the name-entry screen (menu type 1, overlay 3A), drawn 1:1 on the bottom screen: a tap on a letter enters
 * that letter, a tap on a side-menu row (Alphabet, Symbol, Delete, Confirm, Default) does it, at once. The tap puts
 * the game's cursor there (s_menuName's focus and grid or menu cursor, as its own d-pad code would leave them) and runs
 * the game's own confirm (menuName_confirm: the letter, the sound, the name's length rules), so the screen behaves as
 * if the cursor had always been there. Layout (decomp src/ovl_3a): the grid is NAME_GRID_COLS letters per row, 14 px
 * apart, rows 21 px apart, from the name window's (x + 0x3E, y + 0x22); the side menu's rows are 16 px apart, the
 * action rows 0x28 lower. */
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
    int gx0 = g->x + 0x3E - 2, gy0 = g->y + 0x22 - 4;
    int rows = s->gridSize[s->charSet] / NAME_GRID_COLS;
    if (x >= gx0 && x < gx0 + NAME_GRID_COLS * 14 && y >= gy0 && y < gy0 + rows * 21) { /* a letter */
        int cell = ((y - gy0) / 21) * NAME_GRID_COLS + (x - gx0) / 14;
        if (cell >= s->gridSize[s->charSet]) return -1;
        s->focus = NAME_FOCUS_GRID;
        s->gridCursor = (u8)cell;
        help_setBounds(&s_menuNameHelpBounds);
        o3a_menuName_confirm();
        return -1;
    }
    if (x >= m->x && x < m->x + m->w && y >= m->y && y < m->y + m->h) { /* a side-menu row */
        for (int i = 0; i < 5; i++) {
            int ry = m->y + (i < 2 ? 9 + i * 16 : 0x31 + (i - 2) * 16) - 3;
            if (y < ry || y >= ry + 16) continue;
            s->focus = NAME_FOCUS_MENU;
            s->menuCursor = (u8)i;
            help_setBounds(NULL);
            o3a_menuName_confirm();
            break;
        }
    }
    return -1; /* (elsewhere on the screen: nothing) */
}
