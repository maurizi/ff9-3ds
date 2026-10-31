/* The shops' quantity windows by touch, directly (Buy, Sell, Synthesis; overlay 3C): in the window, a tap on its upper
 * half's left side takes one off and on its right side adds one (as Left / Right do, to the game's limits, with its
 * sound), a tap on its lower half (the price) buys or sells (Cross: the game's own confirm). */
#define M2CTX
#include "ovl_3c.h"

/* the quantity window up for the shop mode running: its rect, quantity and limit */
static int quantity_window(RECT **r, u8 **q, u8 *max) {
    if (g_menu.type != 2 || !s_shop.inMode) return 0;
    if (s_shop.kind == 2) { /* synthesis */
        if (s_shopSynth.state != SHOP_ST_QUANTITY) return 0;
        *r = &s_shopSynth.quantityRect, *q = &s_shopSynth.quantity, *max = s_shopSynth.maxQuantity;
    } else if (s_shop.cursor == 0) { /* buy */
        if (s_shopBuy.state != SHOP_ST_QUANTITY) return 0;
        *r = &s_shopBuy.quantityRect, *q = &s_shopBuy.quantity, *max = s_shopBuy.maxQuantity;
    } else { /* sell */
        if (s_shopSell.state != SHOP_ST_QUANTITY) return 0;
        *r = &s_shopSell.winRects[0], *q = &s_shopSell.quantity, *max = s_shopSell.maxQuantity;
    }
    return 1;
}

/* a tap at (x, y) in the frame's coordinates: 1 with a press in *press, -1 taken, 0 not here */
int port_shop_tap(int x, int y, unsigned *press) {
    RECT *r;
    u8 *q, max;
    if (!g_gameState || !quantity_window(&r, &q, &max) || fade_isBusy()) return 0;
    if (x < r->x || x >= r->x + r->w || y < r->y || y >= r->y + r->h) return -1;
    if (y >= r->y + r->h / 2) { *press = 0x4000; return 1; } /* (the price: buy / sell) */
    int n = *q + (x >= r->x + r->w / 2 ? 1 : -1);
    if (n >= 1 && n <= max) {
        snd_playSe(SE_CURSOR);
        *q = (u8)n;
    }
    return -1;
}
