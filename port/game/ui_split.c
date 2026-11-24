/* Which packets are UI: the window system's (windows, their text, list cursors, titles), whatever mode draws them.
 * win_drawAll is wrapped (config/renames.txt): the packets it adds to the frame's buffer lie between the buffer's
 * free pointer before and after it, and the renderer is told that range (port/3ds/render.c draws such packets on
 * the bottom screen; the PC ignores it). */
#define M2CTX
#include "ff9.h"
#include "psx_mem.h"

void win_drawAll_game(void);
__attribute__((weak)) void port_ui_packets(const void *lo, const void *hi) { (void)lo; (void)hi; }

/* A field minigame's own panels (its how-to-play box, its score) belong on the bottom screen with the touch controls,
 * not over the scene; its speech balloons stay with the speakers. 1: window w goes to the bottom screen; 2 and 3: not
 * drawn (2: a panel the port draws itself, 3: field dialogue under a menu). */
void port_bottom_packets(const void *lo, const void *hi);
__attribute__((weak)) void port_bottom_shift(const void *lo, const void *hi, int dx, int dy) { (void)lo, (void)hi, (void)dx, (void)dy; }
/* (the minigames' panels by field and message id, decomp R38: ff9-decomp docs/r32/3ds-requests-8.md) */
static const struct { u16 field, lo, hi; } panels[] = {
    {103, 0xEA, 0xEB},   /* jump rope: "N times" (the girls' comments are speech: on the top) */
    {1850, 0x94, 0x94},  /* Hippaul racing: "Press Square and Circle alternately!" (Hippolady's lines and GO! are speech) */
    {657, 0x183, 0x183}, /* frog catching: "Frogs Captured: N" */
};
int port_menu_state(void);
/* The ending's field cutscenes: after Necron (the party waking on the hill (field 2928) to the Iifa tree's roots (2934))
 * and the epilogue's vignettes (3000-3012: Alexandria, Burmecia, Lindblum, Treno, the theater ship). The player has no
 * control in them: the bottom screen shows no field screen there (field_hud.c), and their frameless text (narration
 * over a dimmed scene, the vignettes' subtitles) is moved to it */
/* (the field's id; -1 outside one, and before the game state exists: the renderer asks from its first present, with
 * g_gameState still null (a crash at start on a 3DS, where address 0 isn't mapped; Azahar maps it)) */
static int ending_field_id(void) {
    return g_gameState && g_gameState->mode == 1 && g_gameState->gameData ? g_gameState->gameData->fieldId : -1;
}
int port_ending_field(void) {
    int f = ending_field_id();
    return (f >= 2928 && f <= 2934) || (f >= 3000 && f <= 3012);
}
/* The vignettes whose scene is the top lines of the frame, a black bar under it for their subtitles: the top screen shows
 * the scene alone, in its middle, the subtitles being on the bottom screen (render.c). The bar is no primitive nor clip: the
 * backgrounds end there, at line 184 (181 in Lindblum's, 3006), measured in each. (3001 and the play on the theater
 * ship, 3009-3011, are full-height) */
