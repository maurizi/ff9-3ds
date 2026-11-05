/* The title screen's choices on the bottom screen (docs/ui-design.md): the top keeps the title picture and logo only
 * (the PS1's NEW GAME / CONTINUE and its hand are not drawn there), the bottom has NEW GAME, CONTINUE and OPTIONS as
 * large buttons (port/game/field_hud.c draws them, the one the D-pad is on marked). A tap on NEW GAME or CONTINUE
 * puts the game's cursor there and runs its own decide (CONTINUE with no save: its buzzer, as on the PS1); OPTIONS
 * shows the port's settings in their place. opening_titleUpdate is wrapped (config/renames.txt): its own steps, with
 * the background in place of the whole draw. */
#define M2CTX
#include "ovl_14.h"

void o14_opening_titleInput(void);
void o14_opening_titleOnDraw(void);
void o14_opening_titleDrawBackground(void);
void o14_opening_titleTick(void);
void o14_opening_titleDecide(void);
void o14_opening_titleConfirm(void);
u8 o14_opening_titleHasCard(void);
long VSync(long);

static long live_frame = -100;
static int tap = -1; /* the choice a tap asked for, applied at the title's next input step */

void o14_opening_titleUpdate(void) {
    pad_updateRepeat(0);
    if (g_titleMenu.decided) {
        o14_opening_titleConfirm();
        return;
    }
    if (tap >= 0 && !fade_isBusy() && g_titleMenu.shown && !g_titleMenu.fadingIn) {
        g_titleMenu.cursor = (u8)tap;
        o14_opening_titleDecide();
    }
    tap = -1;
    o14_opening_titleInput();
    o14_opening_titleOnDraw();
    o14_opening_titleDrawBackground();
    o14_opening_titleTick();
    live_frame = VSync(-1);
}

/* for field_hud.c: whether the title's choices are up (drawn this frame or the last), which one the cursor is on, and
 * whether there is a save to continue */
int port_title_live(void) { return VSync(-1) - live_frame <= 2 && g_titleMenu.shown && !g_titleMenu.decided; }
int port_title_cursor(void) { return g_titleMenu.cursor; }
int port_title_has_save(void) { return o14_opening_titleHasCard() != 0; }
void port_title_choose(int choice) { tap = choice; }
/* whether Start does something now: the opening movies (once playing, until they skip or end) and the attract images
 * take it; the boot screens and logos before and the pauses between them don't (field_hud.c shows its PRESS START
 * card only then) */
extern u16 o14_g_movieFlags; /* (the opening overlay's: the native g_movieFlags is another, unused here) */
int port_opening_takes_start(void) {
    switch (g_openingState) {
    case OPENING_MOVIE:
    case OPENING_MOVIE2:
        return (o14_g_movieFlags & (MOVIE_PLAYING | MOVIE_SKIPPING | MOVIE_ENDED)) == MOVIE_PLAYING; /* (the movie's own skip
                                                                                                * check: before, the logos) */
    case OPENING_ATTRACT1:
    case OPENING_ATTRACT2:
        return 1;
    default:
        return 0;
    }
}
