/* Battle on the 3DS's two screens (docs/ui-design.md). The top screen is the scene alone (with the target hands:
 * render.c). The bottom screen, while the command menu is up or no menu is (animations), is the port's own:
 *
 *   left:  the active member's commands as large rows (g_btlCmdMenu: ids, enabled, cursor; names from the
 *          command table), and DEFEND / CHANGE (the PS1's left / right slides)
 *   right: a card per party member: portrait, name, status icons, HP, MP, the ATB bar and the Trance bar
 *
 * and while an ability, item or throw list is open, the list in its place: six large rows drawn by the game's own row
 * functions (names, MP costs, item icons and counts, greyed entries: g_btluiListRowFns, each pointed at its row), the
 * highlighted entry's description (the game's help text for it), a BACK button (circle), and the party compact.
 *
 * drawn with the game's own text, label, digit and window code over the battle's UI list (field_hud.c's packet
 * buffer and its no-clear UI list), the PS1's command window and party panel left out (port_battle_hud_on: render.c
 * skips their OT entries). While an ability or item list or the target window is open, the game's own windows show
 * (with "Target Window: Always" forced for the battle, so targets are a list there too). A tap on a row moves the
 * game's cursor there and confirms; on a ready member's card, triangle (the PS1's switch to the next ready member).
 * While targeting (the game's target window), the targets: a large button per enemy (left) and party member
 * (right), marked when targeted, ALL / ONE (R1) and BACK. A tap on a unit steers the game's cursor to it (a press per
 * frame, watching what the game targets: port_battle_drive), a tap on the targeted one confirms.
 * Game logic and ATB timing are the game's: nothing here changes what a button does. */
#include "psyq_port.h"
#include "ovl_0b.h"

#define TARGET_WINDOW (1u << CFG_BIT_TARGET_WINDOW)
#define PAD_UP_ 0x10u
#define PAD_RIGHT_ 0x20u
#define PAD_DOWN_ 0x40u
#define PAD_LEFT_ 0x80u
#define PAD_TRIANGLE_ 0x1000u
#define PAD_CROSS_ 0x4000u
#define TARGET_PANEL 1
#define HOLD(n) ((u32)(n) << 24) /* (a press held n more frames: port/3ds/touch.c) */

int port_battle_hud_on; /* the bottom screen is the port's this frame (render.c leaves the PS1 windows out) */
int port_battle_hud_targets; /* ... its targets too (render.c leaves the target window out) */

void ctr_hud_image(int slot, u32 key, const u16 *clut, const u8 *pix, int w, int h);
void ctr_hud_draw(int slot, int x, int y, int w, int h);
const u8 *port_portrait(int index); /* field_hud.c: the menu's portrait entry (0x6C0 bytes), NULL if unread */
const u8 *port_portrait_face(int index); /* field_hud.c: its face, 32x32 8-bit pixels */

void port_battle_ui_frame(void) {
    static int forced, had;
    if (!g_gameState) return;
    int battle = g_gameState->mode == 2;
    if (battle && !forced) {
        had = (g_gameState->config & TARGET_WINDOW) != 0;
        g_gameState->config |= TARGET_WINDOW;
        forced = 1;
    } else if (!battle && forced) {
        if (!had) g_gameState->config &= ~TARGET_WINDOW;
        forced = 0;
    }
}

static RECT r_cmd[4];
static RECT r_defend = {6, 186, 70, 30}, r_change = {80, 186, 70, 30};
static RECT r_cards = {156, 42, 160, 176};
static BtlUnit *card_unit[4];
static int cmd_shown;

static int inside(const RECT *r, int x, int y) { return x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h; }

extern unsigned char psx_ram[];
static int ram_ok(const void *p, unsigned size) { /* (as field_hud.c's) */
    const unsigned char *b = (const unsigned char *)p;
    return b >= psx_ram + 0x10000 && b + size <= psx_ram + 0x200000;
}

static BtlUnit *party_unit(int i) {
    int n = 0, guard = 0;
    for (BtlUnit *u = g_gameState->gameData->ctx.battle->unitList; ram_ok(u, sizeof *u) && guard++ < 32; u = u->next)
        if (u->flags & 1) {
            if (n == i) return u;
            n++;
        }
    return NULL;
}

