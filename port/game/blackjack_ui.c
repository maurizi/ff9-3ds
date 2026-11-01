/* Blackjack (the ending's secret, ovl_15) on the bottom screen: its wager screen as WAGER - / + and DEAL, its moves
 * as STAND, HIT, DOUBLE and SPLIT (dim when the game doesn't allow them), with the bankroll and the bet; the table
 * and cards stay on the top. A tap acts directly at the menu's own input step (bj_wagerUpdate / bj_actionUpdate
 * wrapped: config/renames.txt): the wager changed as Left / Right would, the move's cursor set and its own decide
 * run. Drawn by field_hud.c (port_bj_draw). */
#define M2CTX
#include "ovl_15.h"

void o15_bj_wagerUpdate_game(void), o15_bj_actionUpdate_game(void);
void o15_bj_wagerDecide(void), o15_bj_actionDecide(void);
s32 o15_ending_theEnd_game(void);
s32 o15_ending_playMovie_game(s32 list, s32 entry);
int port_scenario_blackjack(void);
long VSync(long);

static long wager_frame = -100, action_frame = -100;
static int tap = -1; /* wager: 0 minus, 1 plus, 2 deal; action: 10 + move */

static int live(long f) { return VSync(-1) - f <= 2; }
static int ready(const SceneMenu *m) { return m->shown && !m->fadingIn; }

void o15_bj_wagerUpdate(void) {
    if (tap >= 0 && tap < 10 && ready(&g_bjWagerMenu)) {
        if (tap == 2) {
            tap = -1;
            pad_updateRepeat(0);
            o15_bj_wagerDecide();
            wager_frame = VSync(-1);
            return;
        }
        if (tap == 1) {
            g_bjWager += BJ_MIN_BET;
            if (g_bjBankroll < g_bjWager) g_bjWager = g_bjBankroll / BJ_MIN_BET * BJ_MIN_BET;
            if (g_bjWager > BJ_MAX_BET) g_bjWager = BJ_MAX_BET;
        } else {
            g_bjWager -= BJ_MIN_BET;
            if (g_bjWager < BJ_MIN_BET) g_bjWager = BJ_MIN_BET;
        }
        snd_playSe(SE_CURSOR);
    }
    tap = -1;
    o15_bj_wagerUpdate_game();
    wager_frame = VSync(-1);
}

void o15_bj_actionUpdate(void) {
    if (tap >= 10 && ready(&g_bjAction)) {
        int mv = tap - 10;
        tap = -1;
        if ((mv == BJ_DOUBLE && !g_bjCanDouble) || (mv == BJ_SPLIT && !g_bjCanSplit)) snd_playSe(SE_BUZZER);
        else {
            g_bjAction.cursor = (u8)mv;
            pad_updateRepeat(0);
            o15_bj_actionDecide();
            action_frame = VSync(-1);
            return;
        }
    }
    tap = -1;
    o15_bj_actionUpdate_game();
    action_frame = VSync(-1);
}

/* (scenario blackjack=1: straight to the table) */
s32 o15_ending_playMovie(s32 list, s32 entry) { return port_scenario_blackjack() ? 0 : o15_ending_playMovie_game(list, entry); }
s32 o15_ending_theEnd(void) {
    static int once;
    if (port_scenario_blackjack() && !once) { once = 1; return 1; }
    return o15_ending_theEnd_game();
}

/* ---- the bottom screen (field_hud.c's list) ---- */
static RECT r_minus = {24, 96, 72, 44}, r_plus = {224, 96, 72, 44}, r_deal = {80, 156, 160, 40};
static RECT r_move[4] = {{24, 60, 132, 56}, {164, 60, 132, 56}, {24, 124, 132, 56}, {164, 124, 132, 56}};
static void label(const RECT *r, const char *t, int dim) {
    int w = ui_measureLabel((u8 *)t);
    ui_drawLabel(r->x + (r->w - w) / 2, r->y + (r->h - 6) / 2, 2, (s32)t, dim ? 2 : 0);
    win_drawBox((RECT *)r, 3, 2);
}
static void money(int x, int y, const char *name, int v) {
    u8 a[12];
    ui_drawLabel(x, y, 2, (s32)name, 0);
    text_formatNumber(v, a, 5, TEXT_NUM_ASCII);
    ui_drawDigits(x + 80, y - 1, 2, a, 0);
}
int port_bj_live(void) { return live(wager_frame) || live(action_frame); }
void port_bj_draw(void) {
    static RECT head = {24, 16, 272, 32};
    money(head.x + 10, head.y + 12, "BANKROLL", g_bjBankroll);
    money(head.x + 150, head.y + 12, "BET", g_bjWager);
    win_drawBox(&head, 3, 2);
    if (live(wager_frame)) {
        label(&r_minus, "LESS", g_bjWager <= BJ_MIN_BET);
        label(&r_plus, "MORE", g_bjWager >= BJ_MAX_BET || g_bjWager + BJ_MIN_BET > g_bjBankroll);
        static RECT mid = {104, 96, 112, 44};
        u8 a[12];
        text_formatNumber(g_bjWager, a, 4, TEXT_NUM_ASCII);
        ui_drawDigits(mid.x + 34, mid.y + 18, 2, a, 0);
        win_drawBox(&mid, 3, 2);
        label(&r_deal, "DEAL", 0);
        return;
    }
    static const char *const moves[4] = {"STAND", "HIT", "DOUBLE", "SPLIT"};
    for (int i = 0; i < 4; i++) {
        int dim = (i == BJ_DOUBLE && !g_bjCanDouble) || (i == BJ_SPLIT && !g_bjCanSplit);
        label(&r_move[i], moves[i], dim);
        if (i == g_bjAction.cursor) { /* (the D-pad's choice) */
            u32 *p = (u32 *)g_primPtr;
            p[0] = 3u << 24; p[1] = 0x6040E0F8u;
            p[2] = (u32)(r_move[i].x + 4) | (u32)(r_move[i].y + 8) << 16;
            p[3] = 3u | (u32)(r_move[i].h - 16) << 16;
            addPrim(&g_ot[2], p);
            g_primPtr = (u8 *)(p + 4);
        }
    }
}
static int in(const RECT *r, int x, int y) { return x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h; }
int port_bj_tap(int x, int y) {
    if (live(wager_frame)) {
        if (in(&r_minus, x, y)) tap = 0;
        else if (in(&r_plus, x, y)) tap = 1;
        else if (in(&r_deal, x, y)) tap = 2;
        return 1;
    }
    for (int i = 0; i < 4; i++)
        if (in(&r_move[i], x, y)) tap = 10 + i;
    return 1;
}
