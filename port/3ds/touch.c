/* Touch on the bottom screen: a tap moves the game's own cursor there and confirms.
 *
 * FF9's menus point at the selected entry with a hand (render.c records where the UI layer drew it). A tap sets a
 * goal; the driver then presses the d-pad, one press at a time, until the hand points at the tapped row (and
 * column), and presses cross. The game's own cursor logic (wrapping, skipped entries, sub-menus) is never
 * bypassed: touch is a faster way of pressing the buttons the player could press. A tap with no hand on screen
 * does nothing. A swipe instead of a tap: left is back (circle), up or down steps the cursor once per 24 pixels
 * (scrolling a list). AUTOTOUCH="X,Y@FRAME ..." scripts taps for tests (X,Y@FRAME+N holds, X,Y>X2,Y2@FRAME+N
 * drags). */
#include <3ds.h>
#include <stdlib.h>

#include "ctr.h"
#include "psx_runtime.h"

enum { PAD_UP = 0x0010, PAD_RIGHT = 0x0020, PAD_DOWN = 0x0040, PAD_LEFT = 0x0080, PAD_CIRCLE = 0x2000,
       PAD_CROSS = 0x4000 };
#define HAND_W 23
#define HAND_H 13
#define ROW_AT 2  /* the middle of the entry the hand points at, from the hand's top (its text is 4 pixels higher) */
#define PRESS 2   /* frames a press is held */
#define SETTLE 5  /* frames released after it, so the game moves the cursor and redraws the hand */
#define MAX_STEPS 32

/* ---- the field panel ---- */
const CtrPanelButton ctr_panel_buttons[] = {
    {230, 18, 60, 60, 0x1000, 68, 0}, /* triangle: the menu */
    {180, 76, 60, 60, 0x8000, 56, 0}, /* square */
    {268, 90, 52, 52, 0x2000, 44, 0}, /* circle: hold to walk */
    {218, 140, 76, 76, 0x4000, 32, 0}, /* cross: talk, check */
};
const int ctr_panel_nbuttons = sizeof ctr_panel_buttons / sizeof ctr_panel_buttons[0];
unsigned ctr_panel_held;
int ctr_panel_pad_dir, ctr_panel_pad_x, ctr_panel_pad_y;

static int panel_button_at(int x, int y) {
    for (int i = 0; i < ctr_panel_nbuttons; i++) {
        const CtrPanelButton *b = &ctr_panel_buttons[i];
        if (x >= b->x - 6 && x < b->x + b->w + 6 && y >= b->y - 6 && y < b->y + b->h + 6) return i;
    }
    return -1;
}

/* the panel's buttons for this frame: the stylus on a button holds it; dragging on the pad's half is the d-pad */
static unsigned panel(int touching, int x, int y, int went_down) {
    static int on_pad;
    ctr_panel_held = 0;
    ctr_panel_pad_dir = 0;
    if (!touching) { on_pad = 0; return 0; }
    if (went_down) {
        on_pad = x < CTR_PANEL_PAD_W;
        ctr_panel_pad_x = x;
        ctr_panel_pad_y = y;
    }
    if (on_pad) {
        int dx = x - ctr_panel_pad_x, dy = y - ctr_panel_pad_y, d = 0;
        if (dx * dx + dy * dy > 12 * 12) { /* (8 directions: 22.5-degree sectors, by slopes) */
            if (abs(dx) * 5 > abs(dy) * 2) d |= dx > 0 ? PAD_RIGHT : PAD_LEFT;
            if (abs(dy) * 5 > abs(dx) * 2) d |= dy > 0 ? PAD_DOWN : PAD_UP;
        }
        ctr_panel_pad_dir = d;
        return (unsigned)d;
    }
    int i = panel_button_at(x, y);
    if (i < 0) return 0;
    ctr_panel_held = 1u << i;
    return ctr_panel_buttons[i].pad;
}

static int goal, gx, gy, steps, timer;
static unsigned held;
/* the stylus while it is down: where it went down, and how far a vertical swipe has stepped */
static int down, dx0, dy0, swiped, vsteps, last_x, last_y;
static unsigned queued, nqueued; /* presses a swipe asked for, done one at a time */

extern int ctr_ui_oy; /* render.c: the UI layer's row 0 on the bottom screen */
static void tap(int x, int y) {
    y -= ctr_ui_oy;
    port_log("touch: tap %d,%d (UI %s, %d hands)\n", x, y, ctr_ui_active() ? "on" : "off", ctr_ui_nhands);
    if (!ctr_ui_active()) return;
    goal = 1;
    gx = x;
    gy = y;
    steps = 0;
    timer = 0;
}