/* a solid bar (a TILE packet) into ot */
static void bar(u32 *ot, int x, int y, int w, int h, u32 bgr) {
    if (w <= 0) return;
    u32 *p = (u32 *)g_primPtr;
    p[0] = 3u << 24;
    p[1] = 0x60000000u | bgr;
    p[2] = (u32)(x & 0xFFFF) | (u32)y << 16;
    p[3] = (u32)w | (u32)h << 16;
    addPrim(ot, p);
    g_primPtr = (u8 *)(p + 4);
}

static void draw_cards(void) {
    static const u8 hp[] = "HP", mp[] = "MP";
    u8 a[16];
    int y = r_cards.y + 4;
    for (int i = 0; i < 4; i++, y += 43) {
        BtlUnit *u = card_unit[i] = party_unit(i);
        if (!u || !ram_ok(u->menu, 16) || !ram_ok(u->menu->chr, sizeof(Character)) || !ram_ok(u->max, 8)) continue;
        Character *c = u->menu->chr;
        int x = r_cards.x + 4;
        const u8 *e = port_portrait(g_portraitByKind[c->info.b.kind]);
        if (e) {
            ctr_hud_image(4 + i, 0x200u | (u32)g_portraitByKind[c->info.b.kind], (const u16 *)e,
                          port_portrait_face(g_portraitByKind[c->info.b.kind]), 32, 32);
            ctr_hud_draw(4 + i, x, y + 1, 32, 32);
        }
        int dead = u->hp == 0;
        text_drawBlock(x + 38, y, 2, c->name, dead ? 2 : 0);
        { /* its statuses: the game's icons (the battle status list's table), up to four after the name */
            u32 st = u->status | u->autoStatus;
            int ix = x + 100, n = 0;
            for (int k = 0; k < 16 && n < 4; k++)
                if (st & g_btluiStatusIcons[k].status) {
                    ui_drawSprite(ix, y + 1, 2, g_btluiStatusIcons[k].icon, 0);
                    ix += 12;
                    n++;
                }
        }
        int low = dead ? 2 : (u->max->maxHp / 6 >= u->hp) ? 2 : 0;
        ui_drawLabel(x + 38, y + 14, 2, (s32)hp, 0);
        text_formatNumber(u->menu->gauges[0].shown, a, 4, 1);
        ui_drawSmallDigits(x + 52, y + 15, 2, a, low);
        ui_drawLabel(x + 84, y + 14, 2, (s32)mp, 0);
        text_formatNumber(u->menu->gauges[1].shown, a, 3, 1);
        ui_drawSmallDigits(x + 98, y + 15, 2, a, u->max->maxMp / 6 >= u->mp ? 2 : 0);
        /* the ATB bar: white when the member is ready, the trance bar under it */
        int ready = u->inMenuQueue && !u->cmdIssued;
        int max = u->max->maxAtb > 0 ? u->max->maxAtb : 1, atb = u->atb > max ? max : u->atb < 0 ? 0 : u->atb;
        bar(&g_ot[2], x + 38, y + 25, 110 * atb / max, 4, ready ? 0xF0F0F0 : 0xC09040);
        bar(&g_ot[3], x + 38, y + 25, 110, 4, 0x302018);
        if (u->flags & 0x40000000u) {
            bar(&g_ot[2], x + 38, y + 31, 110 * u->trance / 255, 3, 0x9060F0);
            bar(&g_ot[3], x + 38, y + 31, 110, 3, 0x301830);
        }
        if (g_btlUi->cmdUnit == u) bar(&g_ot[2], r_cards.x + 1, y, 2, 34, 0x40E0F8); /* (the member choosing) */
    }
    win_drawBox(&r_cards, 4, 2);
}

