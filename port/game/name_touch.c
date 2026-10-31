/* Touch on the name-entry screen (menu type 1, overlay 3A), drawn 1:1 on the bottom screen: a tap on a letter enters
 * that letter, a tap on a side-menu row (Alphabet, Symbol, Delete, Confirm, Default) picks it. The tap becomes the
 * presses that get there from where the game's cursor is (decomp src/ovl_3a: the grid is NAME_GRID_COLS letters per
 * row, 14 px apart, rows 21 px apart, from the name window's (x + 0x3E, y + 0x22); the side menu's rows 16 px apart,
 * the action rows 0x28 lower), then cross, so the game's own input code does the entering. */
#define M2CTX
#include "ovl_3a.h"

enum { P_UP = 0x10, P_RIGHT = 0x20, P_DOWN = 0x40, P_LEFT = 0x80, P_CROSS = 0x4000 };

static int push(unsigned *seq, int n, int max, unsigned b, int times) {
    while (times-- > 0 && n < max - 1) seq[n++] = b;
    return n;
}

/* presses for a tap at (x, y) in the frame's coordinates: their count, 0 when the tap isn't on name entry */
int port_name_tap(int x, int y, unsigned *seq, int max) {
    if (!g_gameState || g_menu.type != 1 || g_menu.state != 3) return 0; /* (running) */
    NameEntry *s = &s_menuName;
    const RECT *g = &s->windows[4], *m = &s->windows[2];
    int n = 0;
    /* a letter */
    int gx0 = g->x + 0x3E - 2, gy0 = g->y + 0x22 - 4;
    int rows = s->gridSize[s->charSet] / NAME_GRID_COLS;
    if (x >= gx0 && x < gx0 + NAME_GRID_COLS * 14 && y >= gy0 && y < gy0 + rows * 21) {
        int col = (x - gx0) / 14, row = (y - gy0) / 21, cur = s->gridCursor;
        if (s->focus == NAME_FOCUS_MENU) n = push(seq, n, max, P_RIGHT, 1); /* (into the grid, at its cursor) */
        int dr = row - cur / NAME_GRID_COLS, dc = col - cur % NAME_GRID_COLS;
        n = push(seq, n, max, dr > 0 ? P_DOWN : P_UP, dr > 0 ? dr : -dr);
        n = push(seq, n, max, dc > 0 ? P_RIGHT : P_LEFT, dc > 0 ? dc : -dc);
        seq[n++] = P_CROSS;
        return n;
    }
    /* a side-menu row */
    if (x >= m->x && x < m->x + m->w && y >= m->y && y < m->y + m->h) {
        int row = -1;
        for (int i = 0; i < 5; i++) {
            int ry = m->y + (i < 2 ? 9 + i * 16 : 0x31 + (i - 2) * 16) - 3;
            if (y >= ry && y < ry + 16) row = i;
        }
        if (row < 0) return -1;
        if (s->focus == NAME_FOCUS_GRID) /* (left from the grid's first column goes to the menu) */
            n = push(seq, n, max, P_LEFT, s->gridCursor % NAME_GRID_COLS + 1);
        int d = row - s->menuCursor; /* (the menu wraps: the shorter way) */
        if (d > 2) d -= 5;
        if (d < -2) d += 5;
        n = push(seq, n, max, d > 0 ? P_DOWN : P_UP, d > 0 ? d : -d);
        seq[n++] = P_CROSS;
        return n;
    }
    return -1; /* (elsewhere on the screen: nothing, rather than steering the hand) */
}
