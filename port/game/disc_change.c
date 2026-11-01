/* The disc-change screen ("Change to DISC n"): every disc is in the one ROM, so the one it asks for goes in the drive
 * as it opens, and the screen goes straight to the game's own check (which finds the right disc) and closes, as when
 * the player puts the disc in and presses Cross: no prompt, no press, only the check's fade. */
#define M2CTX
#include "ovl_3e.h"

void o3e_discChange_init_game(void);
void psx_cd_swap(unsigned n);
void port_log(const char *fmt, ...);

void o3e_discChange_init(void) {
    o3e_discChange_init_game();
    port_log("disc change: disc %d asked for\n", s_discChange.disc + 1);
    psx_cd_swap(s_discChange.disc + 1u); /* (disc: 0-3, as g_gameState->disc's bits) */
    void port_scenario_disc_changed(void);
    port_scenario_disc_changed();
}

void o3e_discChange_update_game(void);
void o3e_discChange_startCheck(void);
void o3e_discChange_update(void) {
    if ((s_discChange.state == DISC_ST_LID_CLOSED || s_discChange.state == DISC_ST_LID_OPEN) && !fade_isBusy()) {
        o3e_discChange_startCheck(); /* (its fade, then the check: DISC_CHECK_OK closes; else its error screen) */
        return;
    }
    o3e_discChange_update_game();
}
