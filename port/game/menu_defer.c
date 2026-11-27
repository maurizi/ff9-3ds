/* A field script's menu (a shop, a Moogle's save) opened while a message box is still going away. The scripts close
 * the box (the player's confirm, msg_closeAll as the menu opens) and open the menu 3 to 7 frames later, while the box
 * shrinks; the 3DS freezes the scene on the top screen as the menu opens, so it held the box half shrunk, the
 * shopkeeper's words cut mid-word, and the box only went once the menu closed.
 *
 * What the top should hold depends on what comes after the menu (the user's rule):
 *   - frames without any box follow (the shopkeepers: the script waits, then gives control back): the scene runs on
 *     until the box is gone, then the menu opens over it, the box not in it;
 *   - another box opens before the closing one is gone (no frame without a box): the menu opens at once and the top
 *     holds the box whole, from before it began to shrink (render.c holds its snapshot while a window animates).
 * Which: the frames the closing boxes still take (their sizes over their steps, as win_stepClose shrinks them) against
 * the frames the script waits after the menu before its next message (its WAITs, through plain jumps). A script that
 * does anything else first counts as the first case.
 *
 * menu_open is wrapped (config/renames.txt). Deferring is the opcode's own retry: the instruction pointer back to the
 * MENU instruction, run again next frame (as a wait-for-turn does), at most DEFER_MAX frames. Field scripts only (the
 * world map's menu opcode also calls the script's menu function each time). */
#define M2CTX
#include "ff9.h"

void menu_open_game(s32 type, s32 arg);

#define DEFER_MAX 60

/* the frames a window still takes to close: win_stepClose shrinks its size by its steps each frame and it is closed
 * once its width or its height is 0 */
static int close_frames(const Window *w) {
    int sw = w->animMode == WIN_ANIM_TAIL ? w->stepLeft + w->stepRight : w->stepW, sh = w->stepH;
    int fw = sw > 0 ? (w->cur.w + sw - 1) / sw : 1 << 20, fh = sh > 0 ? (w->cur.h + sh - 1) / sh : 1 << 20;
    int f = fw < fh ? fw : fh;
    return f == 1 << 20 ? 0 : f;
}

/* the frames until no box is left (the windows closing, or opening: msg_closeAll has closed those) */
static int boxes_left(void) {
    int most = 0;
    for (int i = 0; i < 10; i++) {
        const Window *w = &g_windows[i];
        if (w->state == WIN_STATE_CLOSED || !w->visible) continue;
        int f = w->state == WIN_STATE_CLOSING ? close_frames(w) : 1;
        if (f > most) most = f;
    }
    return most;
}

/* whether the script opens a message box within `frames` frames of the menu's close: its next instructions, WAITs
 * (literal frames) and plain jumps followed, up to a message opcode */
static int box_follows(const u8 *p, int frames) {
    int waited = 0;
    for (int n = 0; n < 32 && p; n++) {
        switch (p[0]) {
        case 0x22: /* WAIT, literal */
            if (p[1] != 0) return 0;
            waited += p[2];
            p += 3;
            break;
        case 0x01: /* JUMP */
            p += 3 + (s16)(p[1] | p[2] << 8);
            break;
        case 0x1F: case 0x20: case 0x95: case 0x96: /* MSG_WAIT, MSG, MSG_OBJ_WAIT, MSG_OBJ */
            return waited < frames;
        default:
            return 0;
        }
    }
    return 0;
}

/* (frames since a window last animated, kept by the 3DS's presents: the picture lags the windows' state by a frame or
 * two, so the menu waits SETTLE of them, longer than render.c's HOLD_LAG, for the snapshot to be taken from a frame
 * drawn without the box. The PC keeps no time: it waits for the windows alone) */
int port_windows_quiet(void);
#define SETTLE 6

void menu_open(s32 type, s32 arg) {
    static int deferred;
    EvtObject *o = g_evtCurObj;
    u8 *at = g_evtInstrStart;
    /* (the field script's MENU instruction running: not the field's own Triangle menu, a disc change, the title) */
    if (type != MENU_MAIN && g_evtMode == EVT_MODE_FIELD && o && at && *at == EVT_OP_MENU && o->ip > at &&
        o->ip - at <= 16 && deferred < DEFER_MAX) {
        msg_closeAll(); /* (as the opcode does right after: the boxes still open start to close now) */
        int left = boxes_left(), quiet = port_windows_quiet();
        /* (another box before this one is gone: no deferring, the top holds this one whole) */
        int busy = left > 0 || quiet < SETTLE, wait = busy && !(deferred == 0 && left > 0 && box_follows(o->ip, left));
        if (wait) {
            deferred++;
            o->ip = at; /* (the MENU instruction again next frame) */
            g_evtArgRead = 1;
            return;
        }
    }
    deferred = 0;
    menu_open_game(type, arg);
}
