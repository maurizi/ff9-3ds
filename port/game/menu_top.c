/* The top screen while a menu is up (docs/ui-design.md): what the bottom screen's menu points at, which the PS1 shows
 * only in part or not at all. Drawn with the game's window and text code into field_hud.c's top-screen list.
 *
 * Only what the bottom screen's (the PS1's) menu doesn't show; otherwise the top keeps the paused world.
 *   Item, Shop      for equipment: each party member, whether they can wear it and what it changes for them (the
 *                   game's own chr_calcEquipStats), which the PS1 shows one member at a time in shops and not at all
 *                   in the Item screen
 *   Equip           while the candidates for a slot are listed: all of them at once, each with what it changes for
 *                   the member (the PS1 shows one candidate's changes at a time)
 *                   and, when it teaches abilities, each one the member can learn with their AP towards it (the
 *                   PS1 shows taught abilities only in Equip, one member at a time, and never in shops)
 *   Ability         which of the member's equipment teaches the highlighted ability (the PS1 shows it only in Equip)
 *   Help (Select)   the help text, large, along the top screen's foot; the bottom keeps the moogle by the cursor (the
 *                   PS1's help bubble covered the menu it explains)
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
u8 chr_getApNeeded(s32, s32);
void ui_drawItemIcon(s32, s32, s32, s32, s32);

extern unsigned char psx_ram[];
static int ram_ok(const void *p, unsigned size) { /* (PS1 RAM and the overlays' data after it) */
    const unsigned char *b = (const unsigned char *)p;
    return b >= psx_ram + 0x10000 && b + size <= psx_ram + 0x1000000;
}
/* the overlays' statics (their layouts: the decomp session's offsets) */
extern u8 s_menuItem[], s_menuItemHelp[], s_menuEquip[], s_menuEquipItemHelp[], s_menuAbility[], s_menuSave[];
extern u8 s_menuParty[], s_menuStatus[];
extern u8 s_shop[], s_shopBuy[], s_shopSell[], s_shopSynth[], s_shopItemHelp[];
/* The ability names are the Ability screen's (ovl_33, file 0/0x33 read to 0x801EDF00): its action table (0x10-byte
 * entries, the name offset at +0xE) at 0x801F34EC, action names at 0x801F40EC, support names at 0x801F6540. Shops run
 * in the same overlay slot, so the port reads those sectors of the file once (bytes 0x5000-0x9000) and keeps them. */
void psx_cd_read_data(unsigned int lba, void *buf, unsigned int n);
#define ABIL_FROM 0x5000
static u8 abil[0x4000];
static int abil_state; /* 0 not read, 1 read, -1 unreadable */

static u8 *ability_name(int id) {
    if (!abil_state) {
        FileEntry *f = file_find(0, 0x33);
        abil_state = -1;
        if (f && (unsigned)(f->link - f->sector) * 2048 >= ABIL_FROM + sizeof abil) {
            psx_cd_read_data((unsigned)(g_gameState->imgSector + f->sector) + ABIL_FROM / 2048, abil, sizeof abil / 2048);
            abil_state = 1;
        }
    }
    if (abil_state < 0) return NULL;
    if (id >= ABILITY_SUPPORT_FIRST) return abil + 0x8640 - ABIL_FROM + g_supportAbilities[id - ABILITY_SUPPORT_FIRST].name;
    unsigned ofs = *(u16 *)(abil + 0x55EC - ABIL_FROM + id * 0x10 + 0xE);
    return ofs < 0x8640 - 0x61EC ? abil + 0x61EC - ABIL_FROM + ofs : NULL;
}

/* the abilities item id teaches that member c can learn: name and AP gathered / needed (grey once mastered), from x
 * on one row up to x_end; how many shown */
static int teaches(int x, int y, int x_end, Character *c, int id) {
    int ch = c->info.b.id, shown = 0;
    u8 buf[16];
    for (int k = 0; k < 3; k++) {
        int a = g_itemTable[id].abilities[k];
        if (!a) continue;
        int need = chr_getApNeeded(ch, a), have = chr_getAp(ch, a);
        if (need <= 0 || have < 0) continue;
        u8 *name = ability_name(a);
        if (!name) break;
        int w = text_measure(name).w, done = have >= need;
        if (x + w + (done ? 0 : 34) > x_end) break;
        text_drawBlock(x, y, 2, name, done ? 2 : 0);
        x += w + 4;
        if (!done) {
            text_formatNumber(have, buf, -1, 1);
            int n = strlen((char *)buf);
            buf[n] = '/';
            text_formatNumber(need, buf + n + 1, -1, 1);
            ui_drawSmallDigits(x, y + 4, 2, buf, 1);
            x += text_measureDigits(buf);
        }
        x += 12;
        shown++;
    }
    return shown;
}

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