static void draw_commands(void) {
    static const u8 defend[] = "DEFEND", change[] = "CHANGE", command[] = "COMMAND";
    cmd_shown = g_btlUi->menuState == BTLUI_MENU_CMD && g_btlUi->cmdUnit != NULL;
    if (!cmd_shown) return;
    ui_drawLabel(8, 39, 2, (s32)command, 0);
    for (int i = 0; i < 4; i++) {
        RECT *r = &r_cmd[i];
        r->x = 6; r->y = 44 + i * 35; r->w = 144; r->h = 31;
        if (!g_btlCmdMenu.cmdIds[i]) continue;
        u8 *t = g_btlCmdNames + g_btlCmdTable[g_btlCmdMenu.cmdIds[i]].nameOfs;
        text_drawBlock(r->x + 14, r->y + 9, 2, t, g_btlCmdMenu.enabled[i] ? 0 : 2);
        if (g_btlCmdMenu.cursor == i) bar(&g_ot[2], r->x + 4, r->y + 6, 3, r->h - 12, 0x40E0F8);
        win_drawBox(r, 4, 2);
    }
    int w = ui_measureLabel((u8 *)defend);
    ui_drawLabel(r_defend.x + (r_defend.w - w) / 2, r_defend.y + 11, 2, (s32)defend, 0);
    win_drawBox(&r_defend, 4, 2);
    w = ui_measureLabel((u8 *)change);
    ui_drawLabel(r_change.x + (r_change.w - w) / 2, r_change.y + 11, 2, (s32)change, 0);
    win_drawBox(&r_change, 4, 2);
}

static RECT r_list[5], r_desc = {6, 174, 232, 46}, r_back = {242, 174, 74, 46};
static int list_shown, list_first, list_n;
s32 o0b_btlui_getListHelpText(void);
s32 o0b_btlui_getAbilityState(s32);
void *o0b_btl_getUnitName(BtlUnit *);

static int draw_list(void) {
    static const u8 back[] = "BACK", hp[] = "HP", mp[] = "MP";
    list_shown = 0;
    Window *w = g_btlUi->windows[2];
    if (!w || w->state != 3 || !g_btluiList.cmd) return 0;
    MenuList *L = &w->list;
    if (L->cols != 1 || L->count <= 0) return 0; /* (only single-column lists: battle's are) */
    int count = L->count, cur = L->top + L->cursor;
    int vis = 5, first = cur - 2;
    if (first > count - vis) first = count - vis;
    if (first < 0) first = 0;
    MenuView v;
    menuList_getView(L, &v);
    v.y = 0;
    RECT saved = w->cur;
    w->cur.h = w->h; /* (the row functions offset by the open animation's h - cur.h: none) */
    list_n = 0;
    for (int i = 0; i < vis && first + i < count; i++) {
        RECT *r = &r_list[i];
        r->x = 6; r->y = 40 + i * 26; r->w = 202; r->h = 24;
        list_n = i + 1;
        /* (an ability the member hasn't: no row, as the game draws none) */
        if (g_btluiList.cmd->kind == 1 && !o0b_btlui_getAbilityState(g_btlCmdAbilities[g_btluiList.cmd->first + first + i])) {
            r->w = 0;
            continue;
        }
        w->cur.x = r->x + 4;
        w->cur.y = r->y + 5;
        g_btluiListRowFns[g_btluiList.cmd->kind](0, 0, first + i, &v);
        if (first + i == cur) bar(&g_ot[6], r->x + 2, r->y + 5, 3, r->h - 10, 0x40E0F8);
        win_drawBox(r, 9, 2);
    }
    w->cur = saved;
    list_first = first;
    u8 *help = (u8 *)o0b_btlui_getListHelpText();
    if (help) text_drawBlock(r_desc.x + 8, r_desc.y + 5, 2, help, 0);
    win_drawBox(&r_desc, 4, 2);
    int bw = ui_measureLabel((u8 *)back);
    ui_drawLabel(r_back.x + (r_back.w - bw) / 2, r_back.y + 9, 2, (s32)back, 0);
    win_drawBox(&r_back, 4, 2);
    /* the party, compact */
    RECT rp = {212, 40, 104, 128};
    u8 a[8];
    for (int i = 0; i < 4; i++) {
        BtlUnit *u = card_unit[i] = party_unit(i);
        if (!u || !u->menu) continue;
        int y = rp.y + 3 + i * 31;
        text_drawBlock(rp.x + 6, y, 2, u->menu->chr->name, u->hp ? 0 : 2);
        ui_drawLabel(rp.x + 6, y + 14, 2, (s32)hp, 0);
        text_formatNumber(u->menu->gauges[0].shown, a, 4, 1);
        ui_drawSmallDigits(rp.x + 20, y + 15, 2, a, u->max->maxHp / 6 >= u->hp ? 2 : 0);
        ui_drawLabel(rp.x + 56, y + 14, 2, (s32)mp, 0);
        text_formatNumber(u->menu->gauges[1].shown, a, 3, 1);
        ui_drawSmallDigits(rp.x + 70, y + 15, 2, a, u->max->maxMp / 6 >= u->mp ? 2 : 0);
    }
    win_drawBox(&rp, 4, 2);
    list_shown = 1;
    return 1;
}

