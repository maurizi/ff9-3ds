/* The Treno auction (field 909, decomp R40: ff9-decomp docs/r32/3ds-requests-10.md) on the bottom screen: the item's
 * price, who holds the top bid and the funds; while the bid entry is open, the bid large with a raise / lower pair
 * per digit (100s, 1000s, 10000s), BID and CANCEL. The bid is the entry-10 object's local Ls16[4] (in hundreds),
 * written directly within the script's own rules (above the price, at most the funds and 999); BID and CANCEL give
 * the bid loop the button it polls (Cross 0x20000, Circle 0x10000 in g_msgPadPressed between the message and the
 * object updates). The game's own bid windows (messages 0xC9-0xD0) stay off both screens' way: field_hud.c draws
 * this instead of the field's bottom screen. */
#define M2CTX
#include "ff9.h"

#define AUCTION_FIELD 909
static int pending; /* 0 none, 1 bid, 2 cancel, 3 open the bid entry */

static u8 *bid_vars(void) {
    EvtObject *o = evt_getObject(10);
    return o ? (u8 *)o + o->varsOfs * 4 : NULL;
}
static int price(void) {
    const u8 *v = g_evtState->fieldVars;
    return v[0x1B] | v[0x1C] << 8 | v[0x1D] << 16;
}
/* a bid can be opened: the player has control, another bidder holds the item (Fu8[0x39] 1-5), the auctioneer is free
 * (Fu8[0x3C] 0): then the bid region (uid 16) is what Cross at the bidding spot talks to (decomp R40) */
static int can_open(void) {
    const u8 *v = g_evtState->fieldVars;
    return g_evtState->controlEnabled && v[0x39] >= 1 && v[0x39] <= 5 && !v[0x3C] &&
           g_gameState->gameData->gil >= (u32)(v[0x1B] | v[0x1C] << 8 | v[0x1D] << 16) + 100;
}
static int entry_open(void) {
    for (int k = 0; k < 10; k++)
        if (g_msgWindows[k].open && g_msgWindows[k].msgId == 0xCB) return 1;
    return 0;
}
int port_auction_live(void) {
    return g_gameState && g_gameState->mode == 1 && g_gameState->gameData->fieldId == AUCTION_FIELD && g_evtState;
}

/* (field_hud.c's evt_updateObjects) */
void port_auction_step(void) {
    if (!pending || !port_auction_live()) { pending = 0; return; }
    if (pending == 3) {
        EvtObject *o = evt_getObject(16);
        if (o && can_open() && !entry_open()) evt_callObjectFunc(o, 1, 3, 0);
    } else if (entry_open()) g_msgPadPressed |= pending == 1 ? 0x20000 : 0x10000;
    pending = 0;
}

static RECT r_up[3], r_dn[3], r_bid = {24, 184, 132, 36}, r_cancel = {164, 184, 132, 36}, r_open = {60, 100, 200, 48};
static void label(const RECT *r, const char *t, int dim) {
    int w = ui_measureLabel((u8 *)t);
    ui_drawLabel(r->x + (r->w - w) / 2, r->y + (r->h - 6) / 2, 2, (s32)t, dim ? 2 : 0);
    win_drawBox((RECT *)r, 3, 2);
}
static void line(int x, int y, const char *name, int v) {
    u8 a[12];
    ui_drawLabel(x, y, 2, (s32)name, 0);
    text_formatNumber(v, a, 7, TEXT_NUM_ASCII);
    ui_drawDigits(x + 80, y - 1, 2, a, 0);
}
void port_auction_draw(void) {
    static RECT head = {16, 8, 288, 52};
    int p = price(), top = g_evtState->fieldVars[0x39];
    line(head.x + 10, head.y + 10, "PRICE", p);
    line(head.x + 10, head.y + 30, "FUNDS", (int)g_gameState->gameData->gil);
    ui_drawLabel(head.x + 190, head.y + 20, 2, (s32)(top == 0 ? "YOUR BID" : top == 255 ? "" : "OUTBID"), top == 0 ? 0 : 2);
    win_drawBox(&head, 3, 2);
    if (!entry_open()) {
        label(&r_open, "PLACE A BID", !can_open());
        return;
    }
    u8 *v = bid_vars();
    if (!v) return;
    int bid = *(s16 *)(v + 4), hi = (int)(g_gameState->gameData->gil / 100);
    if (hi > 999) hi = 999;
    static RECT box = {16, 70, 288, 106};
    u8 a[12];
    text_formatNumber(bid * 100 < 0 ? 0 : bid * 100, a, 5, TEXT_NUM_ASCII);
    ui_drawLabel(box.x + 12, box.y + 46, 2, (s32) "BID", 0);
    for (int d = 0; d < 3; d++) { /* (columns: 10000s, 1000s, 100s of the five digits) */
        int x = box.x + 64 + d * 44, step = d == 0 ? 100 : d == 1 ? 10 : 1;
        r_up[d] = (RECT){(s16)x, (s16)(box.y + 6), 38, 30};
        r_dn[d] = (RECT){(s16)x, (s16)(box.y + 70), 38, 30};
        label(&r_up[d], "UP", bid + step > hi);
        label(&r_dn[d], "DOWN", bid - step <= p / 100);
        u8 one[2] = {a[d], 0};
        ui_drawDigits(x + 15, box.y + 46, 2, one, 0);
    }
    ui_drawDigits(box.x + 64 + 3 * 44 + 4, box.y + 46, 2, (u8 *)"00", 0);
    ui_drawLabel(box.x + 64 + 3 * 44 + 30, box.y + 47, 2, (s32) "GIL", 0);
    win_drawBox(&box, 3, 2);
    label(&r_bid, "BID", bid * 100 <= p);
    label(&r_cancel, "CANCEL", 0);
}
static int in(const RECT *r, int x, int y) { return x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h; }
int port_auction_tap(int x, int y) {
    if (!entry_open()) {
        if (in(&r_open, x, y) && can_open()) pending = 3;
        return 1;
    }
    u8 *v = bid_vars();
    if (!v) return 1;
    s16 *bid = (s16 *)(v + 4);
    int lo = price() / 100, hi = (int)(g_gameState->gameData->gil / 100);
    if (hi > 999) hi = 999;
    for (int d = 0; d < 3; d++) {
        int step = d == 0 ? 100 : d == 1 ? 10 : 1;
        if (in(&r_up[d], x, y) && *bid + step <= hi) { *bid += step; v[1] = (u8)(2 - d); snd_playSe(SE_CURSOR); }
        if (in(&r_dn[d], x, y) && *bid - step > lo) { *bid -= step; v[1] = (u8)(2 - d); snd_playSe(SE_CURSOR); }
    }
    if (in(&r_bid, x, y) && *bid > lo) pending = 1;
    if (in(&r_cancel, x, y)) pending = 2;
    return 1;
}
