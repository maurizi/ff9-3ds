/* The top screen while a menu is up (docs/ui-design.md): what the bottom screen's menu points at, which the PS1 shows
 * only in part or not at all. Drawn with the game's window and text code into field_hud.c's top-screen list.
 *
 *   Item, Shop      the highlighted item: icon, name, its whole description; for equipment, each party member who can
 *                   wear it, with the stat it changes before and after (the game's own chr_calcEquipStats)
 *   Equip           the member's stats with the candidate in the slot, before and after
 *   Ability         the highlighted ability's AP, learned or not, and which of the member's equipment teaches it
 *   Save            the highlighted file: its party (faces, names, levels), location, play time and gil
 *
 * The menus' state is read from their overlays' statics (by name: in the port each overlay's data is linked on its own,
 * not at the PS1's shared load address), at the offsets the decomp session found (ff9-decomp
 * docs/r32/3ds-requests-2.md). The game's logic pauses as on the PS1; nothing here changes the menu. */
#define M2CTX
#include "ff9.h"

void ctr_hud_image(int slot, u32 key, const u16 *clut, const u8 *pix, int w, int h);
void ctr_hud_draw_top(int slot, int x, int y, int w, int h, int dw, int dh);
const u8 *port_portrait(int index);
const u8 *port_portrait_face(int index);
s32 chr_getAp(s32, s32);
s32 chr_getApNeeded(s32, s32);
void ui_drawItemIcon(s32, s32, s32, s32, s32);

extern unsigned char psx_ram[];
static int ram_ok(const void *p, unsigned size) { /* (PS1 RAM and the overlays' data after it) */
    const unsigned char *b = (const unsigned char *)p;
    return b >= psx_ram + 0x10000 && b + size <= psx_ram + 0x1000000;
}
/* the overlays' statics (their layouts: the decomp session's offsets) */
extern u8 s_menuItem[], s_menuItemHelp[], s_menuEquip[], s_menuEquipItemHelp[], s_menuAbility[], s_menuSave[];
extern u8 s_shop[], s_shopBuy[], s_shopSell[], s_shopSynth[], s_shopItemHelp[];

static void face(int slot, int portrait, int x, int y, int size) {
    const u8 *e = port_portrait(portrait), *f = port_portrait_face(portrait);
    if (!e || !f) return;
    ctr_hud_image(slot, 0x400u | (u32)portrait, (const u16 *)e, f, 32, 32);
    ctr_hud_draw_top(slot, x, y, 32, 32, size, size);
}

static int chr_portrait(Character *c) {
    return g_portraitByKind[g_gameState->gameData->characters[c->info.b.id].info.b.kind];
}

/* the member's stats with equip[slot] = item (0xFF: removed; slot < 0: as worn) */
static EquipStatsOut stats_with(Character *c, int slot, int item) {
    EquipStatsIn in;
    EquipStatsOut out;
    in.base = *(Bytes8 *)&c->baseMaxHp;
    in.hp = c->hp;
    in.mp = c->mp;
    in.support = *(Words2 *)c->supportAbilities;
    in.equip = *(Bytes5 *)c->equip;
    if (slot >= 0 && slot < 5) ((u8 *)&in.equip)[slot] = (u8)item;
    chr_calcEquipStats(&in, &out);
    return out;
}

/* a small right arrow (packets: a bar and a head) */
static void arrow(int x, int y) {
    u32 *p = (u32 *)g_primPtr;
    p[0] = 3u << 24; p[1] = 0x60A0A0A0u; p[2] = (u32)x | (u32)(y + 3) << 16; p[3] = 6u | 1u << 16; /* (a 6x1 tile) */
    addPrim(&g_ot[2], p);
    p += 4;
    p[0] = 4u << 24; p[1] = 0x20A0A0A0u; /* (the head: a triangle) */
    p[2] = (u32)(x + 6) | (u32)y << 16; p[3] = (u32)(x + 10) | (u32)(y + 3) << 16; p[4] = (u32)(x + 6) | (u32)(y + 7) << 16;
    addPrim(&g_ot[2], p);
    g_primPtr = (u8 *)(p + 5);
}

