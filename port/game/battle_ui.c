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
 * (with "Target Window: Always" forced for the battle, so targets are a list there too). While targeting (the game's
 * target window), the targets: a large button per enemy (left) and party member (right), marked when targeted,
 * ALL / ONE and BACK.
 * Touch acts directly, never by simulated presses: a tap is queued, and at the game's next input step for that menu
 * (its btlui_cmdInput / btlui_listInput / btlui_targetInput, wrapped: config/renames.txt) the port puts the game's
 * cursor on the tapped entry (or slides the command window out for DEFEND / CHANGE, or targets the tapped unit) and
 * runs the game's own confirm or cancel, as the button would have. A tap on a ready member's card passes the turn on
 * (the game's btlui_passTurn, the PS1's triangle).
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

/* PAUSE (Start: the game's pause request set directly) and RUN (hold: L1 + R1 held, as btlui_pollEscape counts it,
 * done here after the game's frame while the stylus stays on it), above the party cards */
static RECT r_pause = {156, 6, 76, 30}, r_run = {240, 6, 76, 30};
static int sys_shown, running;
int ctr_touch_held(int *x, int *y);
extern int ctr_ui_oy;
void o0b_btl_tryEscape(u8);

static int ram_ok(const void *p, unsigned size);
void port_battle_ui_frame(void) {
    static int forced, had;
    if (!g_gameState) return;
    int battle = g_gameState->mode == 2;
    { /* the top screen's snapshot for the rewards: the battle's last frame before its exit fade (BTL_PHASE_EXIT
       * subphase 2 on: fade_out started), kept through the rewards screen (mode 8) */
        extern int ctr_freeze_held;
        Battle *b = battle && g_gameState->gameData ? g_gameState->gameData->ctx.battle : NULL;
        if (b && ram_ok(b, sizeof *b) && b->phase == BTL_PHASE_EXIT && b->subPhase >= 2) ctr_freeze_held = 1;
        else if (g_gameState->mode != 8 && !battle) ctr_freeze_held = 0;
        else if (battle && !(b && ram_ok(b, sizeof *b) && b->phase == BTL_PHASE_EXIT)) ctr_freeze_held = 0;
    }
    { /* subPhase moving in combat (a camera scene's cue, decomp-requests F37: the end check took it for a defeat) */
        static int lsub;
        Battle *b = battle && g_gameState->gameData ? g_gameState->gameData->ctx.battle : NULL;
        if (b && ram_ok(b, sizeof *b)) {
            if (b->phase == BTL_PHASE_COMBAT && b->subPhase != lsub)
                port_log("battle: subPhase %d in combat (cmd %d)\n", b->subPhase,
                         b->curCmd && ram_ok(b->curCmd, sizeof *b->curCmd) ? b->curCmd->cmdNo : -1);
            lsub = b->subPhase;
        }
    }
    { /* a battle won but not leaving (btl_victory waits for the effects, the command queue and every enemy hidden):
       * what it waits on, in the log, every 10 s (a hardware report: the Sealion's last hit, Zidane's trance ending,
       * then the battle stayed) */
        static int ended;
        Battle *b = battle && g_gameState->gameData ? g_gameState->gameData->ctx.battle : NULL;
        if (b && ram_ok(b, sizeof *b) && b->phase == BTL_PHASE_END) {
            if (++ended % 600 == 0) {
                port_log("battle end waiting %d s: sub %d fx %d cur %d/%d phase %d\n", ended / 60, b->subPhase,
                         ram_ok(g_btlfxEnv, sizeof *g_btlfxEnv) && ram_ok(g_btlfxEnv->state, 4) ? g_btlfxEnv->state->phase : -99,
                         b->curCmd && ram_ok(b->curCmd, sizeof *b->curCmd) ? b->curCmd->cmdNo : -1,
                         b->curCmd && ram_ok(b->curCmd, sizeof *b->curCmd) ? b->curCmd->subNo : -1, b->cmdPhase);
                int guard = 0;
                for (BtlCmd *c = b->cmdQueue.next; c && ram_ok(c, sizeof *c) && guard++ < 16; c = c->next)
                    port_log("  queued cmd %d/%d caster %08x\n", c->cmdNo, c->subNo,
                             c->caster && ram_ok(c->caster, sizeof *c->caster) ? (unsigned)c->caster->flags : 0);
                guard = 0;
                for (BtlUnit *u = b->unitList; u && ram_ok(u, sizeof *u) && guard++ < 8; u = u->next)
                    port_log("  unit flags %08x status %08x hp %d\n", (unsigned)u->flags, (unsigned)u->status, u->hp);
            }
        } else ended = 0;
    }
    { /* RUN held: the escape poll's own steps (escapeHeld, the hold count, btl_tryEscape) */
        int tx, ty;
        running = 0;
        if (battle && sys_shown && g_btluiState.atbOn && ctr_touch_held(&tx, &ty) &&
            tx >= r_run.x && tx < r_run.x + r_run.w && ty - ctr_ui_oy >= r_run.y && ty - ctr_ui_oy < r_run.y + r_run.h) {
            volatile Unk80104E68 *st = &g_btluiState;
            running = 1;
            g_gameState->gameData->ctx.battle->escapeHeld = 1;
            if (st->escapeFrames++ >= 0x3D) {
                st->escapeFrames = 0;
                o0b_btl_tryEscape(1);
            } else o0b_btl_tryEscape(0);
        }
    }
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
static RECT r_defend = {4, 188, 72, 30}, r_change = {80, 188, 72, 30};
static RECT r_cards = {156, 44, 160, 176}; /* (rows of 43: a 32x32 face 5 px in, the text 6 px after it) */
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
    int y = r_cards.y + 5;
    for (int i = 0; i < 4; i++, y += 43) {
        BtlUnit *u = card_unit[i] = party_unit(i);
        if (!u || !ram_ok(u->menu, 16) || !ram_ok(u->menu->chr, sizeof(Character)) || !ram_ok(u->max, 8)) continue;
        Character *c = u->menu->chr;
        int x = r_cards.x + 6;
        const u8 *e = port_portrait(g_portraitByKind[c->info.b.kind]);
        if (e) {
            ctr_hud_image(4 + i, 0x200u | (u32)g_portraitByKind[c->info.b.kind], (const u16 *)e,
                          port_portrait_face(g_portraitByKind[c->info.b.kind]), 32, 32);
            ctr_hud_draw(4 + i, x, y + 1, 32, 32);
        }
        int dead = u->hp == 0;
        text_drawBlock(x + 40, y, 2, c->name, dead ? 2 : 0);
        { /* its statuses: the game's icons (the battle status list's table), up to four after the name */
            u32 st = u->status | u->autoStatus;
            int ix = x + 104, n = 0;
            for (int k = 0; k < 16 && n < 4; k++)
                if (st & g_btluiStatusIcons[k].status) {
                    ui_drawSprite(ix, y + 1, 2, g_btluiStatusIcons[k].icon, 0);
                    ix += 12;
                    n++;
                }
        }
        int low = dead ? 2 : (u->max->maxHp / 6 >= u->hp) ? 2 : 0;
        ui_drawLabel(x + 40, y + 14, 2, (s32)hp, 0);
        text_formatNumber(u->menu->gauges[0].shown, a, 4, 1);
        ui_drawSmallDigits(x + 54, y + 15, 2, a, low);
        ui_drawLabel(x + 86, y + 14, 2, (s32)mp, 0);
        text_formatNumber(u->menu->gauges[1].shown, a, 3, 1);
        ui_drawSmallDigits(x + 100, y + 15, 2, a, u->max->maxMp / 6 >= u->mp ? 2 : 0);
        /* the ATB bar: white when the member is ready, the trance bar under it */
        int ready = u->inMenuQueue && !u->cmdIssued;
        int max = u->max->maxAtb > 0 ? u->max->maxAtb : 1, atb = u->atb > max ? max : u->atb < 0 ? 0 : u->atb;
        bar(&g_ot[2], x + 40, y + 25, 102 * atb / max, 4, ready ? 0xF0F0F0 : 0xC09040);
        bar(&g_ot[3], x + 40, y + 25, 102, 4, 0x302018);
        if (u->flags & 0x40000000u) {
            bar(&g_ot[2], x + 40, y + 31, 102 * u->trance / 255, 3, 0x9060F0);
            bar(&g_ot[3], x + 40, y + 31, 102, 3, 0x301830);
        }
        if (g_btlUi->cmdUnit == u) bar(&g_ot[2], r_cards.x + 2, y + 1, 2, 32, 0x40E0F8); /* (the member choosing) */
    }
    win_drawBox(&r_cards, 4, 2);
}

