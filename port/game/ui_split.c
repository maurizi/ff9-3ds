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
 * not over the scene; its speech balloons stay with the speakers. 1: window w goes to the bottom screen. */
void port_bottom_packets(const void *lo, const void *hi);
/* (the minigames' panels by field and message id, decomp R38: ff9-decomp docs/r32/3ds-requests-8.md) */
static const struct { u16 field, lo, hi; } panels[] = {
    {103, 0xEA, 0xEB},   /* jump rope: "N times" (the girls' comments are speech: on the top) */
    {1850, 0x94, 0x94},  /* Hippaul racing: "Press Square and Circle alternately!" (Hippolady's lines and GO! are speech) */
    {657, 0x183, 0x183}, /* frog catching: "Frogs Captured: N" */
    {64, 0x6F, 0x6F},    /* the Prima Vista play's sword fight: its instructions */
    {64, 0x78, 0x80},    /* ... the nobles' verdict, the gil */
};
static int to_bottom(const Window *w) {
    if (g_gameState->mode != 1) return 0;
    int field = g_gameState->gameData->fieldId;
    if (field == 909) { /* (the Treno auction: its bid windows replaced by auction_ui.c's panel: off the top screen) */
        for (int k = 0; k < 10; k++)
            if (g_msgWindows[k].open && g_msgWindows[k].window == w && g_msgWindows[k].msgId >= 0xC9 && g_msgWindows[k].msgId <= 0xCE)
                return 2;
        return 0;
    }
    if (field == 2950) return w->style != WIN_STYLE_BALLOON; /* (Chocobo Hot and Cold: its how-to-play box, its score) */
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
long VSync(long);
int port_minigame_panel_up(void) { return VSync(-1) - panel_frame <= 2; }

void win_drawAll(void) {
    u8 *lo = g_primPtr;
    u8 down[10], hide[10], any = 0;
    for (int i = 0; i < 10; i++) { /* (the bottom screen's windows hidden from the first pass) */
        Window *w = &g_windows[i];
        int b = w->state != WIN_STATE_CLOSED && w->visible ? to_bottom(w) : 0;
        down[i] = b == 1;
        hide[i] = b == 2;
        if (b) w->visible = 0, any |= b == 1;
        if (b == 2) panel_frame = VSync(-1);
    }
    win_drawAll_game();
    if (g_primPtr != lo) port_ui_packets((const void *)PSX_PTR(lo), (const void *)PSX_PTR(g_primPtr));
    if (!any) {
        for (int i = 0; i < 10; i++) if (hide[i]) g_windows[i].visible = 1;
        return;
    }
    panel_frame = VSync(-1);
    u8 up[10];
    for (int i = 0; i < 10; i++) { /* (a second pass with only those, its packets to the bottom screen) */
        Window *w = &g_windows[i];
        up[i] = w->visible;
        w->visible = down[i];
    }
    /* on the bottom screen: a panel, not a balloon (no speech tail, no shadow); the packets moved as a block so the
     * first one sits centred along the top (port_bottom_shift: the game's own layout and text kept) */
    u8 saved_style[10], saved_shadow[10];
    int bx0 = 1 << 30, bx1 = -(1 << 30), by0 = 1 << 30;
    for (int i = 0; i < 10; i++) {
        if (!down[i]) continue;
        Window *w = &g_windows[i];
        saved_style[i] = w->style, saved_shadow[i] = w->shadow;
        if (w->style == WIN_STYLE_BALLOON) w->style = WIN_STYLE_BEVEL;
        w->shadow = 0;
        if (w->cur.x < bx0) bx0 = w->cur.x;
        if (w->cur.x + w->cur.w > bx1) bx1 = w->cur.x + w->cur.w;
        if (w->cur.y < by0) by0 = w->cur.y;
    }
    u8 *lo2 = g_primPtr;
    win_drawAll_game();
    for (int i = 0; i < 10; i++)
        if (down[i]) g_windows[i].style = saved_style[i], g_windows[i].shadow = saved_shadow[i];
    if (g_primPtr != lo2) {
        void port_bottom_shift(const void *, const void *, int, int);
        port_bottom_packets((const void *)PSX_PTR(lo2), (const void *)PSX_PTR(g_primPtr));
        port_bottom_shift((const void *)PSX_PTR(lo2), (const void *)PSX_PTR(g_primPtr), (320 - (bx1 - bx0)) / 2 - bx0, 12 - by0);
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
    if (g_gameState->mode == 8) return 2;
    /* (the world map's own full map (Select; the airship's Go to): the world stops and the frame is the map, as a
     * menu: to the bottom screen, the world kept dimmed on the top) */
    { extern s32 g_worldNaviMode; if (g_gameState->mode == 3 && g_worldNaviMode == 2) return 2; }
    /* (the field's battle transition sets these flags too, with the menu system idle: not a menu) */
    if (!(g->flags & GD_MENU_OPEN) || g_menu.state == 0) return 0;
    return (g->flags & GD_HIDE_ALL) == GD_HIDE_ALL ? 2 : 1; /* (events hide parts of the scene by these flags too) */
}

/* the game's mode (g_gameState->mode: 1 field, 2 battle, 3 world map, ...) */
int port_game_mode(void) {
    return g_gameState ? g_gameState->mode : 0;
}

/* the current field's id (the 3DS's heartbeat), -1 outside one */
int port_field_id(void) {
    return g_gameState && g_gameState->gameData && g_gameState->mode == 1 ? g_gameState->gameData->fieldId : -1;
}