/* "label  before -> after", after coloured by the change (the menus' green and red: digit styles 1 and 4) */
static void compare(int x, int y, const char *label, int before, int after) {
    u8 a[8];
    int w = before > 999 || after > 999 ? 4 : 3;
    ui_drawLabel(x, y, 2, (s32)label, 0);
    text_formatNumber(before, a, w, 1);
    ui_drawDigits(x + 62 - (w - 3) * 8, y - 1, 2, a, 0);
    arrow(x + 88, y);
    text_formatNumber(after, a, w, 1);
    ui_drawDigits(x + 102, y - 1, 2, a, after > before ? 1 : after < before ? 4 : 0);
}

/* the stat an item of this slot is compared by */
static const char *slot_stat(int slot, EquipStatsOut *o, int *v) {
    switch (slot) {
    case 0: *v = o->defs[0]; return "ATTACK";
    case 3: *v = o->defs[1]; return "DEFENSE";
    case 2: *v = o->defs[1]; return "DEFENSE";
    case 1: *v = o->defs[3]; return "MAGIC DEF";
    default: *v = o->stats[1]; return "STRENGTH";
    }
}

/* an item: icon, name, description; for equipment, who can wear it and what it changes */
static void item_view(int id, const u8 *help_base) {
    static RECT box = {8, 8, 384, 224};
    if (id < 0 || id >= 0xFF) return;
    ItemInfo *it = &g_itemTable[id];
    ui_drawItemIcon(22, 22, 2, id, 1);
    text_drawBlock(42, 22, 2, g_itemNames + it->name, 0);
    if (help_base && ram_ok(help_base + it->help, 2)) text_drawBlock(22, 46, 2, (u8 *)help_base + it->help, 0);
    int slot = -1;
    for (int s = 0; s < 5; s++)
        if (it->slots & g_equipSlotMasks[s]) slot = s;
    if (slot >= 0) {
        GameData *g = g_gameState->gameData;
        static const u8 who[] = "WHO CAN EQUIP";
        ui_drawLabel(22, 104, 2, (s32)who, 0);
        int row = 0;
        for (int i = 0; i < 4; i++) {
            Character *c = g->party[i];
            if (!ram_ok(c, sizeof *c)) continue;
            int ok = (it->equipMask & (0x800 >> chr_getDefaultSet(c))) != 0;
            int x = 22 + (row & 1) * 184, y = 118 + (row >> 1) * 52;
            row++;
            face(i, chr_portrait(c), x, y, 40);
            text_drawBlock(x + 46, y + 2, 2, c->name, ok ? 0 : 2);
            if (!ok) continue;
            EquipStatsOut now = stats_with(c, -1, 0), then = stats_with(c, slot, id);
            int b, a;
            const char *label = slot_stat(slot, &now, &b);
            slot_stat(slot, &then, &a);
            compare(x + 46, y + 22, label, b, a);
        }
    }
    win_drawBox(&box, 5, 2);
}