static void draw_commands(void) {
    static const u8 defend[] = "DEFEND", change[] = "CHANGE", command[] = "COMMAND";
    cmd_shown = g_btlUi->menuState == BTLUI_MENU_CMD && g_btlUi->cmdUnit != NULL;
    if (!cmd_shown) return;
    ui_drawLabel(10, 41, 2, (s32)command, 0);
    for (int i = 0; i < 4; i++) {
        RECT *r = &r_cmd[i];
        r->x = 4; r->y = 44 + i * 35; r->w = 148; r->h = 31;
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
static int list_shown, list_n, list_entry[5]; /* (the list entry each row shows) */
s32 o0b_btlui_getListHelpText(void);
s32 o0b_btlui_getAbilityState(s32);
void *o0b_btl_getUnitName(BtlUnit *);

/* An ability list holds every ability of the member's command, the ones they haven't (not learned, not on their gear)
 * as blank rows the PS1's cursor still stops on. Here only the ones they have are rows, and the cursor is kept on
 * them (keep_on_listed): a blank one showed as nothing highlighted and no help. */
static int listed(int i) {
    return g_btluiList.cmd->kind != 1 || o0b_btlui_getAbilityState(g_btlCmdAbilities[g_btluiList.cmd->first + i]);
}
static void list_set_cursor(MenuList *L, int i) { /* (the game's view moves so the entry is in it) */
    if (i < L->top) L->top = (u8)i;
    if (L->visibleRows && i >= L->top + L->visibleRows) L->top = (u8)(i - L->visibleRows + 1);
    L->cursor = (u8)(i - L->top);
}

static int draw_list(void) {
    static const u8 back[] = "BACK", hp[] = "HP", mp[] = "MP";
    list_shown = 0;
    Window *w = g_btlUi->windows[2];
    if (!w || w->state != 3 || !g_btluiList.cmd) return 0;
    MenuList *L = &w->list;
    if (L->cols != 1 || L->count <= 0) return 0; /* (only single-column lists: battle's are) */
    int count = L->count, cur = L->top + L->cursor;
    static int idx[256];
    int n = 0, pos = 0;
    for (int i = 0; i < count && n < 256; i++) {
        if (!listed(i)) continue;
        if (i <= cur) pos = n;
        idx[n++] = i;
    }
    int vis = 5, first = pos - 2;
    if (first > n - vis) first = n - vis;
    if (first < 0) first = 0;
    MenuView v;
    menuList_getView(L, &v);
    v.y = 0;
    RECT saved = w->cur;
    w->cur.h = w->h; /* (the row functions offset by the open animation's h - cur.h: none) */
    list_n = 0;
    for (int i = 0; i < vis && first + i < n; i++) {
        RECT *r = &r_list[i];
        int e = list_entry[i] = idx[first + i];
        r->x = 6; r->y = 40 + i * 26; r->w = 202; r->h = 24;
        list_n = i + 1;
        w->cur.x = r->x + 4;
        w->cur.y = r->y + 5;
        g_btluiListRowFns[g_btluiList.cmd->kind](0, 0, e, &v);
        if (e == cur) bar(&g_ot[6], r->x + 2, r->y + 5, 3, r->h - 10, 0x40E0F8);
        win_drawBox(r, 9, 2);
    }
    w->cur = saved;
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
/* the tap waiting for the game's next input step */
enum { TAP_NONE, TAP_CMD, TAP_SIDE, TAP_PASS, TAP_LIST, TAP_LIST_BACK, TAP_TARGET, TAP_TARGET_ALL, TAP_TARGET_BACK };
static int tap_kind, tap_arg;
static BtlUnit *tap_unit;

static int draw_targets(void) {
    static const u8 enemy[] = "ENEMY", party[] = "PARTY", all[] = "ALL", one[] = "ONE", back[] = "BACK";
    tgt_shown = 0;
    if (!g_btlTarget.windowOpen) return 0;
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
        if (is_party) { /* (its HP: whom to heal, as the game's target window showed) */
            u8 hp[8];
            text_formatNumber(u->hp, hp, 4, 1);
            ui_drawSmallDigits(r->x + r->w - 36, r->y + 10, 2, hp, ko ? 2 : 0);
        }
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

unsigned port_battle_drive(void) { return 0; } /* (port/3ds/touch.c's steering: nothing to steer now) */

/* field_hud.c's frame: whether the battle screen is the port's this frame (then it draws, into g_ot / g_primPtr) */
int port_rewards_live_state(void);
int port_battle_hud_draw(void) {
    port_battle_hud_on = 0;
    if (port_rewards_live_state() >= 2) { /* (the rewards have the bottom screen, then the battle fades: below) */
        cmd_shown = list_shown = tgt_shown = sys_shown = 0; /* (nothing of the battle's to tap meanwhile) */
        return 0;
    }
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
    { /* PAUSE and RUN */
        static const u8 pause[] = "PAUSE", run[] = "HOLD TO RUN";
        sys_shown = g_btluiState.started != 0;
        int w = ui_measureLabel((u8 *)pause);
        ui_drawLabel(r_pause.x + (r_pause.w - w) / 2, r_pause.y + 11, 2, (s32)pause, 0);
        win_drawBox(&r_pause, 4, 2);
        w = ui_measureLabel((u8 *)run);
        ui_drawLabel(r_run.x + (r_run.w - w) / 2, r_run.y + 11, 2, (s32)run, running ? 1 : 0);
        win_drawBox(&r_run, 4, 2);
    }
    port_battle_hud_on = 1;
    return 1;
}

/* a tap on the battle screen (field_hud.c's port_hud_tap): queued for the game's next input step; -1 handled */
int port_battle_tap(int x, int y, unsigned *seq, int max) {
    (void)seq;
    (void)max;
    if (!port_battle_hud_on) return 0;
    tap_kind = TAP_NONE;
    if (tgt_shown) {
        for (int i = 0; i < n_tgt; i++)
            if (inside(&r_tgt[i], x, y)) { tap_kind = TAP_TARGET; tap_unit = tgt_unit[i]; return -1; }
        if (g_btlTarget.canToggleAll && inside(&r_all, x, y)) tap_kind = TAP_TARGET_ALL;
        else if (inside(&r_tback, x, y)) tap_kind = TAP_TARGET_BACK;
        return -1;
    }
    if (list_shown) {
        for (int i = 0; i < list_n; i++)
            if (inside(&r_list[i], x, y)) { tap_kind = TAP_LIST; tap_arg = list_entry[i]; return -1; }
        if (inside(&r_back, x, y)) tap_kind = TAP_LIST_BACK;
        return -1;
    }
    if (sys_shown && inside(&r_pause, x, y)) { /* (the game's pause request: Start's) */
        if (g_btluiState.started && !g_btluiState.pauseReq) g_btluiState.pauseReq = 1;
        return -1;
    }
    if (sys_shown && inside(&r_run, x, y)) return -1; /* (acts while held: port_battle_ui_frame) */
    if (cmd_shown) {
        for (int i = 0; i < 4; i++)
            if (g_btlCmdMenu.cmdIds[i] && inside(&r_cmd[i], x, y)) { tap_kind = TAP_CMD; tap_arg = i; return -1; }
        if (inside(&r_defend, x, y)) { tap_kind = TAP_SIDE; tap_arg = 1; return -1; } /* (Defend: slid right) */
        if (inside(&r_change, x, y)) { tap_kind = TAP_SIDE; tap_arg = 2; return -1; } /* (Change: slid left) */
    }
    for (int i = 0; i < 4; i++) {
        BtlUnit *u = card_unit[i];
        RECT r = {r_cards.x, (s16)(r_cards.y + 4 + i * 43), r_cards.w, 40};
        if (u && inside(&r, x, y) && u->inMenuQueue && !u->cmdIssued && g_btlUi->cmdUnit != u && cmd_shown) {
            tap_kind = TAP_PASS;
            return -1;
        }
    }
    return -1;
}

/* the game's input steps, wrapped: a queued tap acts in the game's place this step */
void o0b_btlui_cmdInput_game(void);
void o0b_btlui_listInput_game(void);
void o0b_btlui_targetInput_game(void);
void o0b_btlui_confirmCmd(void);
void o0b_btlui_passTurn(void);
void o0b_btlui_listConfirm(void);
void o0b_btlui_closeList(void);
void o0b_btlui_saveCursor(void *, s32, s32, s32);
void o0b_btlui_confirmTarget(void);
void o0b_btlui_cancelTarget(void);
void o0b_btlui_toggleTargetAll(void);
void o0b_btlui_clearTargeted(void);

void o0b_btlui_cmdInput(void) {
    int k = tap_kind;
    if (k == TAP_CMD && g_btlCmdMenu.sideCmd.side == 0) {
        tap_kind = TAP_NONE;
        g_btlCmdMenu.cursor = (u8)tap_arg;
        o0b_btlui_confirmCmd();
        return;
    }
    if (k == TAP_SIDE) { /* (as if held out to that side: slid, ready) */
        tap_kind = TAP_NONE;
        g_btlSideCmd.side = (u8)tap_arg;
        g_btlSideCmd.slide = 0;
        g_btlSideCmd.slideFrame = 5;
        g_btlSideCmd.ready = 1;
        o0b_btlui_confirmCmd();
        return;
    }
    if (k == TAP_PASS) {
        tap_kind = TAP_NONE;
        o0b_btlui_passTurn();
        return;
    }
    if (k == TAP_LIST || k == TAP_LIST_BACK || k >= TAP_TARGET) tap_kind = TAP_NONE; /* (stale: another menu's) */
    o0b_btlui_cmdInput_game();
}

/* after the game's own step: a cursor on an ability the member hasn't goes on to the next one they have, the way it
 * moved (down if it didn't: the list just opened), else back the other way */
static void keep_on_listed(MenuList *L, int was) {
    int cur = L->top + L->cursor;
    if (cur < 0 || cur >= L->count || listed(cur)) return;
    int dir = cur < was ? -1 : 1;
    for (int pass = 0; pass < 2; pass++, dir = -dir)
        for (int i = cur + dir; i >= 0 && i < L->count; i += dir)
            if (listed(i)) { list_set_cursor(L, i); return; }
}

void o0b_btlui_listInput(void) {
    Window *w = g_btlUi->windows[2];
    int k = tap_kind;
    if ((k == TAP_LIST || k == TAP_LIST_BACK) && w && w->list.scroll == 0) {
        tap_kind = TAP_NONE;
        if (k == TAP_LIST_BACK) { /* (as circle) */
            snd_playSe(SE_CANCEL);
            o0b_btlui_saveCursor(&g_btluiList.unit->menu->cursor, 1, w->list.top, w->list.cursor);
            if (g_btluiList.doubleCast == 0) o0b_btlui_closeList();
            else g_btluiList.doubleCast--;
            return;
        }
        list_set_cursor(&w->list, tap_arg);
        o0b_btlui_listConfirm();
        return;
    }
    if (k != TAP_NONE && k != TAP_LIST && k != TAP_LIST_BACK) tap_kind = TAP_NONE;
    int was = w ? w->list.top + w->list.cursor : 0;
    o0b_btlui_listInput_game();
    if (w && g_btlUi->windows[2] == w && w->state == 3 && g_btluiList.cmd) keep_on_listed(&w->list, was);
}

void o0b_btlui_targetInput(void) {
    int k = tap_kind;
    if (k >= TAP_TARGET) {
        tap_kind = TAP_NONE;
        if (k == TAP_TARGET_BACK) { snd_playSe(SE_CANCEL); o0b_btlui_cancelTarget(); return; }
        if (k == TAP_TARGET_ALL) { o0b_btlui_toggleTargetAll(); return; }
        BtlUnit *u = tap_unit;
        int side = u->flags & BTL_UNIT_PARTY;
        if (!g_btlTarget.targetAll) { /* (one target: that one, if the command can aim there) */
            if (side != g_btlTarget.side && !g_btlTarget.canSwitchSide) { snd_playSe(SE_BUZZER); return; }
            o0b_btlui_clearTargeted();
            g_btlTarget.side = (u8)side;
            u->flags |= BTL_UNIT_TARGETED;
        } else if (side != g_btlTarget.side) { snd_playSe(SE_BUZZER); return; } /* (all of one side: tap that side) */
        o0b_btlui_confirmTarget();
        return;
    }
    if (k != TAP_NONE) tap_kind = TAP_NONE;
    o0b_btlui_targetInput_game();
}

/* Select help in battle (the game's help window, OT entry 2 of its UI list): the text on the top screen instead, as
 * in menus; render.c leaves the window off the bottom while port_battle_help_on */
int port_battle_help_on;
void port_help_panel(u8 *text);
int port_battle_help_top(void) {
    u8 *text = (u8 *)g_btluiHelp.text; /* (an address held as s32) */
    /* (open with no text, as on an entry without help: still kept off the bottom, where it showed behind the port's
     * panels as an empty HELP window) */
    port_battle_help_on = port_battle_hud_on && ram_ok(g_btlUi, sizeof *g_btlUi) && g_btluiHelp.open;
    if (!port_battle_help_on || !text || !ram_ok(text, 1) || *text == TEXT_END) return 0;
    port_help_panel(text);
    return 1;
}

/* ---- the rewards under the battle's victory pose ----------------------------------------------------------------
 * On the PS1 a won battle fades out and mode 8 (ovl_65) shows the rewards on a black screen. Here the rewards run
 * inside the battle's exit (BTL_PHASE_EXIT, held at a subphase that only updates and draws the units: the victory
 * pose loops, the camera goes on) on the bottom screen, the battle on the top; when they close the battle fades and
 * leaves as before, and mode 8 skips them (rewards_top.c). The party is saved first, as btl_saveParty does before the
 * rewards on the PS1, and not again. Not for game overs, escapes or rewards an event script runs (rewards_top.c:
 * port_rewards_live_eligible): those keep the PS1's path. */
#define EXIT_ARMED 10 /* (BTL_PHASE_EXIT: 1 starts the fade, 2 counts it; other subphases only update the units.
                       * Armed, the victory camera's end (its script's subphase++) makes it 11: the rewards' cue) */
#define EXIT_HOLD 20  /* (the rewards up, or waiting for ovl_65) */
static int live; /* 0 not yet, 1 waiting for ovl_65 and its data, 2 showing, 3 shown (mode 8 skips), -1 not this time */
static int live_wait;
static int orbit_start, orbiting; /* (the victory camera's cue seen; the camera circling the party) */
int port_rewards_live_state(void) { return live; }
void port_rewards_live_consumed(void) { live = 0; }
s32 o0b_btl_update_game(void);
s32 o0b_btl_saveParty_game(Battle *);
int port_rewards_live_eligible(void);
int port_rewards_live_ready(void);
void port_rewards_live_start(void);
int port_rewards_live_step(void);

s32 o0b_btl_saveParty(Battle *battle) {
    if (live == 2 || live == 3) return 1; /* (saved when the rewards began: their level-ups would be undone) */
    return o0b_btl_saveParty_game(battle);
}

s32 o0b_btl_update(void) {
    Battle *b = g_gameState->gameData->ctx.battle;
    if (b->phase == BTL_PHASE_INTRO) live = 0, live_wait = 0;
    if (b->phase == BTL_PHASE_EXIT && live == 0) {
        /* (on entering the exit: subphase 0 while the victory camera plays, 1 at once without one) */
        live = g_gameState->gameData->battleResult == BATTLE_END_VICTORY && port_rewards_live_eligible() ? 1 : -1;
        if (live == 1) b->subPhase = b->subPhase == 1 ? EXIT_HOLD : EXIT_ARMED;
        orbit_start = orbiting = 0;
    }
    s32 r = o0b_btl_update_game();
    /* (after the rewards, the exit's own steps: 1 starts the fade, 2 counts it to the leave. Nothing may move it on
     * from 2, as a victory camera's cue would (subphase++): the battle would stay, faded out, for good) */
    if (b->phase == BTL_PHASE_EXIT && live == 3 && b->subPhase > 2) b->subPhase = 2;
    if (b->phase != BTL_PHASE_EXIT || (live != 1 && live != 2)) return r;
    /* the victory camera's cue (its scene's subphase++, where the PS1 fades): from there the camera circles the party
     * (o0b_btlcam_update below) */
    if (b->subPhase > EXIT_ARMED && b->subPhase != EXIT_HOLD) {
        b->subPhase = EXIT_HOLD;
        orbit_start = 1;
    }
    if (live == 1 && b->subPhase == EXIT_HOLD) {
        if (port_rewards_live_ready()) {
            o0b_btl_saveParty_game(b);
            live = 2;
            port_log("rewards: under the battle (waited %d frames)\n", live_wait);
            port_rewards_live_start();
        } else if (++live_wait > 300) { /* (ovl_65 not preloaded: the PS1's path) */
            port_log("rewards: ovl_65 or its data not preloaded, after the battle as on the PS1\n");
            live = -1;
            b->subPhase = 1;
        }
    } else if (live == 2 && !port_rewards_live_step()) {
        port_log("rewards: closed, the battle leaves\n");
        live = 3;
        fade_cancel(); /* (the rewards' own closing fade, kept off the battle while they ran: covered, it would show
                        * black before the battle's fade out starts from its scene) */
        b->subPhase = 1; /* (the fade out, then the battle leaves) */
    }
    return r;
}

/* the game's fade while the rewards run in the battle: their screen change (EXP to items) is theirs, not the scene's */
void game_updateFade_game(void);
void game_updateFade(void) {
    game_updateFade_game();
    if (live == 2 && g_gameState->mode == GS_MODE_BATTLE) ClearOTagR(fade_getOt(g_gameState->gameData->drawBuf), 1);
}

/* The camera while the rewards are up, and on through the fade after them: from where the victory camera gave its cue it circles the party at that
 * distance and height, slowly (a turn in ORBIT_FRAMES), its aim easing from where the script looked to the party's
 * middle: the victory loops without a cut. */
#define ORBIT_FRAMES (60 * 24)
#define ORBIT_EASE 60
void o0b_btlcam_update_game(void);
void o0b_btlcam_lookAt(MATRIX *, SVECTOR *, SVECTOR *);
void o0b_btlcam_update(void) {
    static s32 cx, cy, cz, r, h, a0, t0;
    static s32 ax0, ay0, az0;
    Battle *b = g_gameState->gameData->ctx.battle;
    if (!((live == 1 || live == 2 || live == 3) && b->phase == BTL_PHASE_EXIT && (orbit_start || orbiting))) {
        orbiting = 0;
        o0b_btlcam_update_game();
        return;
    }
    MATRIX *view = &g_gameState->gameData->viewMatrix;
    if (orbit_start) { /* (the party's middle, and the camera relative to it, as the script left it) */
        s32 n = 0, sx = 0, sy = 0, sz = 0;
        for (BtlUnit *u = b->unitList; u; u = u->next)
            if ((u->flags & BTL_UNIT_PARTY) && u->hp) sx += u->pos[0] >> 16, sy += u->pos[1] >> 16, sz += u->pos[2] >> 16, n++;
        if (!n) { orbit_start = 0; o0b_btlcam_update_game(); return; }
        cx = sx / n, cy = sy / n - 500, cz = sz / n; /* (aimed at the chest: y is down) */
        s32 dx = g_btlCamera.eye.vx - cx, dz = g_btlCamera.eye.vz - cz;
        r = SquareRoot0(dx * dx + dz * dz);
        h = g_btlCamera.eye.vy - cy;
        a0 = ratan2(dx, dz);
        t0 = VSync(-1);
        /* (where the script looked: along the view's z axis, as far as the party's middle) */
        s32 d = SquareRoot0(dx * dx + h * h + dz * dz);
        ax0 = g_btlCamera.eye.vx + view->m[2][0] * d / 4096;
        ay0 = g_btlCamera.eye.vy + view->m[2][1] * d / 4096;
        az0 = g_btlCamera.eye.vz + view->m[2][2] * d / 4096;
        orbit_start = 0;
        orbiting = 1;
    }
    s32 t = VSync(-1) - t0;
    s32 a = a0 + (s32)((long long)t * 4096 / ORBIT_FRAMES);
    SVECTOR eye, at;
    eye.vx = (s16)(cx + r * math_sin(a & 0xFFF) / 4096);
    eye.vy = (s16)(cy + h);
    eye.vz = (s16)(cz + r * math_cos(a & 0xFFF) / 4096);
    s32 e = t >= ORBIT_EASE ? 4096 : t * 4096 / ORBIT_EASE;
    e = e * e / 4096 * (3 * 4096 - 2 * e) / 4096; /* (smoothstep) */
    at.vx = (s16)(ax0 + (cx - ax0) * e / 4096);
    at.vy = (s16)(ay0 + (cy - ay0) * e / 4096);
    at.vz = (s16)(az0 + (cz - az0) * e / 4096);
    g_btlCamera.eye = eye;
    o0b_btlcam_lookAt(view, &eye, &at);
    g_btlCamera.yaw = -ratan2(eye.vx - at.vx, eye.vz - at.vz) - 0x800;
}

/* no pause while the rewards run under the battle (Start there is the rewards', which ignore it, as on the PS1's
 * rewards screen): a pause stops the battle's update and the rewards in it */
u8 o0b_btlui_isPauseRequested_game(void);
u8 o0b_btlui_isPauseRequested(void) {
    if (live == 1 || live == 2) { g_btluiState.pauseReq = 0; return 0; }
    return o0b_btlui_isPauseRequested_game();
}