/* a dim highlight bar behind a row */
static void bar_at(int x, int y, int w, int h) {
    u32 *p = (u32 *)g_primPtr;
    p[0] = 3u << 24; p[1] = 0x60403020u; p[2] = (u32)x | (u32)y << 16; p[3] = (u32)w | (u32)h << 16;
    addPrim(&g_ot[4], p);
    g_primPtr = (u8 *)(p + 4);
}

/* a small triangle pointing up (gain, green) or down (loss, red) at (x, y), 7x5 */
static void updown(int x, int y, int up) {
    u32 *p = (u32 *)g_primPtr;
    p[0] = 4u << 24;
    p[1] = 0x20000000u | (up ? 0x30D040u : 0x3040E0u);
    p[2] = up ? ((u32)(x + 3) | (u32)y << 16) : ((u32)x | (u32)y << 16);
    p[3] = up ? ((u32)x | (u32)(y + 5) << 16) : ((u32)(x + 7) | (u32)y << 16);
    p[4] = up ? ((u32)(x + 7) | (u32)(y + 5) << 16) : ((u32)(x + 3) | (u32)(y + 5) << 16);
    addPrim(&g_ot[2], p);
    g_primPtr = (u8 *)(p + 5);
}

/* what an item changes for a member: up to n stats as "LABEL ^3", from x on one row; how many shown */
static int changes(int x, int y, Character *c, int slot, int id, int n) {
    static const char *const names[] = {"SPD", "STR", "MAG", "SPR", "ATK", "DEF", "EVA", "MDEF", "MEVA", "HP", "MP"};
    EquipStatsOut b = stats_with(c, -1, 0), a = stats_with(c, slot, id);
    int before[11] = {b.stats[0], b.stats[1], b.stats[2], b.stats[3], b.defs[0], b.defs[1], b.defs[2], b.defs[3], b.defs[4], b.maxHp, b.maxMp};
    int after[11] = {a.stats[0], a.stats[1], a.stats[2], a.stats[3], a.defs[0], a.defs[1], a.defs[2], a.defs[3], a.defs[4], a.maxHp, a.maxMp};
    int shown = 0;
    u8 buf[8];
    for (int k = 0; k < 11 && shown < n; k++) {
        int d = after[k] - before[k];
        if (!d) continue;
        ui_drawLabel(x, y, 2, (s32)names[k], 0);
        int w = ui_measureLabel((u8 *)names[k]);
        updown(x + w + 3, y, d > 0);
        text_formatNumber(d > 0 ? d : -d, buf, -1, 1);
        ui_drawSmallDigits(x + w + 12, y, 2, buf, d > 0 ? 0 : 2);
        x += w + 40;
        shown++;
    }
    return shown;
}