/* the Equip screen: the member's stats, before and after the candidate */
static void equip_view(const u8 *st) {
    static RECT box = {8, 8, 384, 224};
    GameData *g = g_gameState->gameData;
    int member = st[3], slot = st[2], cand = -1;
    if (member > 3 || !ram_ok(g->party[member], sizeof(Character))) return;
    Character *c = g->party[member];
    if (st[1] == 3) { /* (the candidate list open) */
        MenuList *L = (MenuList *)(st + 4);
        if (L->rows) cand = st[0x3C + (L->top + L->cursor) * 2];
    }
    face(0, chr_portrait(c), 22, 20, 48);
    text_drawBlock(80, 24, 2, c->name, 0);
    if (cand >= 0 && cand != 0xFF) {
        ui_drawItemIcon(80, 46, 2, cand, 1);
        text_drawBlock(100, 46, 2, g_itemNames + g_itemTable[cand].name, 0);
    }
    EquipStatsOut b = stats_with(c, -1, 0), a = cand >= 0 ? stats_with(c, slot, cand) : b;
    compare(28, 92, "SPEED", b.stats[0], a.stats[0]);
    compare(28, 110, "STRENGTH", b.stats[1], a.stats[1]);
    compare(28, 128, "MAGIC", b.stats[2], a.stats[2]);
    compare(28, 146, "SPIRIT", b.stats[3], a.stats[3]);
    compare(212, 92, "ATTACK", b.defs[0], a.defs[0]);
    compare(212, 110, "DEFENSE", b.defs[1], a.defs[1]);
    compare(212, 128, "EVADE", b.defs[2], a.defs[2]);
    compare(212, 146, "MAGIC DEF", b.defs[3], a.defs[3]);
    compare(212, 164, "MAGIC EVA", b.defs[4], a.defs[4]);
    compare(28, 182, "MAX HP", b.maxHp, a.maxHp);
    compare(28, 200, "MAX MP", b.maxMp, a.maxMp);
    win_drawBox(&box, 5, 2);
}

/* the Ability screen: the highlighted ability's AP and what teaches it */
static void ability_view(const u8 *st) {
    static RECT box = {8, 8, 384, 224};
    static const u8 ap[] = "AP", learned[] = "LEARNED", taught[] = "TAUGHT BY";
    GameData *g = g_gameState->gameData;
    int member = g_menuScreen.member;
    if (st[1] != 2 || member > 3 || !ram_ok(g->party[member], sizeof(Character))) return;
    Character *c = g->party[member];
    int tab = st[0];
    MenuList *L = (MenuList *)(st + (tab ? 0x18 : 8));
    int n = (L->top + L->cursor) * L->cols + L->col, id = tab ? st[0x728 + n] : st[0x668 + n * 4];
    if (!id) return;
    int chr = (int)(*(u32 *)((u8 *)c + 0x34) & 0xF);
    face(0, chr_portrait(c), 22, 20, 48);
    text_drawBlock(80, 24, 2, c->name, 0);
    s32 have = chr_getAp(chr, id), need = chr_getApNeeded(chr, id);
    u8 a[8];
    if (have >= 0 && need > 0) {
        ui_drawLabel(80, 50, 2, (s32)ap, 0);
        text_formatNumber(have, a, 3, 1);
        ui_drawDigits(100, 49, 2, a, 0);
        ui_drawLabel(130, 50, 2, (s32)"/", 0);
        text_formatNumber(need, a, 3, 1);
        ui_drawDigits(140, 49, 2, a, 0);
        if (have >= need) ui_drawLabel(180, 50, 2, (s32)learned, 1);
    }
    ui_drawLabel(28, 92, 2, (s32)taught, 0);
    int y = 108;
    for (int s = 0; s < 5; s++) {
        int it = c->equip[s];
        if (it == 0xFF) continue;
        for (int k = 0; k < 3; k++)
            if (g_itemTable[it].abilities[k] == id) {
                ui_drawItemIcon(28, y, 2, it, 1);
                text_drawBlock(48, y, 2, g_itemNames + g_itemTable[it].name, 0);
                y += 20;
            }
    }
    win_drawBox(&box, 5, 2);
}

