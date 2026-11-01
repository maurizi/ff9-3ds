/* Tetra Master (mode 9, overlay 0D) on two screens (docs/ui-design.md), from the decomp's answers (ff9-decomp
 * docs/r32/3ds-requests-4.md):
 *   bottom  the game's own frame 1:1 (render.c), the play surface: board, hands, stock, its windows; touch acts on it
 *           directly
 *   top     the card in focus large (picture, name, stats, arrows), the score and whose turn, and the game's hint
 *           text (the hint bubbles would cover the board): port_tm_top_draw
 * Touch: a tap is queued; at the card game's next frame, right after its pad read (port_tm_frame, a counted patch in
 * tm_mainLoop: config/patches.txt), the port sets the game's cursor fields to what was tapped (a stock cell, a hand
 * card, a board square, Yes/No, the card to take) and adds Cross to this frame's pad, so the game's own confirm block
 * runs in its own handler, as the decomp says it must. Nothing walks a cursor. */
#define M2CTX
#include "ovl_0d.h"

static TmState *tm;         /* the card game's state (a local of tm_mainLoop), this frame's */
static int tap_pending, tap_x, tap_y;
static int clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* a tap at (x, y) in the frame's coordinates: -1 handled, 0 not the card game */
int port_tm_tap(int x, int y) {
    if (!g_gameState || g_gameState->mode != GS_MODE_TETRA_MASTER || !tm) return 0;
    tap_pending = 1;
    tap_x = x;
    tap_y = y;
    return -1;
}

static int in(int x, int y, int rx, int ry, int rw, int rh) { return x >= rx && x < rx + rw && y >= ry && y < ry + rh; }

/* the player's n-th card still in hand (tm_getHandCard's order), its index in cards[], -1 none */
static int hand_card(TmState *s, int n) {
    for (int i = 0; i < 10; i++)
        if (s->cards[i].owner == TM_PLAYER && !s->cards[i].onBoard && n-- == 0) return i;
    return -1;
}
/* the n-th enemy-hand card the player now owns (tm_getCaptured's order), -1 none */
static int captured(TmState *s, int n) {
    for (int i = 5; i < 10; i++)
        if (s->cards[i].owner == TM_PLAYER && n-- == 0) return i;
    return -1;
}
static int card_hit(TmCard *c, int x, int y) {
    int w = c->small ? 34 : 42, h = c->small ? 41 : 51;
    return in(x, y, c->x, c->y, w, h);
}
static int board_hit(int x, int y, int *bx, int *by) {
    if (!in(x, y, 72, 8, 4 * 43, 4 * 52)) return 0;
    *bx = clamp((x - 72) / 43, 0, 3);
    *by = clamp((y - 8) / 52, 0, 3);
    return 1;
}

/* the queued tap, as the game's cursor and a Cross this frame */
static void apply(TmState *s) {
    int x = tap_x, y = tap_y, bx, by, cross = 0;
    tap_pending = 0;
    switch (s->task) {
    case TM_TASK_CHOICE:
    case TM_TASK_CARD_AWAY: /* the stock grid: cell id i at ((i/10)*15 + 0x12, (i%10)*15 + 0x2D), column-major */
        if (in(x, y, 0x12, 0x2D, 150, 150)) {
            s->stockCol = clamp((x - 0x12) / 15, 0, 9);
            s->stockRow = clamp((y - 0x2D) / 15, 0, 9);
            s->copyIdx = 0;
            cross = 1;
        }
        break;
    case TM_TASK_CHOICE2:
    case TM_TASK_TRY_AGAIN:
    case TM_TASK_CARD_AWAY2:
    case TM_TASK_GAME_QUIT: { /* a Yes/No window: Yes on its upper row, No on the lower */
        if (g_tmWindowOpening) break;
        int yes_y = s->task == TM_TASK_CARD_AWAY2 ? 0x4D : s->task == TM_TASK_TRY_AGAIN ? 0x6B : 0x6C;
        if (!in(x, y, 0x50, yes_y - 10, 0xA0, 0x24)) break;
        int choice = y >= yes_y + 7;
        if (s->task == TM_TASK_GAME_QUIT) s->quitChoice = choice;
        else s->cursor = choice;
        cross = 1;
        break;
    }
    case TM_TASK_PLAYER_SELECT:
    case TM_TASK_PLAYER_SET: {
        for (int n = 0; n < 5; n++) { /* a hand card: that card picked */
            int i = hand_card(s, n);
            if (i < 0 || !card_hit(&s->cards[i], x, y)) continue;
            if (s->task == TM_TASK_PLAYER_SET) { /* (back to picking, as Circle does) */
                s->nameSlide = 0;
                s->task = TM_TASK_PLAYER_SELECT;
                s->animFrame = s->savedFrame;
            }
            s->cursor = n;
            cross = 1;
            break;
        }
        if (cross || !board_hit(x, y, &bx, &by)) break;
        if (s->task == TM_TASK_PLAYER_SELECT) { /* a square with a card picked: straight to placing it */
            if (s->cursor >= 5 || hand_card(s, s->cursor) < 0) break;
            s->task = TM_TASK_PLAYER_SET;
            s->savedFrame = s->animFrame;
        }
        s->boardX = bx;
        s->boardY = by;
        cross = 1;
        break;
    }
    case TM_TASK_BATTLE_SELECT:
        if (board_hit(x, y, &bx, &by)) s->boardX = bx, s->boardY = by, cross = 1;
        break;
    case TM_TASK_GET_SELECT:
        for (int n = 0; n < 5; n++) {
            int i = captured(s, n);
            if (i >= 0 && card_hit(&s->cards[i], x, y)) { s->getCursor = n; cross = 1; break; }
        }
        break;
    case TM_TASK_RESULT:
    case TM_TASK_TRADE:
    case TM_TASK_EXTENSION:
    case TM_TASK_AWAY_INFO:
    case TM_TASK_CARD_LESS5: /* "any button" */
        cross = 1;
        break;
    default:
        break;
    }
    if (cross) g_padRepeat |= BTN_CROSS;
}

