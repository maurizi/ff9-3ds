/* The bottom screen in a field (docs/ui-design.md): where you are and how the party is, drawn by the game's own
 * window and text code (its frames, font, labels and digits) into a packet buffer of the port's, which the 3DS
 * renderer draws as a UI list on the bottom screen. Nothing here is needed while walking: speech and prompts stay on
 * the top screen with the speakers.
 *
 *   header:  the location, play time, gil         (GameData.locationName; as the main menu shows them)
 *   MAP:     the walkmesh from above, the player  (port/game/field_map.c)
 *   PARTY:   portrait, name, HP and MP, status    (GameData.party; portraits: the menu's file, read by the port)
 *   [MENU] [CARDS n]                               MENU: a touch shortcut for triangle (port_hud_tap)
 * While a dialogue asks a question (its choices on the top screen with the speaker), the map and party give way to
 * the choices as large buttons: a tap moves the game's choice cursor there and confirms (an input aid: the cursor
 * and the buttons still work).
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
#define BUF_WORDS 0x3000
/* the ordering table and the packets in one array: links are 24-bit, so they share their high byte */
static u32 hud_mem[OTN + BUF_WORDS];
static u32 *const hud_ot = hud_mem;

static RECT r_head = {4, 6, 312, 24};
static RECT r_map = {4, 40, 172, 128};
static RECT r_party = {180, 40, 136, 128};
static RECT r_menu = {4, 178, 100, 38};
static RECT r_cards = {110, 178, 100, 38};

static int shown;

/* whether the field screen is up (and so its buttons are there to touch) */
int port_hud_shown(void) {
    return shown;
}

static int n_choices, choice_first; /* the question's choices shown this frame */
static RECT r_choice[8];

static int inside(const RECT *r, int x, int y) { return x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h; }

/* A tap at (x, y) of the bottom screen (its UI starts oy rows down): the PS1 presses it stands for, into seq (at most
 * max), how many; 0: none here. */
int port_hud_tap(int x, int y, int oy, unsigned *seq, int max) {
    if (!shown) return 0;
    y -= oy;
    if (n_choices) {
        for (int i = 0; i < n_choices; i++) {
            if (!inside(&r_choice[i], x, y)) continue;
            int n = 0, d = i - (g_msgChoice - choice_first);
            while (d && n < max - 1) seq[n++] = d > 0 ? 0x40u : 0x10u, d += d > 0 ? -1 : 1; /* down / up */
            seq[n++] = 0x4000; /* cross */
            return n;
        }
        return 0;
    }
    if (inside(&r_menu, x, y) && max > 0) { seq[0] = 0x1000; return 1; } /* triangle */
    return 0;
}

/* ---- portraits: the menu's portrait file (directory 1, file 30: 0x6C0 bytes each, a 128-colour CLUT, a 32x32 8-bit
 * face and its 32x14 lower part), read once by the port and drawn by the renderer as its own images */
__attribute__((weak)) void ctr_hud_image(int slot, u32 key, const u16 *clut, const u8 *pix, int w, int h) {
    (void)slot; (void)key; (void)clut; (void)pix; (void)w; (void)h;
}
__attribute__((weak)) void ctr_hud_draw(int slot, int x, int y, int w, int h) { (void)slot; (void)x; (void)y; (void)w; (void)h; }
void psx_cd_read_data(unsigned int lba, void *buf, unsigned int n);
#define PORTRAIT_BYTES 0x6C0
static u8 portraits[48 * 1024];
static int portraits_state; /* 0 not read, 1 read, -1 unreadable */

static void portrait(int slot, int index, int x, int y) {
    if (portraits_state == 0) {
        FileEntry *f = file_find(1, 30);
        unsigned n = f ? (unsigned)(f->unkC - f->sector) : 0;
        portraits_state = -1;
        if (n && n * 2048 <= sizeof portraits) {
            psx_cd_read_data((unsigned)(g_gameState->imgSector + f->sector), portraits, n);
            portraits_state = 1;
        }
    }
    if (portraits_state < 0 || (unsigned)(index + 1) * PORTRAIT_BYTES > sizeof portraits) return;
    const u8 *e = portraits + index * PORTRAIT_BYTES;
    ctr_hud_image(slot, 0x100u | (u32)index, (const u16 *)e, e + 0x100, 32, 32);
    ctr_hud_draw(slot, x, y, 32, 32);
}

static void draw_party(void) {
    static const u8 hp[] = "HP", mp[] = "MP";
    GameData *g = g_gameState->gameData;
    u8 a[16];
    int y = r_party.y + 1;
    for (int i = 0; i < 4; i++, y += 31) {
        Character *c = g->party[i];
        if (!c) continue;
        portrait(i, g_portraitByKind[g->characters[c->info.b.id].info.b.kind], r_party.x + 3, y);
        text_drawBlock(r_party.x + 39, y + 1, 2, c->name, 0);
        ui_drawStatusIcons(r_party.x + 98, y + 2, 2, c->status, 0);
        int low = c->hp == 0 ? 3 : (c->hp <= c->maxHp / 6) * 4;
        ui_drawLabel(r_party.x + 39, y + 15, 2, (s32)hp, 0);
        text_formatNumber(c->hp, a, 4, 1);
        a[4] = '/';
        text_formatNumber(c->maxHp, a + 5, -1, 1);
        ui_drawSmallDigits(r_party.x + 54, y + 16, 2, a, low ? 2 : 0);
        ui_drawLabel(r_party.x + 39, y + 23, 2, (s32)mp, 0);
        text_formatNumber(c->mp, a, 4, 1);
        a[4] = '/';
        text_formatNumber(c->maxMp, a + 5, -1, 1);
        ui_drawSmallDigits(r_party.x + 54, y + 24, 2, a, c->mp < c->maxMp / 6 ? 2 : 0);
    }
    win_drawBox(&r_party, 3, 2);
}

