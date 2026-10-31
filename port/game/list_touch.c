/* Touch on the PS1 menus' lists (Item, Ability, Equip, shops, ... drawn 1:1 on the bottom screen), directly: every list
 * is drawn through win_drawListBox (wrapped: config/renames.txt), which tells the port where each list is and its
 * MenuList. A tap on a row of the list the game's hand points into sets that list's cursor (row and column) at once and
 * confirms it with the game's own Cross handling this frame; nothing walks the cursor. A list without the hand (another
 * window on screen) or scrolling is left alone. The Item screen's member window (using an item on someone) is no
 * list: a tap on a member there sets s_menuItem.slot and confirms the same way. */
#define M2CTX
#include "ovl_37.h" /* (the Item screen's member window, below; includes ff9.h) */

extern int ctr_ui_hands[][2], ctr_ui_nhands; /* render.c: the hands drawn last frame, in the frame's coordinates */
long VSync(long);

#define MAX_LISTS 8
static struct { RECT r; MenuList *m; int mode; long frame; } lists[MAX_LISTS];
static int nlists;

void win_drawListBox_game(RECT *r, s32 otz, s32 mode, MenuList *m);
void win_drawListBox(RECT *r, s32 otz, s32 mode, MenuList *m) {
    win_drawListBox_game(r, otz, mode, m);
    long now = VSync(-1);
    int k = 0;
    for (int i = 0; i < nlists; i++) /* (this frame's and the last one's) */
        if (now - lists[i].frame <= 2 && lists[i].m != m) lists[k++] = lists[i];
    nlists = k;
    if (nlists == MAX_LISTS) nlists--;
    lists[nlists].r = *r;
    lists[nlists].m = m;
    lists[nlists].mode = mode;
    lists[nlists].frame = now;
    nlists++;
}

static int hand_in(const RECT *r) {
    for (int i = 0; i < ctr_ui_nhands; i++) {
        int hx = ctr_ui_hands[i][0] + 23, hy = ctr_ui_hands[i][1] + 6; /* (the hand's tip) */
        if (hx >= r->x - 4 && hx < r->x + r->w && hy >= r->y && hy < r->y + r->h) return 1;
    }
    return 0;
}

/* a tap at (x, y) in the frame's coordinates: 1 with the confirm press in *press, -1 taken, 0 not on a list */
int port_list_tap(int x, int y, unsigned *press) {
    long now = VSync(-1);
    if (g_menuScreen.active && g_menuScreen.screen == MENU_SCREEN_ITEM && s_menuItem.charWinOpen) {
        RECT *r = &s_menuItem.charWin.rect;
        if (x < r->x || x >= r->x + r->w || y < r->y || y >= r->y + r->h) return -1;
        int slot = (y - r->y - 6) / 0x2B;
        if (slot < 0 || slot > 3 || !g_gameState->gameData->party[slot] || s_menuItem.charWin.anim) return -1;
        s_menuItem.slot = (u8)slot;
        *press = 0x4000;
        return 1;
    }
    for (int i = nlists - 1; i >= 0; i--) {
        RECT *r = &lists[i].r;
        MenuList *m = lists[i].m;
        if (now - lists[i].frame > 2 || x < r->x || x >= r->x + r->w || y < r->y || y >= r->y + r->h) continue;
        if (!m->cols || !m->visibleRows || m->scroll || !hand_in(r)) return 0;
        /* (MenuList.height is the rows' whole height: 113 for seven 15-pixel rows and the box's margins) */
        int top = r->y + (lists[i].mode == 1 ? 6 : 2), row_h = (m->height - 8) / m->visibleRows;
        if (row_h < 8) row_h = 15;
        int row = (y - top) / row_h, col = (x - r->x) * m->cols / r->w;
        if (row < 0) row = 0;
        if (row >= m->visibleRows) row = m->visibleRows - 1;
        if ((m->top + row) * m->cols + col >= m->count) return -1;
        m->cursor = (u8)row;
        m->col = (u8)col;
        *press = 0x4000; /* (cross: the game's own confirm for its list) */
        return 1;
    }
    return 0;
}