/* tm_mainLoop, each frame after its pad read (the patch) */
void port_tm_frame(void *state) {
    TmState *s = state;
    tm = s;
    if (tap_pending) apply(s);
}
/* (tm_mainLoop ended: no state; port/game/field_hud.c's frame asks) */
static int tm_live(void) { return tm && g_gameState->mode == GS_MODE_TETRA_MASTER; }

/* ---- the top screen ---- */
static u8 *prim(int words) {
    u8 *p = g_primPtr;
    g_primPtr += words * 4;
    return p;
}
/* card id's picture from VRAM (tm_drawCardFace's), scaled to w x h at (x, y) */
static void card_face(int id, int x, int y, int w, int h) {
    u32 *p = (u32 *)prim(10);
    int u = (id % 30 % 6) * 42, v = (id % 30 / 6) * 51;
    u32 clut = (u32)((0x19C + id) << 6 | 0x180 >> 4);
    u32 tpage = 1u << 7 | (0x100 >> 4) | (u32)((id / 30) * 128 >> 6);
    p[0] = 9u << 24;
    p[1] = 0x2C808080u;
    p[2] = (u32)x | (u32)y << 16;
    p[3] = clut << 16 | (u32)v << 8 | (u32)u;
    p[4] = (u32)(x + w) | (u32)y << 16;
    p[5] = tpage << 16 | (u32)v << 8 | (u32)(u + 41);
    p[6] = (u32)x | (u32)(y + h) << 16;
    p[7] = (u32)(v + 50) << 8 | (u32)u;
    p[8] = (u32)(x + w) | (u32)(y + h) << 16;
    p[9] = (u32)(v + 50) << 8 | (u32)(u + 41);
    addPrim(&g_ot[3], p);
}
/* the Card screen's card picture (ovl_36 menuCard_drawFace: 38x47, 8-bit, page (960, 256), uv (0, 0xB0), its CLUT at
 * (0, 0x1F2)) scaled to w x h at (x, y) */