/* an item (equipment): its description, who can wear it and what it changes for each; 0 if it isn't equipment */
static int item_view(int id, u8 *help) {
    static RECT box = {8, 8, 384, 224};
    if (id < 0 || id >= 0xFF) return 0;
    ItemInfo *it = &g_itemTable[id];
    int slot = -1;
    for (int s = 0; s < 5; s++)
        if (it->slots & g_equipSlotMasks[s]) slot = s;
    if (slot < 0 || !(it->equipMask & 0xFFF)) return 0; /* (nobody wears it: Phoenix Pinion, the throwing items) */
    ui_drawItemIcon(22, 20, 2, id, 1);
    text_drawBlock(42, 20, 2, g_itemNames + it->name, 0);
    /* (its description under the name, as Select's help gave it; the party's rows share what is left) */
    int top = 46, step = 46, size = 40;
    if (help && ram_ok(help, 1) && *help != TEXT_END) {
        text_drawBlock(22, 38, 2, help, 0);
        top = 38 + text_measure(help).h + 6;
        step = (226 - top) / 4;
        if (step > 46) step = 46;
        size = step - 4 < 40 ? step - 4 : 40;
    }
    GameData *g = g_gameState->gameData;
    for (int i = 0; i < 4; i++) {
        Character *c = g->party[i];
        if (!ram_ok(c, sizeof *c)) continue;
        int ok = (it->equipMask & (0x800 >> chr_getDefaultSet(c))) != 0, y = top + i * step;
        face(i, chr_portrait(c), 22, y, size);
        text_drawBlock(70, y, 2, c->name, ok ? 0 : 2);
        if (!ok) {
            ui_drawLabel(70, y + 18, 2, (s32)"CAN T EQUIP", 2);
            continue;
        }
        int same = 0;
        for (int s = 0; s < 5; s++) same |= c->equip[s] == id;
        /* (rows closer than three lines (a long description: Cachusha's four): the abilities beside the name, the
         * changes under it; else under each other, the abilities ran into the next member's row) */
        int compact = step < 42, ly = compact ? y + 15 : y + 18;
        if (same) ui_drawLabel(70, ly, 2, (s32)"EQUIPPED", 1);
        else if (!changes(70, ly, c, slot, id, 4)) ui_drawLabel(70, ly, 2, (s32)"NO CHANGE", 2);
        if (compact) teaches(70 + text_measure(c->name).w + 16, y, 380, c, id);
        else teaches(70, y + 29, 380, c, id);
    }
    win_drawBox(&box, 5, 2);
    return 1;
}

/* the Equip screen with a slot's candidates listed: all of them, each with what it changes for the member */
static int equip_view(const u8 *st) {
    static RECT box = {8, 8, 384, 224};
    GameData *g = g_gameState->gameData;
    int member = st[3], slot = st[2];
    if (st[1] != 3 || member > 3 || !ram_ok(g->party[member], sizeof(Character))) return 0;
    Character *c = g->party[member];
    MenuList *L = (MenuList *)(st + 4);
    int count = L->count, cur = L->top + L->cursor;
    if (count <= 0) return 0;
    face(0, chr_portrait(c), 18, 16, 32);
    text_drawBlock(58, 24, 2, c->name, 0);
    /* a row per candidate: its name and what it changes, under them the abilities it teaches with the member's AP */
    int rows = 6, first = cur - rows / 2;
    if (first > count - rows) first = count - rows;
    if (first < 0) first = 0;
    for (int i = 0; i < rows && first + i < count; i++) {
        int id = st[0x3C + (first + i) * 2], y = 56 + i * 28;
        if (id == 0xFF) continue;
        if (first + i == cur) bar_at(18, y - 2, 364, 28);
        ui_drawItemIcon(22, y, 2, id, 1);
        text_drawBlock(40, y, 2, g_itemNames + g_itemTable[id].name, 0);
        if (c->equip[slot] == id) ui_drawLabel(176, y + 3, 2, (s32)"EQUIPPED", 1);
        else if (!changes(176, y + 3, c, slot, id, 4)) ui_drawLabel(176, y + 3, 2, (s32)"NO CHANGE", 2);
        teaches(40, y + 13, 380, c, id);
    }
    win_drawBox(&box, 5, 2);
    return 1;
}

/* the Ability screen's list: the highlighted ability's description (its Select help, which the top shows instead) and
 * what teaches it */
void o33_menuAbility_updateHelp(void);
static int ability_view(const u8 *st) {
    static RECT box = {8, 8, 384, 224};
    static const u8 taught[] = "TAUGHT BY";
    GameData *g = g_gameState->gameData;
    int member = st[2]; /* (AbilityMenu.slot: the member shown; g_menuScreen.member is the one the screen opened on) */
    if (st[1] != 2 || member > 3 || !ram_ok(g->party[member], sizeof(Character))) return 0;
    Character *c = g->party[member];
    int tab = st[0];
    MenuList *L = (MenuList *)(st + (tab ? 0x18 : 8));
    if (L->count <= 0) return 0;
    int n = (L->top + L->cursor) * L->cols + L->col, id = tab ? st[0x728 + n] : st[0x668 + n * 4];
    /* (the game's own help text for it: its update, as help.c runs it while the help is open; text and bounds only) */
    u8 *was = g_menuHelp.text;
    o33_menuAbility_updateHelp();
    u8 *help = g_menuHelp.text;
    g_menuHelp.text = was;
    face(0, chr_portrait(c), 22, 20, 48);
    text_drawBlock(80, 24, 2, c->name, 0);
    int y = 80;
    if (help && ram_ok(help, 1) && *help != TEXT_END) {
        text_drawBlock(28, y, 2, help, 0);
        y += text_measure(help).h + 14;
    }
    int any = 0;
    for (int s = 0; id && s < 5; s++) {
        int it = c->equip[s];
        if (it == 0xFF) continue;
        for (int k = 0; k < 3; k++)
            if (g_itemTable[it].abilities[k] == id) {
                if (!any) ui_drawLabel(28, y, 2, (s32)taught, 0), y += 16;
                ui_drawItemIcon(28, y, 2, it, 1);
                text_drawBlock(48, y, 2, g_itemNames + g_itemTable[it].name, 0);
                y += 20;
                any = 1;
            }
    }
    win_drawBox(&box, 5, 2);
    return 1;
}