static RECT r_tgt[12], r_all = {6, 196, 100, 26}, r_tback = {242, 196, 74, 26};
static BtlUnit *tgt_unit[12];
static int n_tgt, tgt_shown;
static BtlUnit *tgt_goal; /* the unit a tap asked for */
static int tgt_steps;

static int draw_targets(void) {
    static const u8 enemy[] = "ENEMY", party[] = "PARTY", all[] = "ALL", one[] = "ONE", back[] = "BACK";
    tgt_shown = 0;
    if (!g_btlTarget.windowOpen || g_btlTarget.listKind) return 0;
    int ne = 0, np = 0;
    n_tgt = 0;
    ui_drawLabel(8, 37, 2, (s32)enemy, 0);
    ui_drawLabel(214, 37, 2, (s32)party, 0);
    for (BtlUnit *u = g_gameState->gameData->ctx.battle->unitList; u && n_tgt < 12; u = u->next) {
        if (!(u->flags >> 28 & 1)) continue;
        int is_party = u->flags & 1;
        RECT *r = &r_tgt[n_tgt];
        if (is_party) { if (np == 5) continue; r->x = 212; r->y = 42 + np * 30; r->w = 104; r->h = 26; np++; }
        else { if (ne == 5) continue; r->x = 6; r->y = 42 + ne * 30; r->w = 200; r->h = 26; ne++; }
        tgt_unit[n_tgt++] = u;
        int ko = u->hp == 0;
        text_drawBlock(r->x + 12, r->y + 7, 2, (u8 *)o0b_btl_getUnitName(u), ko ? 2 : is_party && u->max->maxHp / 6 >= u->hp ? 4 : 0);
        if (u->flags >> 29 & 1) bar(&g_ot[2], r->x + 3, r->y + 6, 3, r->h - 12, 0x40E0F8);
        win_drawBox(r, 4, 2);
    }
    if (g_btlTarget.canToggleAll) {
        const u8 *l = g_btlTarget.targetAll ? one : all;
        int w = ui_measureLabel((u8 *)l);
        ui_drawLabel(r_all.x + (r_all.w - w) / 2, r_all.y + 9, 2, (s32)l, 0);
        win_drawBox(&r_all, 4, 2);
    }
    int w = ui_measureLabel((u8 *)back);
    ui_drawLabel(r_tback.x + (r_tback.w - w) / 2, r_tback.y + 9, 2, (s32)back, 0);
    win_drawBox(&r_tback, 4, 2);
    tgt_shown = 1;
    return 1;
}

/* each idle frame (port/3ds/touch.c): the next press toward the unit a tap asked for, 0 when there or none */
unsigned port_battle_drive(void) {
    if (!tgt_goal || !tgt_shown || g_btlUi->menuState != BTLUI_MENU_TARGET) { tgt_goal = NULL; return 0; }
    if (tgt_goal->flags >> 29 & 1 && !g_btlTarget.targetAll) { tgt_goal = NULL; return 0; } /* (there: a tap confirms) */
    if (++tgt_steps > 24) { tgt_goal = NULL; return 0; }
    if (g_btlTarget.targetAll) return 0x0800; /* (R1: back to one target) */
    int want_party = tgt_goal->flags & 1;
    if (want_party != g_btlTarget.side && g_btlTarget.canSwitchSide) return want_party ? PAD_RIGHT_ : PAD_LEFT_;
    return PAD_DOWN_;
}

