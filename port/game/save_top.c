/* The save / load screen's top screen: the file the list's cursor is on, large (its disc, location, the party's
 * portraits with names and levels, play time and gil: what the PS1's row shows in small, and the party it doesn't);
 * on an empty block, or before the list (checking the card), the game's name. Drawn with the game's window and text
 * code into field_hud.c's top-screen list (menu_top.c calls it for menu type 4). */
#define M2CTX
#include "ovl_39.h"

void port_big_portrait_index(int slot, int index, int x, int y, int size);

static void name_box(void) {
    static RECT box = {100, 84, 200, 72};
    static const char name[] = "FINAL FANTASY IX";
    int w = ui_measureLabel((u8 *)name);
    ui_drawLabel(box.x + (box.w - w) / 2, box.y + 32, 2, (s32)name, 0);
    win_drawBox(&box, 4, WIN_STYLE_BEVEL);
}

int port_save_top(void) {
    if (g_menu.state != 3 || g_menu.type != 4) return 0;
    SaveMenu *m = &s_menuSave;
    int row = m->list.top + m->list.cursor;
    if (m->state != SAVE_ST_LIST && m->state != SAVE_ST_CONFIRM) { name_box(); return 1; }
    if (row < 0 || row >= SAVE_FILES || m->files[row].state != SAVE_FILE_USED) { name_box(); return 1; }
    SaveFile *f = &m->files[row];
    static RECT box = {8, 8, 384, 224};
    u8 a[16];
    /* the disc: the highest one this save is on */
    int disc = 1;
    for (int d = 0; d < 4; d++)
        if (f->discs >> d & 1) disc = d + 1;
    ui_drawLabel(24, 22, 2, (s32) "DISC", 0);
    a[0] = (u8)('0' + disc), a[1] = 0;
    ui_drawDigits(56, 21, 2, a, 0);
    u8 loc[0x1C + 1];
    __builtin_memcpy(loc, &f->location, 0x1C);
    loc[0x1C] = 0xFF;
    text_drawBlock(84, 19, 2, loc, 0);
    for (int i = 0; i < 4; i++) { /* (the party, in a row: portrait, name, level) */
        SaveMember *p = &f->party[i];
        if (!p->present) continue;
        int x = 24 + i * 92, y = 52;
        if (p->portrait != 0xFF) port_big_portrait_index(4 + i, p->portrait, x + 14, y, 56);
        u8 nm[0xA + 1];
        __builtin_memcpy(nm, p->name, 0xA);
        nm[0xA] = 0xFF;
        text_drawBlock(x + 4, y + 64, 2, nm, 0);
        ui_drawLabel(x + 4, y + 84, 2, (s32) "LV", 0);
        text_formatNumber(p->level, a, 2, TEXT_NUM_ASCII);
        ui_drawDigits(x + 22, y + 83, 2, a, 0);
    }
    ui_drawSprite(24, 182, 2, 0xB1, 0); /* (the clock and the gil as the main menu shows them) */
    ui_drawSmallDigits(38, 183, 2, a, text_formatTime(f->playTime, a, 1));
    text_formatNumber(f->gil, a, 7, TEXT_NUM_ASCII);
    ui_drawSmallDigits(140, 183, 2, a, 0);
    ui_drawSprite(196, 182, 2, 0xB2, 0);
    win_drawBox(&box, 4, WIN_STYLE_BEVEL);
    return 1;
}