static void menu_card_face(int x, int y, int w, int h) {
    u32 *p = (u32 *)prim(10);
    u32 clut = (u32)(0x1F2 << 6), tpage = 0xBF;
    p[0] = 9u << 24;
    p[1] = 0x2C808080u;
    p[2] = (u32)x | (u32)y << 16;
    p[3] = clut << 16 | (u32)0xB0 << 8 | 0;
    p[4] = (u32)(x + w) | (u32)y << 16;
    p[5] = tpage << 16 | (u32)0xB0 << 8 | 38;
    p[6] = (u32)x | (u32)(y + h) << 16;
    p[7] = (u32)(0xB0 + 47) << 8 | 0;
    p[8] = (u32)(x + w) | (u32)(y + h) << 16;
    p[9] = (u32)(0xB0 + 47) << 8 | 38;
    addPrim(&g_ot[3], p);
}
/* a small arrow (triangle) at (cx, cy) pointing in direction d (0 up, clockwise in eighths) */
static void arrow(int cx, int cy, int d) {
    static const s8 dir[8][2] = {{0, -1}, {1, -1}, {1, 0}, {1, 1}, {0, 1}, {-1, 1}, {-1, 0}, {-1, -1}};
    int dx = dir[d][0], dy = dir[d][1];
    u32 *p = (u32 *)prim(5);
    p[0] = 4u << 24;
    p[1] = 0x2030D0F0u;
    p[2] = (u32)(cx + dx * 7) | (u32)(cy + dy * 7) << 16;
    p[3] = (u32)(cx - dy * 5) | (u32)(cy + dx * 5) << 16;
    p[4] = (u32)(cx + dy * 5) | (u32)(cy - dx * 5) << 16;
    addPrim(&g_ot[2], p);
}
static int focus_card(TmState *s, TmCardStats *st) {
    int i = -1;
    switch (s->task) {
    case TM_TASK_CHOICE:
    case TM_TASK_CARD_AWAY: {
        int id = s->stockCol * 10 + s->stockRow;
        CardEntry *e = card_find(id, s->copyIdx);
        if (!e || e == (CardEntry *)-1) return -1; /* (no copy: card_find gives -1) */
        st->id = e->id, st->arrows = e->arrows, st->power = e->power, st->type = e->type;
        st->pdef = e->physDef, st->mdef = e->magicDef;
        return id;
    }
    case TM_TASK_PLAYER_SELECT:
    case TM_TASK_PLAYER_SET: i = hand_card(s, s->cursor); break;
    case TM_TASK_GET_SELECT: i = captured(s, s->getCursor); break;
    case TM_TASK_TRADE:
    case TM_TASK_CARD_CHANGE:
    case TM_TASK_CHANGE2:
        if (s->tradeCard) i = (int)(s->tradeCard - s->cards);
        break;
    default:
        if (s->srcX >= 0 && s->srcX < 4 && s->srcY >= 0 && s->srcY < 4 && s->board[s->srcY][s->srcX].state == TM_CELL_CARD)
            i = (int)(s->board[s->srcY][s->srcX].card - s->cards);
        break;
    }
    if (i < 0 || i >= 10 || s->cards[i].hidden) return -1;
    TmCard *c = &s->cards[i];
    st->id = c->id, st->arrows = c->arrows, st->power = c->power, st->type = c->type, st->pdef = c->pdef, st->mdef = c->mdef;
    return c->id;
}

/* a card large in the top screen's card box (picture, its arrows) and its name and "1P2A" string in the info box: the
 * card game's focus and the Card screen's (card_touch.c) */
void port_card_panel(int id, int arrows, int power, int type, int pdef, int mdef, int menu) {
    static RECT info = {174, 12, 210, 176};
    if (menu == 1) menu_card_face(30, 26, 122, 148); /* (the Card screen's picture: menu 1; menu 2: not loaded yet) */
    else if (!menu) card_face(id, 30, 26, 122, 148); /* (about 3x) */
    for (int d = 0; d < 8; d++)
        if (arrows >> d & 1) {
            static const s8 at[8][2] = {{61, -6}, {128, -6}, {128, 74}, {128, 154}, {61, 154}, {-6, 154}, {-6, 74}, {-6, -6}};
            arrow(30 + at[d][0], 26 + at[d][1] + 6, d);
        }
    text_drawBlock(info.x + 10, info.y + 12, 2, (u8 *)card_getName(id), 0);
    static const char hex[] = "0123456789ABCDEF", tp[] = "PMXA";
    u8 str[5] = {(u8)hex[power >> 4 & 15], (u8)tp[type & 3], (u8)hex[pdef >> 4 & 15], (u8)hex[mdef >> 4 & 15], 0};
    ui_drawLabel(info.x + 10, info.y + 38, 2, (s32)str, 0); /* (the label font: the digit font has no letters) */
}

void port_help_panel(u8 *text);
int port_tm_top_draw(void) {
    if (!tm_live()) return 0;
    TmState *s = tm;
    static RECT card_box = {16, 12, 150, 176}, info = {174, 12, 210, 176};
    TmCardStats st;
    int id = focus_card(s, &st);
    if (id >= 0 && id < 100) port_card_panel(id, st.arrows, st.power, st.type, st.pdef, st.mdef, 0);
    if (s->showScore) { /* the score and whose turn */
        int you = 0, foe = 0;
        for (int i = 0; i < 10; i++)
            if (s->cards[i].onBoard) { if (s->cards[i].owner == TM_PLAYER) you++; else foe++; }
        u8 a[4];
        ui_drawLabel(info.x + 10, info.y + 120, 2, (s32) "YOU", s->turn == 0 ? 1 : 0);
        text_formatNumber(you, a, 2, 1);
        ui_drawDigits(info.x + 50, info.y + 118, 2, a, 0);
        ui_drawLabel(info.x + 110, info.y + 120, 2, (s32) "FOE", s->turn == 1 ? 1 : 0);
        text_formatNumber(foe, a, 2, 1);
        ui_drawDigits(info.x + 150, info.y + 118, 2, a, 0);
    }
    win_drawBox(&card_box, 5, 2);
    win_drawBox(&info, 5, 2);
    if (g_menuHelp.isOpen && g_menuHelp.text && *g_menuHelp.text != TEXT_END) port_help_panel(g_menuHelp.text);
    return 1;
}