/* the save screen: the highlighted file's summary */
static void save_view(void) {
    static RECT box = {8, 8, 384, 224};
    static const u8 lv[] = "LV";
    const u8 *st = s_menuSave;
    if (st[1] != 4) return; /* (choosing a block) */
    MenuList *L = (MenuList *)(st + 0x24);
    int row = L->top + L->cursor;
    if (row < 0 || row > 14) return;
    const u8 *f = st + 0x34 + row * 0x98;
    if (f[0x1A] != 4) return; /* (not an FF9 save) */
    u8 a[16];
    text_drawBlock(22, 20, 2, (u8 *)f + 0x28, 0); /* location */
    s32 t = *(s32 *)(f + 0x50) / 60;
    ui_drawSprite(240, 22, 2, 0xB1, 0);
    a[0] = '0' + t / 36000 % 10; a[1] = '0' + t / 3600 % 10; a[2] = ':';
    a[3] = '0' + t / 600 % 6; a[4] = '0' + t / 60 % 10; a[5] = ':';
    a[6] = '0' + t / 10 % 6; a[7] = '0' + t % 10; a[8] = 0;
    ui_drawSmallDigits(253, 23, 2, a, 0);
    ui_drawSprite(240, 38, 2, 0xB2, 0);
    text_formatNumber(*(s32 *)(f + 0x54), a, 7, 1);
    ui_drawSmallDigits(253, 39, 2, a, 0);
    for (int i = 0; i < 4; i++) {
        const u8 *m = f + 0x58 + i * 0x10;
        if (!m[0]) continue;
        int x = 22 + (i & 1) * 184, y = 70 + (i >> 1) * 76;
        face(i, m[1], x, y, 56);
        text_drawBlock(x + 64, y + 6, 2, (u8 *)m + 2, 0);
        ui_drawLabel(x + 64, y + 28, 2, (s32)lv, 0);
        text_formatNumber(m[0xC], a, 2, 1);
        ui_drawDigits(x + 82, y + 27, 2, a, 0);
    }
    win_drawBox(&box, 5, 2);
}

/* field_hud.c's top-screen frame while a menu is up: 1 if drawn */
int port_menu_top_draw(void) {
    GameData *g = g_gameState->gameData;
    switch (g_menu.type) {
    case 0: /* the main menu: its screens */
        if (!g_menuScreen.active) return 0;
        switch (g_menuScreen.screen) {
        case 1: { /* Item */
            const u8 *st = s_menuItem;
            if (st[1] != 2 || st[0] == 2) return 0; /* (the list, not Key items) */
            MenuList *L = (MenuList *)(st + 0xC);
            int slot = (L->top + L->cursor) * 2 + L->col;
            if (slot < 0 || slot > 255 || !g->items.slots[slot].count) return 0;
            item_view(g->items.slots[slot].id, s_menuItemHelp);
            return 1;
        }
        case 2: ability_view(s_menuAbility); return 1;
        case 3: equip_view(s_menuEquip); return 1;
        default: return 0;
        }
    case 2: { /* a shop */
        const u8 *sh = s_shop;
        const u8 *help = s_shopItemHelp;
        int id = -1;
        if (!sh[3]) return 0; /* (the shop's own menu, no mode running) */
        if (sh[0] == 2) { /* synthesis: inline 6-byte recipes, the product at +4 */
            const u8 *sy = s_shopSynth;
            MenuList *L = (MenuList *)(sy + 0x28);
            id = sy[0x3C + (L->top + L->cursor) * 6 + 4];
        } else if (sh[1] == 0) { /* buy */
            const u8 *b = s_shopBuy;
            MenuList *L = (MenuList *)(b + 0x28);
            const u8 *items = *(const u8 *const *)(b + 0x3C);
            if (ram_ok(items, 1)) id = items[L->top + L->cursor];
        } else { /* sell */
            const u8 *se = s_shopSell;
            MenuList *L = (MenuList *)(se + 4);
            int slot = (L->top + L->cursor) * 2 + L->col;
            if (slot >= 0 && slot < 256 && g->items.slots[slot].count) id = g->items.slots[slot].id;
        }
        if (id < 0) return 0;
        item_view(id, help);
        return 1;
    }
    case 4: save_view(); return 1;
    default: return 0;
    }
}