int port_top_crop(void) {
    int f = ending_field_id();
    return f == 3006 ? 181 : f == 3000 || (f >= 3002 && f <= 3008) || f == 3012 ? 184 : 0;
}
static int to_bottom(const Window *w) {
    if (g_gameState->mode != 1) return 0;
    int field = g_gameState->gameData->fieldId;
    if (port_ending_field() && w->style == WIN_STYLE_FRAMELESS) return 4; /* (centred on the bottom screen: win_drawAll) */
    /* (a menu over the field: the field's dialogue (its speech balloons, its message windows) stays with the frozen scene
     * on the top screen; while the menu opened, the scene hidden and the menu not yet drawn, a bubble showed on the
     * bottom screen alone for some 20 frames ("You wanna try?", Alexandria's jump rope)) */
    if (port_menu_state() == 1 || port_menu_state() == 2) {
        if (w->style == WIN_STYLE_BALLOON) return 3;
        for (int k = 0; k < 10; k++)
            if (g_msgWindows[k].open && g_msgWindows[k].window == w) return 3;
    }
    if (field == 909) { /* (the Treno auction: its bid windows replaced by auction_ui.c's panel: off the top screen) */
        for (int k = 0; k < 10; k++)
            if (g_msgWindows[k].open && g_msgWindows[k].window == w && g_msgWindows[k].msgId >= 0xC9 && g_msgWindows[k].msgId <= 0xCE)
                return 2;
        return 0;
    }
    for (int k = 0; k < 10; k++) {
        const MsgWindow *m = &g_msgWindows[k];
        if (!m->open || m->window != w) continue;
        for (unsigned i = 0; i < sizeof panels / sizeof panels[0]; i++)
            if (panels[i].field == field && m->msgId >= panels[i].lo && m->msgId <= panels[i].hi) return 1;
    }
    return 0;
}

/* a minigame's panel was drawn to the bottom screen on the last frames: the field's bottom screen gives way to it */
static long panel_frame = -100;
/* The ending's narration over a scene (Kuja's words to Zidane, field 3001): on the PS1 the scene is darkened (a partial
 * fade, to 200 of 255) and the narration shown over it. Its text being on the bottom screen, the top keeps the scene as
 * it was until the text has gone: the fade is left out of the top's frame (game_updateFade, battle_ui.c) from its start.
 * Once the text has gone: if the game lightens the scene again the top stays as it is; if it goes on to black, or holds
 * the dimming WAIT_FRAMES, the top darkens to where the fade is over RAMP_FRAMES, the fade then drawn again as it is.
 * (A partial fade only: a fade to black is a change of scene, and the next field would show under it. If no narration
 * comes within PEND_FRAMES of the fade's end, the top darkens all the same) */
#define RAMP_FRAMES 20
#define PEND_FRAMES 180
#define WAIT_FRAMES 40
static long narr_frame = -100, hold_tick = -1;
static int hold, ramp, pend, hold_field, hold_from_black; /* hold: 3 the fade begun, waiting for the text; 1 the text up; 4 the text gone,
                                          * waiting on the fade; 5 the fade lightening; 2 darkening */
int port_ending_field(void);
static int fade_level(void);
static int fade_target(void) {
    int c = g_fade.endR > g_fade.endG ? g_fade.endR : g_fade.endG;
    return g_fade.endB > c ? g_fade.endB : c;
}
static void hold_update(void) {
    long now = VSync(-1);
    if (now == hold_tick) return;
    hold_tick = now;
    if (hold && (g_gameState->mode != 1 || g_gameState->gameData->fieldId != hold_field)) hold = 0;
    int t = fade_target();
    /* (a strong darkening: the narrations' (128 grey on the hill, 90, 140, 200 in field 3001). A light one is a tint (the
     * Hilda Garde's sunset, field 2931: 0, 32, 64), drawn as it is) */
    /* (begun from black, as field 3001 is after the Iifa tree (black to its dimming, Mikoto and Vivi's words over it): the
     * top fades in to the scene itself over the fade's frames, not to the dimming (port_top_fade): a scene in motion,
     * the user's choice; field 3000's "Some time later..." opens from black to a light dimming (32), not held) */
    if (!hold && port_ending_field() && g_fade.active && g_fade.subtract && g_fade.covered && t >= 100 && t < 250 &&
        g_fade.frame * 4 < g_fade.frames)
        hold = 3, pend = 0, hold_field = g_gameState->gameData->fieldId, hold_from_black = fade_level() >= 64;
    int lightening = !g_fade.covered || !t;
    if (hold == 3) { /* (the dimming begun: the text may come) */
        if (lightening) hold = 5;
        else if (!g_fade.active && ++pend > PEND_FRAMES) hold = 2, ramp = 0;
    } else if (hold == 1 && now - narr_frame > 2) { /* (the text gone: what the game does next decides) */
        hold = 4, pend = 0;
    } else if (hold == 4) {
        /* lightened again (Necron's lines on the hill, field 2928: dimmed for them and back): the scene stays as it is,
         * no dip to the dimming and back; on to black (a change of scene), or held dim a while (field 3001), it darkens */
        if (lightening) hold = 5;
        else if ((g_fade.active && t >= 250) || ++pend > WAIT_FRAMES) hold = 2, ramp = 0;
    } else if (hold == 5) { /* (lightening: its fade left out until it is done, then nothing is left of it to draw) */
        if (!lightening) hold = 3, pend = 0;
        else if (!g_fade.active && !fade_level()) hold = 0;
    } else if (hold == 2 && ++ramp >= RAMP_FRAMES) hold = 0;
}
static void narration_shown(void) {
    hold_update();
    if (hold) hold = 1;
    narr_frame = VSync(-1);
}
int port_ending_fade_held(void) {
    if (!g_gameState || !hold) return 0;
    hold_update();
    return hold != 0;
}
/* what is subtracted from the top screen (0: nothing): the darkening after an ending narration, the fade's own colour
 * growing to where the fade is (render.c), so that the fade drawn again from there changes nothing (darkened grey, a
 * tinted fade came in at once when given back: a visible pop) */
