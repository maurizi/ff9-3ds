/* Which packets are UI: the window system's (windows, their text, list cursors, titles), whatever mode draws them.
 * win_drawAll is wrapped (config/renames.txt): the packets it adds to the frame's buffer lie between the buffer's
 * free pointer before and after it, and the renderer is told that range (port/3ds/render.c draws such packets on
 * the bottom screen; the PC ignores it). */
#define M2CTX
#include "ff9.h"
#include "psx_mem.h"

void win_drawAll_game(void);
__attribute__((weak)) void port_ui_packets(const void *lo, const void *hi) { (void)lo; (void)hi; }

void win_drawAll(void) {
    u8 *lo = g_primPtr;
    win_drawAll_game();
    if (g_primPtr != lo) port_ui_packets((const void *)PSX_PTR(lo), (const void *)PSX_PTR(g_primPtr));
}

/* Whether a menu is up (menu_doOpen .. menu_finish): 0 no, 1 opening (the scene fades out), 2 showing (the scene is
 * hidden and the frame is all menu), 3 the card game (mode 9: all of it UI, on both screens). The 3DS shows a menu on its bottom screen and freezes the scene on the top. */
int port_menu_state(void) {
    GameData *g = g_gameState ? g_gameState->gameData : NULL;
    if (!g) return 0;
    if (g_gameState->mode == 9) return 3; /* (the card game: all UI, on both screens) */
    /* (the battle's end (ovl_65: EXP / AP, gil, items, level-ups) is its own screen, not the menu system's: a menu like
     * the others, to the bottom screen, the battle's last frame dimmed on top) */
    if (g_gameState->mode == 8) return 2;
    /* (the field's battle transition sets these flags too, with the menu system idle: not a menu) */
    if (!(g->flags & GD_MENU_OPEN) || g_menu.state == 0) return 0;
    return (g->flags & GD_HIDE_ALL) == GD_HIDE_ALL ? 2 : 1; /* (events hide parts of the scene by these flags too) */
}

/* the game's mode (g_gameState->mode: 1 field, 2 battle, 3 world map, ...) */
int port_game_mode(void) {
    return g_gameState ? g_gameState->mode : 0;
}

/* the current field's id (the 3DS's heartbeat), -1 outside one */
int port_field_id(void) {
    return g_gameState && g_gameState->gameData && g_gameState->mode == 1 ? g_gameState->gameData->fieldId : -1;
}
