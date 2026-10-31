/* The Config screen by touch, directly: a tap on a row's left or right option sets that row's setting as Left/Right
 * would (the cursor put on the row, the game's menuConfig_setOption: choices take that side, sliders step that way);
 * a tap on the Controller or Battle Control row's name opens its window (Cross). Rows 0x14 apart from y 0x2A, the two
 * options from x 0x98, 0x5A apart (menuConfig_drawRows, menuConfig_drawChoice). */
#define M2CTX
#include "ovl_34.h"

void o34_menuConfig_setOption(u8 v);

/* a tap at (x, y) in the frame's coordinates: 1 with a press in *press, -1 taken, 0 not here */
int port_config_tap(int x, int y, unsigned *press) {
    if (!g_menuScreen.active || g_menuScreen.screen != MENU_SCREEN_CONFIG) return 0;
    MenuList *l = &s_menuConfig.list;
    if (s_menuConfig.editing || s_menuConfig.sliderDir || l->scroll || fade_isBusy()) return -1;
    int row = (y - 0x2A) / 0x14;
    if (y < 0x2A || row >= l->visibleRows || l->top + row >= l->count || x < 8 || x >= 8 + 0x12C) return -1;
    l->cursor = (u8)row;
    int item = l->top + row;
    if (x >= 8 + 0x90 - 8) { /* an option: the left one or the right one */
        o34_menuConfig_setOption(x >= 8 + 0x90 + 0x5A - 8);
        return -1;
    }
    if (item == CONFIG_ROW_CONTROLLER || item == CONFIG_ROW_BTL_CONTROL) { *press = 0x4000; return 1; }
    snd_playSe(SE_CURSOR);
    return -1;
}