int port_top_fade(u8 rgb[3]) {
    if (!port_ending_fade_held()) return 0;
    if (hold == 3 && hold_from_black && g_fade.active && g_fade.frames > 0) { /* (from black to the scene) */
        int left = g_fade.frames - (int)g_fade.frame;
        if (left < 0) left = 0;
        for (int i = 0; i < 3; i++) rgb[i] = (u8)(g_fade.startColor.b[i] * left / g_fade.frames);
        return rgb[0] | rgb[1] | rgb[2];
    }
    if (hold != 2) return 0;
    for (int i = 0; i < 3; i++) rgb[i] = (u8)(g_fade.color.b[i] * ramp / RAMP_FRAMES);
    return rgb[0] | rgb[1] | rgb[2];
}
long VSync(long);
int port_minigame_panel_up(void) { return VSync(-1) - panel_frame <= 2; }
/* The Prima Vista play's sword fight (field 64; decomp R38 4): the bottom screen dark from Zidane's "Expect no quarter"
 * (msg 0x6A) through the instructions (0x6F), Blank's prompts (0x70-0x77) and the results (0x78-0x80), its text all on
 * the top as the PS1 has it. Its instructions and results as a panel on the bottom screen were a poor fit, and between
 * them, whenever none was up, the field's screen came back for a moment (the user's report: "just go fully black").
 * FIGHT_GAP VBlanks between two of its messages (a prompt answered, the next some 30 to 50 later) keep it dark. */
#define FIGHT_GAP 120
int port_sword_fight(void) {
    static long seen = -1000, tick = -1;
    static int live;
    const long now = VSync(-1);
    if (now == tick) return live;
    tick = now;
    if (!g_gameState || g_gameState->mode != 1 || g_gameState->gameData->fieldId != 64) return live = 0;
    for (int k = 0; k < 10; k++)
        if (g_msgWindows[k].open && g_msgWindows[k].msgId >= 0x6A && g_msgWindows[k].msgId <= 0x80) seen = now;
    return live = now - seen < FIGHT_GAP;
}

