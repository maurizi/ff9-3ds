/* The bottom screen in a field (docs/ui-design.md): where you are and how the party is, drawn by the game's own
 * window and text code (its frames, font, labels and digits) into a packet buffer of the port's, which the 3DS
 * renderer draws as a UI list on the bottom screen. Nothing here is needed while walking: speech and prompts stay on
 * the top screen with the speakers.
 *
 *   LOCATION: the menu's location text           (GameData.locationName)
 *   the party: name, HP cur/max, MP cur/max      (GameData.party)
 *   time and gil                                  as the main menu shows them
 *   [MENU]                                        a touch shortcut for triangle (port_hud_button_at)
 *
 * The game's drawing functions append to g_primPtr and link into g_ot[otz]: both are pointed at the port's buffer
 * while it draws, and restored. A packet at the end of the ordering table (drawn first) sets the drawing area and
 * offset to the displayed buffer, so the coordinates are the screen's whichever buffer the field draws next; the
 * GPU state is put back after. */
#define M2CTX
#include "ff9.h"
#include "gp0.h"
#include "psx_display.h"

void port_draw_ot_buffer(unsigned int *head, const void *buffer, unsigned int size);
void psx_ui_list_begin(void);
void psx_ui_list_end(void);
int port_menu_state(void);
u32 *ClearOTagR(u32 *ot, int n);

#define OTN 8
#define BUF_WORDS 0x1000
/* the ordering table and the packets in one array: links are 24-bit, so they share their high byte */
static u32 hud_mem[OTN + BUF_WORDS];
static u32 *const hud_ot = hud_mem;

#define BOX_X 6
#define BOX_W 308
static RECT r_location = {BOX_X, 10, BOX_W, 26};
static RECT r_party = {BOX_X, 50, BOX_W, 4 * 24 + 10};
static RECT r_timegil = {BOX_X, 168, 150, 34};
static RECT r_menu = {164, 168, 150, 34};

static int shown;

/* whether the field screen is up (and so its buttons are there to touch) */
int port_hud_shown(void) {
    return shown;
}

/* the PS1 button a touch at (x, y) of the bottom screen's 320x240 stands for, 0 = none */
int port_hud_button_at(int x, int y, int oy) {
    if (!shown) return 0;
    y -= oy;
    if (x >= r_menu.x && x < r_menu.x + r_menu.w && y >= r_menu.y && y < r_menu.y + r_menu.h) return 0x1000; /* triangle */
    return 0;
}

static void draw_party(void) {
    static const u8 hp[] = "HP", mp[] = "MP";
    GameData *g = g_gameState->gameData;
    u8 a[12], b[12];
    int y = r_party.y + 8;
    for (int i = 0; i < 4; i++, y += 24) {
        Character *c = g->party[i];
        if (!c) continue;
        text_drawBlock(r_party.x + 10, y, 2, c->name, 0);
        int low = c->hp == 0 ? 3 : (c->hp <= c->maxHp / 6) * 4;
        ui_drawLabel(r_party.x + 104, y + 2, 2, (s32)hp, 0);
        text_formatNumber(c->hp, a, 4, 1);
        b[0] = '/';
        text_formatNumber(c->maxHp, b + 1, -1, 1);
        ui_drawDigits(r_party.x + 122, y + 1, 2, a, low);
        ui_drawDigits(r_party.x + 154, y + 1, 2, b, low);
        ui_drawLabel(r_party.x + 206, y + 2, 2, (s32)mp, 0);
        text_formatNumber(c->mp, a, 3, 1);
        b[0] = '/';
        text_formatNumber(c->maxMp, b + 1, -1, 1);
        ui_drawDigits(r_party.x + 224, y + 1, 2, a, c->mp < c->maxMp / 6 ? 4 : 0);
        ui_drawDigits(r_party.x + 248, y + 1, 2, b, c->mp < c->maxMp / 6 ? 4 : 0);
    }
    win_drawBox(&r_party, 3, 2);
}

static void draw(void) {
    static const u8 menu[] = "MENU", party[] = "PARTY", gil_suffix[] = "G";
    GameData *g = g_gameState->gameData;
    u8 buf[16];

    ui_drawLabel(r_location.x + 8, r_location.y - 3, 2, g_labelLocation, 0);
    if (g->locationName != NULL && *g->locationName != 0xFF) text_drawBlock(r_location.x + 8, r_location.y + 7, 2, g->locationName, 0);
    win_drawBox(&r_location, 3, 2);

    ui_drawLabel(r_party.x + 8, r_party.y - 3, 2, (s32)party, 0);
    draw_party();

    ui_drawLabel(r_timegil.x + 6, r_timegil.y - 3, 2, g_labelTimeAndGil, 0);
    ui_drawSprite(r_timegil.x + 6, r_timegil.y + 8, 2, 0xB1, 0);
    ui_drawSprite(r_timegil.x + 6, r_timegil.y + 0x15, 2, 0xB2, 0);
    ui_drawSmallDigits(r_timegil.x + 0x13, r_timegil.y + 9, 2, buf, text_formatTime(game_getPlayTime(), buf, 1));
    text_formatNumber(g->gil, buf, 7, 1);
    strcat((char *)buf, (const char *)gil_suffix);
    ui_drawSmallDigits(r_timegil.x + 0x13, r_timegil.y + 0x16, 2, buf, 0);
    win_drawBox(&r_timegil, 3, 2);

    ui_drawLabel(r_menu.x + (r_menu.w - ui_measureLabel((u8 *)menu)) / 2, r_menu.y + 12, 2, (s32)menu, 0);
    win_drawBox(&r_menu, 3, 2);
}

/* each frame (the 3DS's VBlank wait): the field screen, when a field is up with no menu */
void port_bottom_hud(void) {
    shown = 0;
    if (!g_gameState || !g_gameState->gameData || g_gameState->mode != 1 || port_menu_state()) return;
    if (!g_gameState->gameData->font) return; /* (the common images aren't loaded yet) */
    PsxDisplayInfo d;
    psx_gpu_display_info(&d);
    if (d.rgb24 || d.w < 256) return; /* (a movie) */

    ClearOTagR(hud_ot, OTN);
    u32 *saved_ot = g_ot;
    u8 *saved_prim = g_primPtr;
    u32 *p = hud_mem + OTN;
    /* drawn first: the drawing area and offset of the displayed buffer */
    p[0] = 3u << 24;
    p[1] = 0xE3000000u | (u32)d.x | (u32)d.y << 10;
    p[2] = 0xE4000000u | (u32)(d.x + d.w - 1) | (u32)(d.y + d.h - 1) << 10;
    p[3] = 0xE5000000u | ((u32)d.x & 0x7FF) | ((u32)d.y & 0x7FF) << 11;
    addPrim(&hud_ot[OTN - 1], p);
    g_ot = hud_ot;
    g_primPtr = (u8 *)(p + 4);
    draw();
    unsigned used = (unsigned)(g_primPtr - (u8 *)hud_mem);
    g_ot = saved_ot;
    g_primPtr = saved_prim;
    if (used > sizeof hud_mem) port_fatal("field HUD: packet buffer overrun");

    Gp0State saved_gp0 = gp0_state; /* (the field's drawing environment is the field's again after) */
    psx_ui_list_begin();
    port_draw_ot_buffer(&hud_ot[OTN - 1], hud_mem, sizeof hud_mem);
    psx_ui_list_end();
    gp0_state = saved_gp0;
    shown = 1;
}
