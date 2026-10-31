/* The disc-change screen ("Change to DISC n"): all four discs are on the card, so the one it asks for goes in the
 * drive as it opens. The screen is the PS1's: its picture, and the check (cross) that then finds the right disc. */
#define M2CTX
#include "ovl_3e.h"

void o3e_discChange_init_game(void);
void psx_cd_swap(unsigned n);
void port_log(const char *fmt, ...);

void o3e_discChange_init(void) {
    o3e_discChange_init_game();
    port_log("disc change: disc %d asked for\n", s_discChange.disc + 1);
    psx_cd_swap(s_discChange.disc + 1u); /* (disc: 0-3, as g_gameState->disc's bits) */
}