void win_drawAll(void) {
    u8 *lo = g_primPtr;
    if (g_gameState->mode == 1) hold_update();
    u8 down[10], hide[10], any = 0, mid = 0;
    for (int i = 0; i < 10; i++) { /* (the bottom screen's windows hidden from the first pass) */
        Window *w = &g_windows[i];
        int b = w->state != WIN_STATE_CLOSED && w->visible ? to_bottom(w) : 0;
        down[i] = b == 1 || b == 4;
        mid |= b == 4;
        hide[i] = b == 2 || b == 3; /* (3: hidden only, not a minigame's panel) */
        if (b) w->visible = 0, any |= b == 1 || b == 4;
        if (b == 2) panel_frame = VSync(-1);
    }
    win_drawAll_game();
    if (g_primPtr != lo) port_ui_packets((const void *)PSX_PTR(lo), (const void *)PSX_PTR(g_primPtr));
    if (!any) {
        for (int i = 0; i < 10; i++) if (hide[i]) g_windows[i].visible = 1;
        return;
    }
    panel_frame = VSync(-1);
    if (mid) narration_shown();
    u8 up[10];
    for (int i = 0; i < 10; i++) { /* (a second pass with only those, its packets to the bottom screen) */
        Window *w = &g_windows[i];
        up[i] = w->visible;
        w->visible = down[i];
    }
    /* on the bottom screen: a panel, not a balloon (no speech tail, no shadow); the packets moved as a block so the
     * first one sits centred along the top (port_bottom_shift: the game's own layout and text kept) */
    u8 saved_style[10], saved_shadow[10];
    int bx0 = 1 << 30, bx1 = -(1 << 30), by0 = 1 << 30, by1 = -(1 << 30);
    for (int i = 0; i < 10; i++) {
        if (!down[i]) continue;
        Window *w = &g_windows[i];
        saved_style[i] = w->style, saved_shadow[i] = w->shadow;
        if (w->style == WIN_STYLE_BALLOON) w->style = WIN_STYLE_BEVEL;
        w->shadow = 0;
        if (w->cur.x < bx0) bx0 = w->cur.x;
        if (w->cur.x + w->cur.w > bx1) bx1 = w->cur.x + w->cur.w;
        if (w->cur.y < by0) by0 = w->cur.y;
        if (w->cur.y + w->cur.h > by1) by1 = w->cur.y + w->cur.h;
    }
    u8 *lo2 = g_primPtr;
    win_drawAll_game();
    for (int i = 0; i < 10; i++)
        if (down[i]) g_windows[i].style = saved_style[i], g_windows[i].shadow = saved_shadow[i];
    if (g_primPtr != lo2) {
        void port_bottom_shift(const void *, const void *, int, int);
        port_bottom_packets((const void *)PSX_PTR(lo2), (const void *)PSX_PTR(g_primPtr));
        /* (the ending's text: in the middle of the screen, which shows nothing else) */
        port_bottom_shift((const void *)PSX_PTR(lo2), (const void *)PSX_PTR(g_primPtr), (320 - (bx1 - bx0)) / 2 - bx0,
                          mid ? (240 - (by1 - by0)) / 2 - by0 : 12 - by0);
    }
    for (int i = 0; i < 10; i++) g_windows[i].visible = (u8)(up[i] | down[i] | hide[i]);
}

/* Whether a menu is up (menu_doOpen .. menu_finish): 0 no, 1 opening (the scene fades out), 2 showing (the scene is
 * hidden and the frame is all menu), 3 the card game (mode 9: all of it UI, on both screens). The 3DS shows a menu on its bottom screen and freezes the scene on the top. */
