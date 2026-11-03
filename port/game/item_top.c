/* The Item screen's top view: the item under the list's cursor (equipment: who can wear it and what it changes;
 * anything else: its description, how many are held and the party's HP / MP) or the key item under it (its name and
 * description), drawn by menu_top.c's panels. */
#define M2CTX
#include "ovl_37.h"

int port_item_panel(int id, u8 *help);
int port_key_item_panel(u8 *name, u8 *help);
static int empty(void) {
    static RECT box = {8, 8, 384, 224};
    win_drawBox(&box, 5, 2);
    return 1;
}

/* (the list's states, and BUSY while one of them is the last shown: a window sliding open or shut over the list, the
 * key item's picture, keeps the panel instead of flicking to the scene) */
static int last_shown;
int port_item_top_describes(void) { /* (menu_top.c: Select's help would repeat the top's description) */
    ItemMenu *m = &s_menuItem;
    int st = m->state;
    if (st == ITEM_STATE_BUSY) return last_shown;
    if (m->tab == ITEM_TAB_KEY) return st == ITEM_STATE_LIST || st == ITEM_STATE_KEY_VIEW;
    return st == ITEM_STATE_LIST || st == ITEM_STATE_TARGET || st == ITEM_STATE_MANUAL;
}

int port_item_top(void) {
    ItemMenu *m = &s_menuItem;
    GameData *g = g_gameState->gameData;
    if (!(last_shown = port_item_top_describes())) return 0;
    if (m->tab == ITEM_TAB_KEY) {
        MenuList *l = &m->keyList;
        int i = (l->top + l->cursor) * l->cols + l->col;
        if (i < 0 || i >= m->keyCount) return empty();
        KeyItemInfo *k = &g_keyItemTable[m->keyItems[i]];
        return port_key_item_panel(g_keyItemNames + k->name, &s_menuItemKeyHelp[k->help]);
    }
    MenuList *l = &m->itemList;
    int slot = (l->top + l->cursor) * 2 + l->col;
    if (slot < 0 || slot > 255 || !g->items.slots[slot].count) return empty();
    int id = g->items.slots[slot].id;
    return port_item_panel(id, &s_menuItemHelp[g_itemTable[id].help]);
}