/* the panel with nothing in it: while a screen that has top views points at something without one (a potion in the
 * shop), so the top doesn't flick between a panel and the scene as the cursor moves */
static int empty_panel(void) {
    static RECT box = {8, 8, 384, 224};
    win_drawBox(&box, 5, 2);
    return 1;
}

/* An item that isn't equipment (a potion, a Phoenix Down, a tent...): its icon and name, how many are held, its
 * description (the screen's own help text), and the party's HP and MP with bars, what one looks at before using it.
 * help: the description, NULL if none. */
static void hpmp_bar(int x, int y, int w, int cur, int max, u32 bgr) {
    u32 *p = (u32 *)g_primPtr;
    int f = max > 0 ? cur * (w - 2) / max : 0;
    if (f > w - 2) f = w - 2;
    p[0] = 3u << 24; p[1] = 0x60302020u; p[2] = (u32)x | (u32)y << 16; p[3] = (u32)w | (u32)4 << 16;
    addPrim(&g_ot[2], p);
    p += 4;
    if (f > 0) {
        p[0] = 3u << 24; p[1] = 0x60000000u | bgr; p[2] = (u32)(x + 1) | (u32)(y + 1) << 16; p[3] = (u32)f | (u32)2 << 16;
        addPrim(&g_ot[2], p);
        p += 4;
    }
    g_primPtr = (u8 *)p;
}
static int consumable_view(int id, u8 *help) {
    static RECT box = {8, 8, 384, 224}, party = {16, 92, 368, 132};
    GameData *g = g_gameState->gameData;
    u8 a[16];
    ui_drawItemIcon(22, 20, 2, id, 1);
    text_drawBlock(42, 20, 2, g_itemNames + g_itemTable[id].name, 0);
    int held = 0;
    for (int s = 0; s < 256; s++)
        if (g->items.slots[s].id == id) held += g->items.slots[s].count;
    ui_drawLabel(300, 22, 2, (s32) "HELD", 0);
    text_formatNumber(held, a, 2, 1);
    ui_drawDigits(334, 21, 2, a, 0);
    if (help && ram_ok(help, 1)) text_drawBlock(22, 44, 2, help, 0);
    for (int i = 0; i < 4; i++) { /* (the party: who needs it) */
        Character *c = g->party[i];
        if (!ram_ok(c, sizeof *c)) continue;
        int y = party.y + 6 + i * 31;
        face(i, chr_portrait(c), party.x + 6, y, 28);
        text_drawBlock(party.x + 40, y + 2, 2, c->name, 0);
        ui_drawLabel(party.x + 130, y + 4, 2, (s32) "HP", 0);
        text_formatNumber(c->hp, a, 4, 1);
        a[4] = '/';
        text_formatNumber(c->maxHp, a + 5, -1, 1);
        ui_drawSmallDigits(party.x + 148, y + 4, 2, a, c->hp <= c->maxHp / 6 ? 4 : 0);
        hpmp_bar(party.x + 130, y + 16, 100, c->hp, c->maxHp, 0x40C040);
        ui_drawLabel(party.x + 250, y + 4, 2, (s32) "MP", 0);
        text_formatNumber(c->mp, a, 3, 1);
        a[3] = '/';
        text_formatNumber(c->maxMp, a + 4, -1, 1);
        ui_drawSmallDigits(party.x + 268, y + 4, 2, a, 0);
        hpmp_bar(party.x + 250, y + 16, 100, c->mp, c->maxMp, 0xE09040);
    }
    win_drawBox(&party, 4, 2);
    win_drawBox(&box, 5, 2);
    return 1;
}
/* any item for the top screen: equipment as item_view, the rest as consumable_view; help: its description */
int port_item_panel(int id, u8 *help) {
    if (id < 0 || id >= 0xFF) return empty_panel();
    return item_view(id, help) || consumable_view(id, help);
}
/* a key item: its name and description, large */
int port_key_item_panel(u8 *name, u8 *help) {
    static RECT box = {8, 8, 384, 224};
    if (name && ram_ok(name, 1)) text_drawBlock(22, 20, 2, name, 0);
    if (help && ram_ok(help, 1)) text_drawBlock(22, 48, 2, help, 0);
    win_drawBox(&box, 5, 2);
    return 1;
}