int port_menu_state(void) {
    GameData *g = g_gameState ? g_gameState->gameData : NULL;
    if (!g) return 0;
    if (g_gameState->mode == 9) return 3; /* (the card game: all UI, on both screens) */
    /* (the battle's end (ovl_65: EXP / AP, gil, items, level-ups) is its own screen, not the menu system's: a menu like
     * the others, to the bottom screen, the battle's last frame dimmed on top) */
    { /* (unless they ran under the battle already: then the mode passes, its frames black; through the whole mode,
       * which outlasts the state) */
        int port_rewards_live_state(void);
        static int passed;
        if (g_gameState->mode != 8) passed = 0;
        /* (a game over is mode 8 too: its picture is the screen, on the top as on the PS1. As a menu the top held the
         * battle's last frame, faded out: black, and the picture's fade went to the bottom (the user's report)) */
        else if (g->battleResult == BATTLE_END_GAME_OVER) return 0;
        else {
            if (port_rewards_live_state() == 3) passed = 1;
            return passed ? 0 : 2;
        }
    }
    /* (the world map's own full map (Select; the airship's Go to): the world stops and the frame is the map, as a
     * menu: to the bottom screen, the world kept dimmed on the top) */
    { extern s32 g_worldNaviMode; if (g_gameState->mode == 3 && g_worldNaviMode == 2) return 2; }
    /* (the field's battle transition sets these flags too, with the menu system idle: not a menu) */
    if (!(g->flags & GD_MENU_OPEN) || g_menu.state == 0) return 0;
    return (g->flags & GD_HIDE_ALL) == GD_HIDE_ALL ? 2 : 1; /* (events hide parts of the scene by these flags too) */
}

/* a window is closing (a speech bubble shrinking away): render.c keeps the scene it holds for a menu's top screen as it
 * was before, the bubble whole (a menu opened by a talk showed the bubble half shrunk, its text cut: "der.") */
int port_window_closing(void) {
    for (int i = 0; i < 10; i++) {
        const Window *w = &g_windows[i];
        if (w->state == WIN_STATE_CLOSING || w->state == WIN_STATE_OPENING) return 1;
        /* (any not yet at its size: a question's box shrinking as its answer is taken ("on't buy") before its state
         * says so) */
        if (w->state != WIN_STATE_CLOSED && w->visible && (w->cur.w < w->w || w->cur.h < w->h)) return 1;
    }
    return 0;
}

/* how dark the game's screen fade has the scene now, 0 (clear) to 256 (black), for the bottom screen to follow: the
 * darkening fade (subtract) of a field or the world map: a flat one's colour, a wipe's frame (covered: towards black,
 * else from it) */
/* (an ending narration's text is above the fade, as on the PS1: the bottom isn't dimmed while it is held) */
int port_bottom_fade(void) {
    int port_ending_fade_held(void);
    return port_ending_fade_held() ? 0 : fade_level();
}
static int fade_level(void) {
    if (!g_gameState || (g_gameState->mode != 1 && g_gameState->mode != 3)) return 0;
    int port_field_current(void);
    /* (a field still loading: the top is black without the fade (after a battle, a disc's load), the bottom too) */
    if (g_gameState->mode == 1 && !port_field_current()) return 256;
    if (!g_fade.subtract || (!g_fade.active && !g_fade.covered)) return 0;
    if (g_fade.type == FADE_FLAT) { /* as dark as its colour (Hot and Cold's "Time's Up" dims the scene to 32 only) */
        int c = g_fade.color.b[0] > g_fade.color.b[1] ? g_fade.color.b[0] : g_fade.color.b[1];
        if (g_fade.color.b[2] > c) c = g_fade.color.b[2];
        return c * 256 / 255;
    }
    if (!g_fade.active) return g_fade.covered ? 256 : 0;
    int f = g_fade.frames > 0 ? g_fade.frame * 256 / g_fade.frames : 256;
    if (f < 0) f = 0;
    if (f > 256) f = 256;
    return g_fade.covered ? f : 256 - f;
}

/* the game's screen fade is running (render.c: a closed menu's paused scene kept on the top meanwhile) */
int port_fade_busy(void) {
    return fade_isActive();
}

/* the game's mode (g_gameState->mode: 1 field, 2 battle, 3 world map, ...) */
int port_game_mode(void) {
    return g_gameState ? g_gameState->mode : 0;
}

/* the current field's id (the 3DS's heartbeat), -1 outside one */
int port_field_id(void) {
    return g_gameState && g_gameState->gameData && g_gameState->mode == 1 ? g_gameState->gameData->fieldId : -1;
}