#ifdef PORT_AUTOTOUCH
static struct { int x, y, x2, y2; long frame, hold; } script[32];
static int nscript, script_parsed;
static void parse_script(void) {
    const char *p = PORT_AUTOTOUCH;
    script_parsed = 1;
    while (*p && nscript < 32) {
        char *e;
        int x = (int)strtol(p, &e, 10);
        if (*e != ',') break;
        int y = (int)strtol(e + 1, &e, 10), x2 = x, y2 = y;
        if (*e == '>') { /* X,Y>X2,Y2: a drag, X,Y to X2,Y2 over the hold */
            x2 = (int)strtol(e + 1, &e, 10);
            if (*e != ',') break;
            y2 = (int)strtol(e + 1, &e, 10);
        }
        if (*e != '@') break;
        long f = strtol(e + 1, &e, 10), hold = 0;
        if (*e == '+') hold = strtol(e + 1, &e, 10);
        script[nscript].x = x; script[nscript].y = y; script[nscript].frame = f; script[nscript].hold = hold;
        script[nscript].x2 = x2; script[nscript].y2 = y2;
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
        int dx = gx - (ctr_ui_hands[i][0] + HAND_W), dy = gy - (ctr_ui_hands[i][1] + ROW_AT);
        int d = abs(dx) + abs(dy);
        if (d < bestd) bestd = d, best = i;
    }
    int hx = ctr_ui_hands[best][0], hy = ctr_ui_hands[best][1] + ROW_AT;
    if (++steps > MAX_STEPS) { goal = 0; return 0; }
    extern int port_game_mode(void);
    if (port_game_mode() == 9) { /* the card game: small cells in a grid, the hand's tip on the one it points at */
        int tx = hx + HAND_W + 6;
        if (gy > hy + 7) return PAD_DOWN;
        if (gy < hy - 7) return PAD_UP;
        if (gx > tx + 9) return PAD_RIGHT;
        if (gx < tx - 9) return PAD_LEFT;
        goal = 0;
        return PAD_CROSS;
    }
    if (gy > hy + 7) return PAD_DOWN;
    if (gy < hy - 7) return PAD_UP;
    if (gx < hx - 4) return PAD_LEFT;              /* left of the hand: the column before */
    if (gx > hx + HAND_W + 112) return PAD_RIGHT;  /* well right of its entry: the next column */
    goal = 0;
    return PAD_CROSS;
}

unsigned int ctr_ui_pad_buttons(long frame) {
    u32 kd = hidKeysDown(), kh = hidKeysHeld();
    touchPosition t = {0, 0};
    if (kh & KEY_TOUCH) hidTouchRead(&t);
#ifdef PORT_AUTOTOUCH
    /* scripted: X,Y@FRAME taps (the cursor driver's tap), X,Y@FRAME+N holds the stylus there for N frames */
    if (!script_parsed) parse_script();
    for (int i = 0; i < nscript; i++) {
        if (!script[i].hold && script[i].frame == frame) tap(script[i].x, script[i].y);
        if (script[i].hold && frame >= script[i].frame && frame < script[i].frame + script[i].hold) {
            if (frame == script[i].frame) kd |= KEY_TOUCH;
            kh |= KEY_TOUCH;
            long k = frame - script[i].frame, n = script[i].hold > 1 ? script[i].hold - 1 : 1;
            if (k > n / 4) k = n; /* (a drag reaches its end within its first quarter, then holds there) */
            t.px = (u16)(script[i].x + (script[i].x2 - script[i].x) * k / n);
            t.py = (u16)(script[i].y + (script[i].y2 - script[i].y) * k / n);
        }
    }
#else
    (void)frame;
#endif
    if (ctr_panel_shown() && !ctr_ui_active()) { /* the field panel: no cursor to drive */
        down = 0;
        return panel(!!(kh & KEY_TOUCH), t.px, t.py, !!(kd & KEY_TOUCH));
    }
    ctr_panel_held = 0;
    ctr_panel_pad_dir = 0;
    if (kh & KEY_TOUCH) {
        if (kd & KEY_TOUCH) down = 1, dx0 = t.px, dy0 = t.py, swiped = 0, vsteps = 0;
        last_x = t.px;
        last_y = t.py;
        int mx = t.px - dx0, my = t.py - dy0;
        if (!swiped && mx < -48 && abs(my) < 32) { /* swipe left: back */
            swiped = 1;
            queued = PAD_CIRCLE, nqueued = 1;
        } else if (!swiped || vsteps) {
            int want = my / 24; /* swipe up/down: a step per 24 pixels */
            if (abs(mx) < 32 && want != vsteps) {
                swiped = 1;
                queued = want > vsteps ? PAD_DOWN : PAD_UP;
                nqueued += (unsigned)abs(want - vsteps);
                vsteps = want;
            }
        }
    } else if (down) {
        down = 0;
        if (!swiped) tap(last_x, last_y); /* a tap: on release, where it was */
    }
    if (nqueued && timer == 0) {
        nqueued--;
        held = queued;
        timer = PRESS + SETTLE - 1;
        goal = 0;
        return held;
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