/* field_hud.c's frame: whether the battle screen is the port's this frame (then it draws, into g_ot / g_primPtr) */
int port_battle_hud_draw(void) {
    port_battle_hud_on = 0;
    if (!ram_ok(g_gameState->gameData->ctx.battle, 16) || !ram_ok(g_btlUi, sizeof *g_btlUi) || !g_btlUi->partyCount) return 0;
    int st = g_btlUi->menuState;
    cmd_shown = list_shown = tgt_shown = 0;
    port_battle_hud_targets = 0;
    if (st == BTLUI_MENU_LIST) {
        if (!draw_list()) return 0;
        port_battle_hud_on = 1;
        return 1;
    }
    port_battle_hud_targets = 0;
    if (st == BTLUI_MENU_TARGET) {
        if (!TARGET_PANEL || !draw_targets()) return 0;
        port_battle_hud_on = port_battle_hud_targets = 1;
        return 1;
    }
    if (st == BTLUI_MENU_CLOSING && g_btlUi->windows[2] && g_btlUi->windows[2]->state) return 0; /* (a list opening) */
    draw_commands();
    draw_cards();
    port_battle_hud_on = 1;
    return 1;
}

/* a tap on the battle screen: the presses it stands for (field_hud.c's port_hud_tap) */
int port_battle_tap(int x, int y, unsigned *seq, int max) {
    if (!port_battle_hud_on || max < 10) return 0;
    if (tgt_shown) {
        for (int i = 0; i < n_tgt; i++) {
            if (!inside(&r_tgt[i], x, y)) continue;
            BtlUnit *u = tgt_unit[i];
            if (u->flags >> 29 & 1 && !g_btlTarget.targetAll) { seq[0] = PAD_CROSS_; return 1; } /* (targeted: confirm) */
            tgt_goal = u;
            tgt_steps = 0;
            return -1; /* (handled: port_battle_drive steers there) */
        }
        if (g_btlTarget.canToggleAll && inside(&r_all, x, y)) { seq[0] = 0x0800; return 1; } /* R1 */
        if (inside(&r_tback, x, y)) { seq[0] = 0x2000; return 1; } /* circle */
        return 0;
    }
    if (list_shown) {
        Window *w = g_btlUi->windows[2];
        int cur = w ? w->list.top + w->list.cursor : 0;
        for (int i = 0; i < list_n; i++) {
            if (!inside(&r_list[i], x, y)) continue;
            int n = 0, d = list_first + i - cur;
            while (d && n < max - 1) seq[n++] = d > 0 ? PAD_DOWN_ : PAD_UP_, d += d > 0 ? -1 : 1;
            seq[n++] = PAD_CROSS_;
            return n;
        }
        if (inside(&r_back, x, y)) { seq[0] = 0x2000; return 1; } /* circle */
        return 0;
    }
    if (cmd_shown) {
        for (int i = 0; i < 4; i++) {
            if (!g_btlCmdMenu.cmdIds[i] || !inside(&r_cmd[i], x, y)) continue;
            int n = 0, c = g_btlCmdMenu.cursor;
            /* the cursor skips empty rows: count the rows it stops at on the way */
            for (int j = c + 1; j <= i; j++) if (g_btlCmdMenu.cmdIds[j]) seq[n++] = PAD_DOWN_;
            for (int j = c - 1; j >= i; j--) if (g_btlCmdMenu.cmdIds[j]) seq[n++] = PAD_UP_;
            seq[n++] = PAD_CROSS_;
            return n;
        }
        if (inside(&r_defend, x, y) || inside(&r_change, x, y)) {
            u32 side = inside(&r_defend, x, y) ? PAD_RIGHT_ : PAD_LEFT_; /* (held: the command slides out, then cross) */
            seq[0] = side | HOLD(8);
            seq[1] = side | PAD_CROSS_;
            return 2;
        }
    }
    for (int i = 0; i < 4; i++) {
        BtlUnit *u = card_unit[i];
        RECT r = {r_cards.x, (s16)(r_cards.y + 4 + i * 43), r_cards.w, 40};
        if (u && inside(&r, x, y) && u->inMenuQueue && !u->cmdIssued && g_btlUi->cmdUnit != u && cmd_shown) {
            seq[0] = PAD_TRIANGLE_;
            return 1;
        }
    }
    return 0;
}