/* field_hud.c's top-screen frame while a menu is up: 1 if drawn */
/* A member at a glance, beside their model (Equip and Status, model_view.c on the left): the abilities their gear is
 * teaching them (each equipped item's, once each), the AP gathered towards each with a bar, mastered ones dim. What
 * the PS1 shows only on the Ability screen, one ability at a time. */
static void ap_bar(int x, int y, int w, int have, int need) {
    u32 *p = (u32 *)g_primPtr;
    int f = need > 0 ? have * (w - 2) / need : 0;
    if (f > w - 2) f = w - 2;
    p[0] = 3u << 24; p[1] = 0x60302020u; p[2] = (u32)x | (u32)y << 16; p[3] = (u32)w | (u32)4 << 16;
    addPrim(&g_ot[2], p);
    p += 4;
    if (f > 0) {
        p[0] = 3u << 24; p[1] = 0x6030C8F0u; p[2] = (u32)(x + 1) | (u32)(y + 1) << 16; p[3] = (u32)f | (u32)2 << 16;
        addPrim(&g_ot[2], p);
        p += 4;
    }
    g_primPtr = (u8 *)p;
}
static void member_view(Character *c) {
    static RECT box = {204, 8, 188, 224};
    ui_drawLabel(box.x + 8, box.y - 3, 2, (s32) "LEARNING", 0);
    int ch = c->info.b.id, y = box.y + 10, seen[16], nseen = 0;
    u8 buf[16];
    for (int sl = 0; sl < 5 && y < box.y + box.h - 24; sl++) {
        int item = c->equip[sl];
        if (item == 0xFF) continue;
        for (int k = 0; k < 3 && y < box.y + box.h - 24; k++) {
            int a = g_itemTable[item].abilities[k], dup = 0;
            if (!a) continue;
            for (int j = 0; j < nseen; j++) dup |= seen[j] == a;
            if (dup || nseen == 16) continue;
            seen[nseen++] = a;
            int need = chr_getApNeeded(ch, a), have = chr_getAp(ch, a);
            if (need <= 0 || have < 0) continue; /* (not one this member learns) */
            u8 *name = ability_name(a);
            if (!name) continue;
            int done = have >= need;
            text_drawBlock(box.x + 10, y, 2, name, done ? 2 : 0);
            if (!done) {
                text_formatNumber(have, buf, -1, 1);
                int n = strlen((char *)buf);
                buf[n] = '/';
                text_formatNumber(need, buf + n + 1, -1, 1);
                ui_drawSmallDigits(box.x + box.w - 10 - text_measureDigits(buf), y + 3, 2, buf, 1);
                ap_bar(box.x + 10, y + 15, box.w - 20, have, need);
            }
            y += 24;
        }
    }
    if (y == box.y + 10) ui_drawLabel(box.x + 10, y + 4, 2, (s32) "NOTHING TO LEARN", 2);
    win_drawBox(&box, 4, 2);
}

/* Order: the party as they stand in battle (model_view.c: each in their battle idle with their weapon, the back row a
 * step nearer the camera), each one's name and row under their feet, the Order cursor's member marked by a bar under
 * theirs (Character.battleRow: 1 the front row, 0 the back, as the menu sets the back row in) */
