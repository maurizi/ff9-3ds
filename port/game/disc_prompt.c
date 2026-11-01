/* The end of a disc's "DISC CHANGE / Do you want to save your game?" (the field's own disc-change loop, ovl_0a
 * fld_disc.c): the question stays on the top screen with the scene; its two answers are large buttons on the bottom
 * (field_hud.c, as a dialogue's choices), and a tap puts the prompt's cursor there and confirms with the game's own
 * Cross. fld_runDiscChange is wrapped (config/renames.txt) to know when the loop runs. */
#define M2CTX
#include "ovl_0a.h"

void o0a_fld_runDiscChange_game(void);
static int running;

void o0a_fld_runDiscChange(void) {
    running = 1;
    o0a_fld_runDiscChange_game();
    running = 0;
}

int port_disc_prompt_live(void) { return running && g_fieldDiscChange.mode == FLD_DISC_PROMPT && !fade_isBusy(); }
int port_disc_prompt_cursor(void) { return g_fieldDiscChange.cursor; }
u8 *port_disc_prompt_text(int i) { return text_get(i); } /* (0 the question, 1 and 2 the answers: the prompt's bank) */
void port_disc_prompt_set(int c) { g_fieldDiscChange.cursor = (u8)c; }
