/* The Equip screen's slots by touch, directly (its slot column isn't a list box): a tap on a slot puts the game's
 * slot cursor there (s_menuEquip.equipSlot, picking slots) and confirms, so the slot's candidates open in one tap; the
 * candidates are a list box (list_touch.c). Rows 17 px apart from equipRect.y (menuEquip_drawCursor). */
#define M2CTX
#include "ovl_33.h"

/* a tap at (x, y) in the frame's coordinates: 1 with the confirm press in *press, -1 taken, 0 not here */
int port_equip_tap(int x, int y, unsigned *press) {
    if (!g_menuScreen.active || g_menuScreen.screen != MENU_SCREEN_EQUIP) return 0;
    RECT *r = &s_menuEquip.equipRect;
    if (x < r->x || x >= r->x + r->w || y < r->y + 4 || y >= r->y + 4 + EQUIP_SLOT_COUNT * 17) return 0;
    if (s_menuEquip.state != EQUIP_STATE_SLOT && s_menuEquip.state != EQUIP_STATE_TABS) return 0;
    s_menuEquip.state = EQUIP_STATE_SLOT;
    s_menuEquip.equipSlot = (u8)((y - r->y - 4) / 17);
    *press = 0x4000;
    return 1;
}

/* The Ability screen's use-on window (a Cure out of battle), as the Item screen's: a tap on a member there sets
 * s_menuAbility.target (or, aimed at all, takes the whole party) and confirms. Members 0x2B apart (menuAbility_
 * drawTargetWindow). */
int port_ability_tap(int x, int y, unsigned *press) {
    if (!g_menuScreen.active || g_menuScreen.screen != MENU_SCREEN_ABILITY) return 0;
    /* the list (also from the tabs: as Cross on an enabled tab, then the entry), rows 15 apart, columns 150 */
    RECT *lr = &s_menuAbility.listRect;
    if ((s_menuAbility.state == ABILITY_STATE_TABS || s_menuAbility.state == ABILITY_STATE_LIST) && x >= lr->x &&
        x < lr->x + lr->w && y >= lr->y + 3 && y < lr->y + lr->h) {
        MenuList *l = &s_menuAbility.lists[s_menuAbility.tab];
        int row = (y - lr->y - 3) / 15, col = (x - lr->x) / 150;
        if (!s_menuAbility.tabEnabled[s_menuAbility.tab] || l->scroll || row >= l->visibleRows || col >= l->cols ||
            (l->top + row) * l->cols + col >= l->count)
            return -1;
        s_menuAbility.state = ABILITY_STATE_LIST;
        l->cursor = (u8)row;
        l->col = (u8)col;
        *press = 0x4000;
        return 1;
    }
    /* the tabs (Use / Equip): that tab */
    RECT *tr = &s_menuAbility.tabRect;
    if (s_menuAbility.state == ABILITY_STATE_TABS && x >= tr->x && x < tr->x + tr->w && y >= tr->y && y < tr->y + tr->h) {
        int tab = x >= tr->x + (s_menuAbilityTabCursorX[0][0] + s_menuAbilityTabCursorX[1][0]) / 2;
        if (tab != s_menuAbility.tab) snd_playSe(SE_CURSOR), s_menuAbility.tab = (u8)tab;
        return -1;
    }
    if (s_menuAbility.state != ABILITY_STATE_TARGET) return 0;
    RECT *r = &s_menuAbility.targetWin.rect;
    if (s_menuAbility.targetWin.anim || x < r->x || x >= r->x + r->w || y < r->y || y >= r->y + r->h) return -1;
    if (!(s_menuAbility.target & ABILITY_TARGET_ALL)) {
        int slot = (y - r->y - 6) / 0x2B;
        if (slot < 0 || slot > 3 || !g_gameState->gameData->party[slot]) return -1;
        s_menuAbility.target = (u8)slot;
    }
    *press = 0x4000;
    return 1;
}