static void order_view(int cur) {
    GameData *g = g_gameState->gameData;
    Character *party[4];
    int backrow[4];
    float feet[4][2];
    for (int i = 0; i < 4; i++) {
        Character *c = g->party[i];
        party[i] = ram_ok(c, sizeof *c) ? c : NULL;
        backrow[i] = party[i] && c->info.b.battleRow == 0;
    }
    void port_party_view(Character *const party[4], const int backrow[4], float height, float feet[4][2]);
    port_party_view(party, backrow, 104, feet);
    for (int i = 0; i < 4; i++) {
        if (!party[i] || feet[i][0] < 0) continue;
        int x = (int)feet[i][0] - 44, y = (int)feet[i][1] + 8;
        if (x < 4) x = 4;
        if (x > 308) x = 308;
        if (y > 202) y = 202;
        if (i == cur) bar_at(x, y - 2, 88, 34);
        text_drawBlock(x + 4, y, 2, party[i]->name, 0);
        ui_drawLabel(x + 4, y + 16, 2, (s32)(backrow[i] ? "BACK ROW" : "FRONT ROW"), 0);
    }
}

long VSync(long);
int port_menu_top_draw(void) {
    GameData *g = g_gameState->gameData;
    switch (g_menu.type) {
    case 0: /* the main menu: its screens */
        if (!g_menuScreen.active) return 0;
        if (g_menuScreen.screen == 0) { /* (the main menu itself: in Order, the formation) */
            extern u8 o0a_s_menuMain[], o0c_s_menuMain[];
            const u8 *mm = g_gameState->mode == 3 ? o0c_s_menuMain : o0a_s_menuMain;
            if (mm[1] == 2) { order_view(mm[2]); return 1; } /* (MenuMain.state MENUMAIN_ORDER; .orderCursor) */
            return 0;
        }
        switch (g_menuScreen.screen) {
        case 1: { /* Item: the item or key item under the cursor (item_top.c) */
            int port_item_top(void);
            return port_item_top();
        }
        case 2: { /* Ability: the highlighted ability's teachers; else (the tabs, where L / R switch member) as Status */
            if (ability_view(s_menuAbility)) return 1;
            void port_model_view(Character *c, float cx, float cy, float height);
            int member = s_menuAbility[2];
            if (member < 4 && ram_ok(g->party[member], sizeof(Character))) {
                port_model_view(g->party[member], 104, 226, 160);
                member_view(g->party[member]);
                return 1;
            }
            return s_menuAbility[1] == 2 && empty_panel();
        }
        case 3: { /* the slot's candidates, or the member's model turning in what they wear (model_view.c) and what
                   * their gear is teaching them */
            if (equip_view(s_menuEquip)) return 1;
            void port_model_view(Character *c, float cx, float cy, float height);
            int member = s_menuEquip[3];
            if (member < 4 && ram_ok(g->party[member], sizeof(Character))) {
                port_model_view(g->party[member], 104, 226, 160);
                member_view(g->party[member]);
                return 1;
            }
            return 0;
        }
        case 4: { /* Status: the member's model and what their gear is teaching them */
            void port_model_view(Character *c, float cx, float cy, float height);
            int member = s_menuStatus[2];
            if (member < 4 && ram_ok(g->party[member], sizeof(Character))) {
                port_model_view(g->party[member], 104, 226, 160);
                member_view(g->party[member]);
                return 1;
            }
            return 0;
        }
        case 6: { /* Card: the card under the cursor (card_touch.c) */
            int port_card_top(void);
            return port_card_top();
        }
        default: return 0;
        }
    case 3: { /* party select: whoever the cursor is on, turning in their gear (model_view.c) */
        if (g_menu.state != 3) return 0;
        const u8 *ps = s_menuParty; /* (ovl_3b.h PartySelect: mode, size, cursor[2] {idx, row}, ..., slots[2][8] at 10) */
        int mode = ps[0] & 1, idx = ps[4 + mode * 2], row = ps[5 + mode * 2] & 1;
        int id = ps[10 + row * 8 + (idx & 7)];
        if (id >= 9) return 0;
        void port_model_view(Character *c, float cx, float cy, float height);
        port_model_view(&g->characters[id], 200, 226, 160);
        return 0;
    }
    case 5: { /* the Chocograph screen (chocograph_touch.c) */
        int port_chocograph_top(void);
        return port_chocograph_top();
    }
    case 2: { /* a shop */
        const u8 *sh = s_shop;
        const u8 *help = s_shopItemHelp;
        int id = -1;
        if (!sh[3]) return 0; /* (the shop's own menu, no mode running) */
        /* (only once the shop is up: while its menu fades in, s_shop is still the previous shop's (a mode running, its
         * item lists), and the panel showed a stale item with garbage for its description, the first time a shopkeeper
         * is talked to: South Gate's rest stop) */
        if (g_menu.state != 3 || fade_isBusy()) return 0;
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
        if (id < 0) return empty_panel();
        return port_item_panel(id, help + g_itemTable[id].help); /* (the shop's help text: the item's description) */
    }
    default: return 0;
    }
}