/* the open question's choices (its window's current page, from g_msgChoiceLine on), as buttons in the middle area */
static int draw_choices(void) {
    n_choices = 0;
    if (g_msgChoiceLine < 0 || g_msgChoiceCount <= 0 || g_msgChoiceWindow < 0 || g_msgChoiceWindow >= 10) return 0;
    MsgWindow *w = &g_msgWindows[g_msgChoiceWindow];
    if (!w->open) return 0;
    u8 *t = msg_drawText_msg(w->msgId);
    if (!t) return 0;
    t += w->pageStart;
    /* skip to the first choice's line */
    for (int line = 0; line < g_msgChoiceLine && *t != TEXT_END; t++) {
        if (*t == TEXT_NEWLINE) line++;
        else if (*t == TEXT_ESC) t += t[1] == 0x43 || t[1] == 0x44 || t[1] == 0x49 || t[1] == 0x4A || (t[1] >= 0x4B && t[1] <= 0x4E) || (t[1] >= 0x68 && t[1] <= 0x6F) ? 2 : 1;
    }
    int n = g_msgChoiceCount > 8 ? 8 : g_msgChoiceCount;
    int h = n > 4 ? 22 : 30, gap = n > 4 ? 3 : 6, y0 = 40 + (128 - (n * h + (n - 1) * gap)) / 2;
    static u8 lines[8][96];
    for (int i = 0; i < n; i++) {
        u8 *o = lines[i];
        while (*t != TEXT_END && *t != TEXT_NEWLINE && o < lines[i] + 92) {
            if (*t == TEXT_ESC) { /* (an escape and its argument bytes, whole) */
                int k = t[1] == 0x43 || t[1] == 0x44 || t[1] == 0x49 || t[1] == 0x4A || (t[1] >= 0x4B && t[1] <= 0x4E) ||
                                (t[1] >= 0x68 && t[1] <= 0x6F) ? 3 : 2;
                while (k--) *o++ = *t++;
            } else *o++ = *t++;
        }
        *o = TEXT_END;
        if (*t == TEXT_NEWLINE) t++;
        RECT *r = &r_choice[i];
        r->x = 24; r->w = 272; r->h = h; r->y = y0 + i * (h + gap);
        int sel = i == g_msgChoice - 0;
        text_drawBlock(r->x + 12, r->y + (h - 12) / 2, 2, lines[i], sel ? 0 : 2);
        win_drawBox(r, 3, 2);
    }
    n_choices = n;
    choice_first = 0;
    return 1;
}

static int card_count(void) {
    int n = 0;
    for (int i = 0; i < 105; i++) n += g_cards.cards[i].id != 0xFF;
    return n;
}

static void button(RECT *r, const u8 *label, const u8 *count) {
    int w = ui_measureLabel((u8 *)label);
    int x = r->x + (r->w - w - (count ? 24 : 0)) / 2;
    ui_drawLabel(x, r->y + 15, 2, (s32)label, 0);
    if (count) ui_drawSmallDigits(x + w + 6, r->y + 16, 2, (u8 *)count, 0);
    win_drawBox(r, 3, 2);
}

u8 *port_field_map_draw(u32 *ot, u8 *pkt, u8 *pkt_end, int bx, int by, int bw, int bh);

static void draw(void) {
    static const u8 menu[] = "MENU", cards[] = "CARDS", party[] = "PARTY", map[] = "MAP", gil_suffix[] = "G";
    GameData *g = g_gameState->gameData;
    u8 buf[16];

    /* the header: where, and since when and with what */
    if (g->locationName != NULL && *g->locationName != 0xFF) text_drawBlock(r_head.x + 8, r_head.y + 6, 2, g->locationName, 0);
    ui_drawSprite(r_head.x + 190, r_head.y + 8, 2, 0xB1, 0);
    ui_drawSmallDigits(r_head.x + 203, r_head.y + 9, 2, buf, text_formatTime(game_getPlayTime(), buf, 1));
    ui_drawSprite(r_head.x + 246, r_head.y + 8, 2, 0xB2, 0);
    text_formatNumber(g->gil, buf, 7, 1);
    strcat((char *)buf, (const char *)gil_suffix);
    ui_drawSmallDigits(r_head.x + 259, r_head.y + 9, 2, buf, 0);
    win_drawBox(&r_head, 3, 2);

    if (!draw_choices()) {
        ui_drawLabel(r_map.x + 8, r_map.y - 3, 2, (s32)map, 0);
        g_primPtr = port_field_map_draw(&g_ot[2], g_primPtr, (u8 *)(hud_mem + OTN + BUF_WORDS) - 64, r_map.x + 3,
                                        r_map.y + 3, r_map.w - 6, r_map.h - 6);
        win_drawBox(&r_map, 3, 2);
        ui_drawLabel(r_party.x + 8, r_party.y - 3, 2, (s32)party, 0);
        draw_party();
    }

    button(&r_menu, menu, NULL);
    text_formatNumber(card_count(), buf, -1, 1);
    button(&r_cards, cards, buf);
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
