/* Battle on the 3DS's two screens (docs/ui-design.md): the scene alone on top (with the target hands: render.c),
 * the commands, party and targets on the bottom. While targeting, the game's own target window (ENEMY / PLAYER
 * names with the hand cursor) is the bottom screen's target panel, touchable like every other list there: it is
 * forced on ("Target Window: Always", config bit 19) for the battle, and the player's setting is put back after. */
#define M2CTX
#include "ff9.h"

#define TARGET_WINDOW (1u << CFG_BIT_TARGET_WINDOW)

void port_battle_ui_frame(void) {
    static int forced, had;
    if (!g_gameState) return;
    int battle = g_gameState->mode == 2;
    if (battle && !forced) {
        had = (g_gameState->config & TARGET_WINDOW) != 0;
        g_gameState->config |= TARGET_WINDOW;
        forced = 1;
    } else if (!battle && forced) {
        if (!had) g_gameState->config &= ~TARGET_WINDOW;
        forced = 0;
    }
}
