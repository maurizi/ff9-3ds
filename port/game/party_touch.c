/* Party select (Form Party, menu type 3) by touch, directly: a tap on a party row or on a reserve portrait puts the
 * game's cursor there and runs its own pick (Cross): the first tap takes that member up, the second swaps them with
 * the slot tapped (as the cursor and Cross do; a member who must stay is refused by the game's own check). A tap on
 * the "Form Party" title confirms the party (Circle: the game checks it, and in the middle of a swap Circle backs
 * out of it instead, as on the PS1). */
#define M2CTX
#include "ovl_3b.h"

static int in(const RECT *r, int x, int y, int dx, int dy, int w, int h) {
    return x >= r->x + dx && x < r->x + dx + w && y >= r->y + dy && y < r->y + dy + h;
}

/* a tap at (x, y) in the frame's coordinates: 1 with a press in *press, -1 taken, 0 not here */
int port_party_tap(int x, int y, unsigned *press) {
    if (!g_gameState || g_menu.state != 3 || g_menu.type != MENU_PARTY || fade_isBusy()) return 0;
    if (in(&s_menuPartyTitleRect, x, y, 0, 0, s_menuPartyTitleRect.w, s_menuPartyTitleRect.h)) {
        *press = 0x2000; /* (Circle) */
        return 1;
    }
    int row = -1, idx = 0;
    for (int i = 0; i < 4 && row < 0; i++) /* (the party: rows 0x33 apart from +6; empty slots in the size too) */
        if (i < s_menuParty.size && in(&s_menuPartyListRect, x, y, 0, 6 + i * 0x33, s_menuPartyListRect.w, 0x33))
            row = PARTY_ROW_PARTY, idx = i;
    for (int i = 0; i < 8 && row < 0; i++) /* (the reserve: portraits 0x25 x 0x30 apart, 4 a line) */
        if (s_menuParty.slots[PARTY_ROW_RESERVE][i] != PARTY_EMPTY &&
            in(&s_menuPartyInfoRect, x, y, (i & 3) * 0x25 + 0x10 - 2, (i >> 2) * 0x30 + 0x3A - 2, 0x24, 0x30))
            row = PARTY_ROW_RESERVE, idx = i;
    if (row < 0) return -1;
    Cursor *c = &s_menuParty.cursor[s_menuParty.mode];
    if (c->row != row) { /* (the game keeps each row's place as the cursor crosses: the same here) */
        if (c->row == PARTY_ROW_PARTY) s_menuParty.partyIdx = c->idx;
        else s_menuParty.reserveIdx = c->idx;
    }
    c->row = (u8)row;
    c->idx = (u8)idx;
    *press = 0x4000; /* (Cross: menuParty_pick) */
    return 1;
}