/* ---- Select help on the top screen ----
 * The game's help (g_menuHelp: help_update calls help_draw while it's open) draws the moogle by the hand and a bubble
 * with the text beside it, over the menu. While a menu is up the bubble's text goes to the top screen instead
 * (port_help_top_draw), and only the hand and moogle stay. help_update is wrapped (config/renames.txt; its own call to
 * help_draw is inside its object, so help_draw itself can't be): the same steps, with the port's help_draw. */
int port_menu_state(void);
void help_draw(void);
void help_onDraw(void);

/* the top screen shows the highlighted entry's description (the item list, the ability list, a shop's lists): Select opens no help
 * there, and help left open from another screen isn't drawn over the list (it would be, under the top's panels) */
int port_item_top_describes(void);
static int top_describes(void) {
    if (port_menu_state() != 2) return 0;
    if (g_menu.type == 2) return s_shop[3] != 0 && g_menu.state == 3 && !fade_isBusy(); /* (a shop's buy / sell / synthesis list:
                                                                                      * port_menu_top_draw's item panel) */
    if (g_menu.type != 0 || !g_menuScreen.active) return 0;
    if (g_menuScreen.screen == 1) return port_item_top_describes();
    if (g_menuScreen.screen == 2) return s_menuAbility[1] == 2; /* (ABILITY_STATE_LIST) */
    return 0;
}

static int help_moved(void) {
    int st = port_menu_state(); /* (a menu, or the card game: tm_ui.c shows its hints on top) */
    return (st == 1 || st == 2 || st == 3) && g_menuHelp.isOpen && g_menuHelp.textShown && g_menuHelp.text &&
           *g_menuHelp.text != TEXT_END;
}

void help_update_game(void);
void help_toggle(void);

static void help_draw_port(void) {
    if (!help_moved()) {
        help_draw();
        return;
    }
    int otz = g_menuHelp.cursorOnTop ? 1 : g_menuHelp.otz;
    if (g_menuHelp.follow) {
        if (g_menuHelp.cursorOnTop) ui_drawHand(g_menuHelp.x, g_menuHelp.y, otz);
        sprAnim_draw(g_menuHelp.x + 5, g_menuHelp.y + 2, otz, &g_menuHelp.iconAnim);
    }
    help_onDraw();
}

void help_update(void) { /* (help.c's, drawing with help_draw_port) */
    /* (Select opens no help where the top screen already shows the highlighted entry's description: the item list) */
    int shown = top_describes();
    if (!g_menuHelp.locked && !fade_isBusy() && (g_padRepeat & BTN_SELECT) && (g_menuHelp.isOpen || !shown)) help_toggle();
    if (g_menuHelp.isOpen && !shown && !fade_isBusy() && g_menuHelp.callback) {
        g_menuHelp.callback();
        if (g_menuHelp.text && *g_menuHelp.text != TEXT_END) help_draw_port();
    }
    sprAnim_update(&g_menuHelp.iconAnim);
}

/* a help text along the top screen's foot, in the game's help frame */
void port_help_panel(u8 *text) {
    TextSize sz = text_measure(text);
    RECT r;
    r.w = 368;
    r.h = (s16)(sz.h + 18);
    r.x = 16;
    r.y = (s16)(232 - r.h);
    ui_drawSprite(r.x + 8, r.y, 1, 0xB7, 0);
    text_drawBlock(r.x + 10, r.y + 10, 1, text, TEXT_PAL_HELP);
    win_drawHelpFrame(&r, 1, HELP_TAIL_NONE, 0);
}

/* the menu's help text there (field_hud.c's top frame, over the menu's views): 1 if drawn */
int port_help_top_draw(void) {
    if (!help_moved() || port_menu_state() == 3 || top_describes()) return 0;
    port_help_panel(g_menuHelp.text);
    return 1;
}
