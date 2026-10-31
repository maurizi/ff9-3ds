/* The Status screen by touch, directly: a tap near its left edge shows the previous member, near the right edge the
 * next (the game's menuStatus_setMember with its slide, as L1 / R1), elsewhere opens the abilities page (Cross). */
#define M2CTX
#include "ovl_38.h"

void o38_menuStatus_setMember(s32 slot, s32 dir);

/* a tap at (x, y) in the frame's coordinates: 1 with a press in *press, -1 taken, 0 not here */
int port_status_tap(int x, int y, unsigned *press) {
    (void)y;
    if (!g_menuScreen.active || g_menuScreen.screen != MENU_SCREEN_STATUS) return 0;
    if (s_menuStatus.state != STATUS_STATE_MAIN || fade_isBusy()) return 0; /* (the abilities page: the hand) */
    int cur = s_menuStatus.slot, next = cur;
    if (x < 48) next = party_getPrevMember(cur);
    else if (x >= 272) next = party_getNextMember(cur);
    else { *press = 0x4000; return 1; }
    if (next != cur) {
        snd_playSe(SE_PAGE);
        o38_menuStatus_setMember(next, x < 48 ? 1 : 2);
    }
    return -1;
}
