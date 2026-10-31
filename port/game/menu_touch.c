/* Touch on the main menu (drawn 1:1 on the bottom screen), directly: a tap on a command row picks that command, on a
 * party member (while a command asks for one) opens that member's screen, in Order picks and swaps. The tap waits for
 * the menu's next input step (its per-state handlers, which menuMain_input reaches through the s_menuMainInput table:
 * wrapped for both copies, the field's ovl_0a and the world map's ovl_0c, config/renames.txt), which then sets the
 * game's cursor and runs the game's own selection
 * (menuMain_selectCommand, menuScreen_open, menuMain_reorder), with its sounds. Layout (the PS1's, in the frame): the
 * commands 15 px apart from y 14 at x 236-310, the four member rows 47 px apart from y 14 at x 12-262. */
#define M2CTX
#include "ovl_0a.h"

enum { TAP_NONE, TAP_CMD, TAP_MEMBER };
static int tap_kind, tap_arg;

/* a tap at (x, y) in the frame's coordinates: -1 handled, 0 not the main menu */
int port_menu_tap(int x, int y) {
    if (!g_gameState || g_menu.type != MENU_MAIN || !g_menuScreen.active || g_menuScreen.screen != 0) return 0;
    tap_kind = TAP_NONE;
    if (x >= 236 && x < 312 && y >= 14 && y < 14 + 7 * 15) tap_kind = TAP_CMD, tap_arg = (y - 14) / 15;
    else if (x >= 12 && x < 262 && y >= 14 && y < 14 + 4 * 47) tap_kind = TAP_MEMBER, tap_arg = (y - 14) / 47;
    return -1;
}

static int apply(MenuMain *m, void (*select)(void), void (*reorder)(s32, s32)) {
    int k = tap_kind;
    if (k == TAP_NONE || g_menuScreen.slideIn || fade_isBusy()) return 0;
    tap_kind = TAP_NONE;
    if (k == TAP_CMD) {
        m->cursor = (u8)tap_arg;
        m->state = MENUMAIN_COMMAND;
        select();
        return 1;
    }
    if (!g_gameState->gameData->party[tap_arg]) return 1;
    if (m->state == MENUMAIN_MEMBER) {
        snd_playSe(SE_CURSOR);
        g_menuScreen.member = (u8)tap_arg;
        menuScreen_open(m->cursor + 1);
    } else if (m->state == MENUMAIN_ORDER) {
        snd_playSe(SE_CURSOR);
        if (!m->orderPicked) {
            m->orderCursor = (u8)tap_arg;
            m->orderPicked = 1;
            m->orderTarget = m->orderCursor;
        } else {
            reorder(m->orderCursor, tap_arg);
            m->orderPicked = 0;
            m->orderCursor = (u8)tap_arg;
        }
    }
    return 1;
}

extern MenuMain o0a_s_menuMain, o0c_s_menuMain;
void o0a_menuMain_selectCommand(void), o0c_menuMain_selectCommand(void);
void o0a_menuMain_reorder(s32, s32), o0c_menuMain_reorder(s32, s32);
#define WRAP(ovl, state)                                                                                               \
    void ovl##_menuMain_input##state##_game(void);                                                                     \
    void ovl##_menuMain_input##state(void) {                                                                           \
        if (!apply(&ovl##_s_menuMain, ovl##_menuMain_selectCommand, ovl##_menuMain_reorder))                          \
            ovl##_menuMain_input##state##_game();                                                                      \
    }
WRAP(o0a, Command)
WRAP(o0a, Member)
WRAP(o0a, Order)
WRAP(o0c, Command)
WRAP(o0c, Member)
WRAP(o0c, Order)
