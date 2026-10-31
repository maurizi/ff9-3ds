/* Touch on the bottom screen: a tap moves the game's own cursor there and confirms.
 *
 * FF9's menus point at the selected entry with a hand (render.c records where the UI layer drew it). A tap sets a
 * goal; the driver then presses the d-pad, one press at a time, until the hand points at the tapped row (and
 * column), and presses cross. The game's own cursor logic (wrapping, skipped entries, sub-menus) is never
 * bypassed: touch is a faster way of pressing the buttons the player could press. A tap with no hand on screen
 * does nothing. AUTOTOUCH="X,Y@FRAME ..." scripts taps for tests. */
#include <3ds.h>
#include <stdlib.h>

#include "ctr.h"
#include "psx_runtime.h"

enum { PAD_UP = 0x0010, PAD_RIGHT = 0x0020, PAD_DOWN = 0x0040, PAD_LEFT = 0x0080, PAD_CROSS = 0x4000 };
#define HAND_W 23
#define HAND_H 13
#define PRESS 2   /* frames a press is held */
#define SETTLE 5  /* frames released after it, so the game moves the cursor and redraws the hand */
#define MAX_STEPS 32

static int goal, gx, gy, steps, timer;
static unsigned held;

static void tap(int x, int y) {
    port_log("touch: tap %d,%d (UI %s, %d hands)\n", x, y, ctr_ui_active() ? "on" : "off", ctr_ui_nhands);
    if (!ctr_ui_active()) return;
    goal = 1;
    gx = x;
    gy = y;
    steps = 0;
    timer = 0;
}

#ifdef PORT_AUTOTOUCH
static struct { int x, y; long frame; } script[32];
static int nscript, script_parsed;
static void parse_script(void) {
    const char *p = PORT_AUTOTOUCH;
    script_parsed = 1;
    while (*p && nscript < 32) {
        char *e;
        int x = (int)strtol(p, &e, 10);
        if (*e != ',') break;
        int y = (int)strtol(e + 1, &e, 10);
        if (*e != '@') break;
        long f = strtol(e + 1, &e, 10);
        script[nscript].x = x; script[nscript].y = y; script[nscript].frame = f;
        nscript++;
        while (*e == ' ' || *e == ',') e++;
        p = e;
    }
}
#endif

/* the next button to press toward the goal, 0 when there is nothing to do */
static unsigned step(void) {
    if (!ctr_ui_nhands) { goal = 0; return 0; }
    /* the hand nearest the goal (the battle shows one; lists with a second, inactive hand are rare) */
    int best = 0, bestd = 1 << 30;
    for (int i = 0; i < ctr_ui_nhands; i++) {
        int dx = gx - (ctr_ui_hands[i][0] + HAND_W), dy = gy - (ctr_ui_hands[i][1] + HAND_H / 2);
        int d = abs(dx) + abs(dy);
        if (d < bestd) bestd = d, best = i;
    }
    int hx = ctr_ui_hands[best][0], hy = ctr_ui_hands[best][1] + HAND_H / 2;
    if (++steps > MAX_STEPS) { goal = 0; return 0; }
    if (gy > hy + 7) return PAD_DOWN;
    if (gy < hy - 7) return PAD_UP;
    if (gx < hx - 4) return PAD_LEFT;              /* left of the hand: the column before */
    if (gx > hx + HAND_W + 112) return PAD_RIGHT;  /* well right of its entry: the next column */
    goal = 0;
    return PAD_CROSS;
}

unsigned int ctr_ui_pad_buttons(long frame) {
#ifdef PORT_AUTOTOUCH
    if (!script_parsed) parse_script();
    for (int i = 0; i < nscript; i++)
        if (script[i].frame == frame) tap(script[i].x, script[i].y);
#else
    (void)frame;
#endif
    if (hidKeysDown() & KEY_TOUCH) {
        touchPosition t;
        hidTouchRead(&t);
        tap(t.px, t.py);
    }
    if (timer > 0) {
        timer--;
        return timer >= SETTLE ? held : 0;
    }
    if (!goal) return 0;
    held = step();
    if (ctr_ui_nhands) port_log("touch: hand %d,%d -> %04x\n", ctr_ui_hands[0][0], ctr_ui_hands[0][1], held);
    if (!held) return 0;
    timer = PRESS + SETTLE - 1;
    return held;
}
