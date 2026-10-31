/* The bottom screen in a field (docs/ui-design.md): where you are and how the party is, drawn by the game's own
 * window and text code (its frames, font, labels and digits) into a packet buffer of the port's, which the 3DS
 * renderer draws as a UI list on the bottom screen. Nothing here is needed while walking: speech and prompts stay on
 * the top screen with the speakers.
 *
 *   header:  the location, play time, gil         (GameData.locationName; as the main menu shows them)
 *   MAP:     the walkmesh from above, the player  (port/game/field_map.c)
 *   PARTY:   portrait, name, HP and MP, status    (GameData.party; portraits: the menu's file, read by the port)
 *   [MENU] [CARDS n] [ATE] [LOG]                    MENU: a touch shortcut for triangle (port_hud_tap); LOG: the
 *                                                  dialogue log in place of the map and party (tap: scroll)
 * The party and MENU only while the game allows the menu (not in the opening and other cutscene stretches, where the
 * party may be a placeholder); CARDS once the card game has been met.
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

#define OTN 12 /* (entries 0-10 for drawing: the game's list rows draw at 7, their boxes behind at 9) */
#define BUF_WORDS 0x3000
/* the ordering table and the packets in one array: links are 24-bit, so they share their high byte */
static u32 hud_mem[OTN + BUF_WORDS];
static u32 *const hud_ot = hud_mem;

/* the layout: 6 px inside every box, 4 px between them, labels on the boxes' top edges (as the PS1's) */
static RECT r_head = {4, 4, 312, 24};
static RECT r_map = {4, 34, 168, 152};
static RECT r_party = {176, 34, 140, 152};
static RECT r_menu = {4, 190, 75, 30};
static RECT r_cards = {83, 190, 75, 30};
static RECT r_ate = {162, 190, 75, 30};
static RECT r_log = {241, 190, 75, 30};
static RECT r_logbox = {4, 34, 312, 152};

static int shown;

/* game pointers the bottom screen follows, checked to point into PS1 RAM (a field starting up may hold garbage: a real
 * 3DS doesn't zero memory as an emulator does) */
extern unsigned char psx_ram[];
static int ram_ok(const void *p, unsigned size) {
    const unsigned char *b = (const unsigned char *)p;
    return b >= psx_ram + 0x10000 && b + size <= psx_ram + 0x200000;
}

/* whether the field screen is up (and so its buttons are there to touch) */
int port_hud_shown(void) {
    return shown;
}

static int menu_shown; /* the MENU button is there (the game allows the menu) */
static int log_head, log_count, log_scroll, log_on; /* the dialogue log (port_dialogue_log) */
static RECT *cards_rect; /* where CARDS is this frame (NULL: not shown) */
static int ate_shown;
static int cards_pending; /* frames left to wait for the main menu to open onto the card screen */

/* an Active Time Event can be watched now (the field script's byte b[0xEE]: decomp-requests R30) */
static int ate_available(void) {
    return *PSX_RAM(volatile u8 *, 0x8007AFDE) == 1;
}

/* CARDS: the main menu, then its card screen (once the menu runs: the cursor on Card, the slide-in over, the screen
 * opened as the menu would; decomp-requests R30, ff9-decomp r32) */
static void cards_step(void) {
    if (!cards_pending) return;
    cards_pending--;
    if (g_menuScreen.active != 1 || g_menuScreen.screen != 0) return;
    *PSX_RAM(u8 *, 0x800CB188) = 5; /* (s_menuMain.cursor: row 5 is Card) */
    g_menuScreen.slideIn = 0;
    menuScreen_open(6);
    cards_pending = 0;
}
static int n_choices, choice_first; /* the question's choices shown this frame */
static RECT r_choice[8];

static int inside(const RECT *r, int x, int y) { return x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h; }

/* A tap at (x, y) of the bottom screen (its UI starts oy rows down): the PS1 presses it stands for, into seq (at most
 * max), how many; 0: none here (the touch driver's own handling then); -1: taken, nothing to press now. */
int port_battle_tap(int x, int y, unsigned *seq, int max);
unsigned port_battle_drive(void);
static int shown_battle;
/* each idle frame of the touch driver: a press the bottom screen steers with (battle targets), 0 none */
unsigned port_hud_drive(void) {
    return shown_battle ? port_battle_drive() : 0;
}
__attribute__((weak)) int port_world_tap(void) { return 0; }
int port_hud_tap(int x, int y, int oy, unsigned *seq, int max) {
    if (g_gameState && g_gameState->mode == 3 && !port_menu_state()) return port_world_tap() ? -1 : 0; /* (zoom) */
    if (!shown) return 0;
    y -= oy;
    if (shown_battle) return port_battle_tap(x, y, seq, max);
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
    if (menu_shown && inside(&r_menu, x, y) && max > 0) { seq[0] = 0x1000; return 1; } /* triangle */
    if (menu_shown && cards_rect && inside(cards_rect, x, y) && max > 0) {
        cards_pending = 90;
        seq[0] = 0x1000; /* (the menu, then the card screen: cards_step) */
        return 1;
    }
    if (ate_shown && inside(&r_ate, x, y) && max > 0) { seq[0] = 0x0001; return 1; } /* select */
    if (inside(&r_log, x, y)) { log_on = !log_on; log_scroll = 0; return -1; }
    if (log_on && inside(&r_logbox, x, y)) { /* (the upper half: older, the lower: newer) */
        if (y < r_logbox.y + r_logbox.h / 2) { if (log_scroll < log_count - 1) log_scroll++; }
        else if (log_scroll > 0) log_scroll--;
        return -1;
    }
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

/* the menu's portrait entry index (its CLUT, then the 32x32 face), NULL if the file can't be read */
const u8 *port_portrait(int index) {
    if (portraits_state == 0) {
        FileEntry *f = file_find(1, 30);
        unsigned n = f ? (unsigned)(f->unkC - f->sector) : 0;
        portraits_state = -1;
        if (n && n * 2048 <= sizeof portraits) {
            psx_cd_read_data((unsigned)(g_gameState->imgSector + f->sector), portraits, n);
            portraits_state = 1;
        }
    }
    if (portraits_state < 0 || index < 0 || (unsigned)(index + 1) * PORTRAIT_BYTES > sizeof portraits) return NULL;
    return portraits + index * PORTRAIT_BYTES;
}

/* the portrait's face, 32x32: its rows 7-38 (the 32x46 portrait is the entry's 32x32 block and, under it, its 32x14
 * strip; the face sits in the middle), as 8-bit pixels; NULL if unread */
const u8 *port_portrait_face(int index) {
    static u8 face[32 * 32];
    const u8 *e = port_portrait(index);
    if (!e) return NULL;
    for (int y = 0; y < 32; y++) {
        int sy = y + 7;
        __builtin_memcpy(face + y * 32, sy < 32 ? e + 0x100 + sy * 32 : e + 0x500 + (sy - 32) * 32, 32);
    }
    return face;
}

static void portrait(int slot, int index, int x, int y) {
    const u8 *e = port_portrait(index);
    if (!e) return;
    const u8 *f = port_portrait_face(index);
    ctr_hud_image(slot, 0x100u | (u32)index, (const u16 *)e, f, 32, 32);
    ctr_hud_draw(slot, x, y, 32, 32);
}

static void draw_party(void) {
    static const u8 hp[] = "HP", mp[] = "MP";
    GameData *g = g_gameState->gameData;
    u8 a[16];
    int y = r_party.y + 6;
    for (int i = 0; i < 4; i++, y += 36) { /* (a row: the 32x32 face, then name, HP, MP; 4 px between rows) */
        Character *c = g->party[i];
        if (!ram_ok(c, sizeof *c)) continue;
        int x = r_party.x + 6, tx = x + 38;
        portrait(i, g_portraitByKind[g->characters[c->info.b.id].info.b.kind], x, y);
        text_drawBlock(tx, y, 2, c->name, 0);
        ui_drawStatusIcons(r_party.x + r_party.w - 30, y + 1, 2, c->status, 0);
        int low = c->hp == 0 ? 3 : (c->hp <= c->maxHp / 6) * 4;
        ui_drawLabel(tx, y + 14, 2, (s32)hp, 0);
        text_formatNumber(c->hp, a, 4, 1);
        a[4] = '/';
        text_formatNumber(c->maxHp, a + 5, -1, 1);
        ui_drawSmallDigits(tx + 14, y + 15, 2, a, low ? 2 : 0);
        ui_drawLabel(tx, y + 23, 2, (s32)mp, 0);
        text_formatNumber(c->mp, a, 4, 1);
        a[4] = '/';
        text_formatNumber(c->maxMp, a + 5, -1, 1);
        ui_drawSmallDigits(tx + 14, y + 24, 2, a, c->mp < c->maxMp / 6 ? 2 : 0);
    }
    win_drawBox(&r_party, 3, 2);
}

/* Text the message system shows, as text_drawBlock draws it: printable codes and newlines kept, with the escapes that
 * change what is drawn (names, colours, argument values); layout, timing, choice and page codes dropped. To the first
 * TEXT_END or stop byte (stop: TEXT_NEWLINE for one line, TEXT_END for all), at most max - 1 bytes; returns where
 * it stopped in src. */
/* the argument bytes after the message system's escape F9 c (msg.c's text stepping) */
static int esc_args(u8 c) {
    switch (c) {
    case 0x06: case 0x07: case 0x48: return 2;
    case 0x00: case 0x01: case 0x02: case 0x0E: case 0x0F: case 0x40: case 0x41: case 0x43: case 0x44: case 0x49:
    case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x70: return 1;
    default: return c >= 0x68 && c <= 0x6F ? 1 : 0;
    }
}

static const u8 *msg_plain(const u8 *t, u8 *o, int max, u8 stop) {
    u8 *start = o;
    while (*t != TEXT_END && *t != stop && o < start + max - 5) {
        if (*t == TEXT_ESC) {
            u8 c = t[1];
            int n = 2 + esc_args(c);
            /* drawn: names, colours, icons, button icons, argument values; the rest is timing, layout and choices */
            int keep = (c >= 0x10 && c <= 0x3F) || (c >= 0x60 && c <= 0x6F);
            while (n--) { if (keep) *o++ = *t; t++; }
        } else if (*t >= 0xFA && *t <= 0xFD) { /* (a two-byte character) */
            *o++ = *t++;
            *o++ = *t++;
        } else if (*t == 0xFE) { /* (a page break: a new line here) */
            *o++ = TEXT_NEWLINE;
            t++;
        } else *o++ = *t++;
    }
    *o = TEXT_END;
    return t;
}

/* ---- the dialogue log: every line shown (msg_open, voice_hook.c), newest last ---- */
#define LOG_LINES 24
static u8 log_text[LOG_LINES][320];
void port_dialogue_log(const u8 *text) {
    if (!text) return;
    u8 *o = log_text[log_head];
    msg_plain(text, o, sizeof log_text[0], TEXT_END);
    if (o[0] == TEXT_END) return;
    if (log_count && !__builtin_memcmp(o, log_text[(log_head + LOG_LINES - 1) % LOG_LINES], sizeof log_text[0])) return;
    log_head = (log_head + 1) % LOG_LINES;
    if (log_count < LOG_LINES) log_count++;
    log_scroll = 0;
}

/* the log in the middle area, newest at the bottom (log_scroll lines back) */
static void draw_log(void) {
    static const u8 label[] = "LOG";
    RECT box = r_logbox;
    ui_drawLabel(box.x + 8, box.y - 3, 2, (s32)label, 0);
    int y = box.y + box.h - 6;
    for (int i = log_scroll; i < log_count; i++) {
        u8 *t = log_text[(log_head + LOG_LINES - 1 - i) % LOG_LINES];
        TextSize sz = text_measure(t);
        int h = sz.h > 0 ? sz.h : 14;
        if (y - h < box.y + 4) break;
        y -= h + 4;
        text_drawBlock(box.x + 8, y, 2, t, i ? 2 : 0);
    }
    win_drawBox(&box, 3, 2);
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
    int h = n > 4 ? 24 : 32, gap = n > 4 ? 3 : 6, y0 = 34 + (152 - (n * h + (n - 1) * gap)) / 2;
    static u8 lines[8][96];
    for (int i = 0; i < n; i++) {
        t = (u8 *)msg_plain(t, lines[i], sizeof lines[i], TEXT_NEWLINE);
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

/* what the PS1 shows only through its menu is shown only when the menu can be opened: in the opening and other
 * cutscene stretches (GD_MENU_DISABLED) the party may be a placeholder, and there is no menu to offer */
static int menu_allowed(void) {
    /* once the player has had control in this field (the opening's party is a placeholder until then; a dialogue
     * takes control away for a moment, which mustn't hide anything) */
    static int field = -1, had_control;
    GameData *g = g_gameState->gameData;
    if (g->fieldId != field) field = g->fieldId, had_control = 0;
    if (ram_ok(g_evtState, sizeof *g_evtState) && g_evtState->controlEnabled) had_control = 1;
    return had_control && !(g->flags & GD_MENU_DISABLED);
}

static int card_count(void) {
    int n = 0;
    for (int i = 0; i < 105; i++) n += g_cards.cards[i].id != 0xFF;
    return n;
}

/* the card game has been met: a card owned or a match played */
static int cards_known(void) {
    return card_count() > 0 || g_cards.wins || g_cards.losses || g_cards.draws;
}

static void button(RECT *r, const u8 *label, const u8 *count) {
    int w = ui_measureLabel((u8 *)label);
    int x = r->x + (r->w - w - (count ? 24 : 0)) / 2;
    ui_drawLabel(x, r->y + (r->h - 6) / 2, 2, (s32)label, 0);
    if (count) ui_drawSmallDigits(x + w + 6, r->y + (r->h - 6) / 2 + 1, 2, (u8 *)count, 0);
    win_drawBox(r, 3, 2);
}

u8 *port_field_map_draw(u32 *ot, u8 *pkt, u8 *pkt_end, int bx, int by, int bw, int bh);

static void draw(void) {
    static const u8 menu[] = "MENU", cards[] = "CARDS", party[] = "PARTY", map[] = "MAP", gil_suffix[] = "G";
    GameData *g = g_gameState->gameData;
    u8 buf[16];

    /* the header: where, and since when and with what */
    if (ram_ok(g->locationName, 1) && *g->locationName != 0xFF) text_drawBlock(r_head.x + 8, r_head.y + 6, 2, g->locationName, 0);
    ui_drawSprite(r_head.x + 178, r_head.y + 8, 2, 0xB1, 0);
    ui_drawSmallDigits(r_head.x + 191, r_head.y + 9, 2, buf, text_formatTime(game_getPlayTime(), buf, 1));
    ui_drawSprite(r_head.x + 236, r_head.y + 8, 2, 0xB2, 0);
    text_formatNumber(g->gil, buf, 7, 1);
    strcat((char *)buf, (const char *)gil_suffix);
    ui_drawSmallDigits(r_head.x + 249, r_head.y + 9, 2, buf, 0);
    win_drawBox(&r_head, 3, 2);

    if (!draw_choices() && log_on) draw_log();
    else if (!n_choices) {
        ui_drawLabel(r_map.x + 8, r_map.y - 3, 2, (s32)map, 0);
        g_primPtr = port_field_map_draw(&g_ot[2], g_primPtr, (u8 *)(hud_mem + OTN + BUF_WORDS) - 64, r_map.x + 3,
                                        r_map.y + 3, r_map.w - 6, r_map.h - 6);
        win_drawBox(&r_map, 3, 2);
        if (menu_allowed()) {
            ui_drawLabel(r_party.x + 8, r_party.y - 3, 2, (s32)party, 0);
            draw_party();
        }
    }

    menu_shown = menu_allowed();
    if (menu_shown) button(&r_menu, menu, NULL);
    cards_rect = NULL;
    if (cards_known() && menu_shown) {
        text_formatNumber(card_count(), buf, -1, 1);
        button(cards_rect = &r_cards, cards, buf);
    }
    {
        static const u8 logl[] = "LOG", mapl[] = "MAP";
        button(&r_log, log_on ? mapl : logl, NULL);
    }
    ate_shown = ate_available();
    if (ate_shown) { /* (pulsing, as the PS1's prompt) */
        static const u8 ate[] = "ATE";
        static int t;
        t++;
        int w = ui_measureLabel((u8 *)ate);
        ui_drawLabel(r_ate.x + (r_ate.w - w) / 2, r_ate.y + (r_ate.h - 6) / 2, 2, (s32)ate, (t / 22) & 1 ? 2 : 0);
        win_drawBox(&r_ate, 3, 2);
    }
}

/* each frame (the 3DS's VBlank wait): the field screen, when a field is up with no menu; in a battle, the battle
 * screen's own part (port/game/battle_ui.c) */
int port_battle_hud_draw(void);
void psx_ui_list_begin_keep(void);
extern int port_battle_hud_on;
static void top_hud(void);
void port_bottom_hud(void) {
    shown = shown_battle = 0;
    if (g_gameState && g_gameState->gameData) cards_step();
    if (g_gameState && g_gameState->gameData && g_gameState->gameData->font) top_hud();
    if (!g_gameState || !g_gameState->gameData || port_menu_state()) return;
    int battle = g_gameState->mode == 2;
    if (!battle) port_battle_hud_on = 0;
    if (g_gameState->mode != 1 && !battle) return;
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
    int drew = 1;
    if (battle) drew = port_battle_hud_draw(); /* (battle_ui.c: over the battle's own UI list) */
    else draw();
    unsigned used = (unsigned)(g_primPtr - (u8 *)hud_mem);
    g_ot = saved_ot;
    g_primPtr = saved_prim;
    if (used > sizeof hud_mem) port_fatal("field HUD: packet buffer overrun");
    if (!drew) return;

    Gp0State saved_gp0 = gp0_state; /* (the field's drawing environment is the field's again after) */
    if (battle) psx_ui_list_begin_keep();
    else psx_ui_list_begin();
    port_draw_ot_buffer(&hud_ot[OTN - 1], hud_mem, sizeof hud_mem);
    psx_ui_list_end();
    gp0_state = saved_gp0;
    shown = 1;
    shown_battle = battle;
}

/* The PS1 button icons the game draws (g_uiSprites: Cross 4, Circle 5, Square 6, Triangle 7, L1 8, L2 9, R1 10, R2 11),
 * for the renderer to draw the 3DS's buttons in their place (port/3ds/render.c): per 3DS button A, B, X, Y, L, R, ZL,
 * ZR, the sprite's texture page, CLUT, uv and size as {tx, ty, clut_x, clut_y, u, v, w, h}. */
int port_button_sprites(int out[8][8]) {
    static const u8 ids[8] = {4, 5, 7, 6, 8, 10, 9, 11}; /* (A = cross, B = circle, X = triangle, Y = square, ...) */
    for (int k = 0; k < 8; k++) {
        const SpriteDef *d = &g_uiSprites[ids[k]];
        u32 clut = d->uvclut >> 16;
        out[k][0] = (d->tpage & 0xF) * 64;
        out[k][1] = (d->tpage >> 4 & 1) * 256;
        out[k][2] = (int)(clut & 0x3F) * 16;
        out[k][3] = (int)(clut >> 6 & 0x1FF);
        out[k][4] = (int)(d->uvclut & 0xFF);
        out[k][5] = (int)(d->uvclut >> 8 & 0xFF);
        out[k][6] = d->w;
        out[k][7] = d->h;
    }
    return 8;
}

/* ---- the top screen's detail views (docs/ui-design.md), drawn with the game's window and text code like the bottom
 * screen, onto the top screen over its scene (port/3ds/render.c ctr_top_ui) ------------------------------------------
 * The main menu: the party at a glance; in Status, Equip or Abilities, the chosen member's sheet (a large portrait,
 * level, HP, MP, EXP, the eight stats, the equipment). The game's logic pauses as on the PS1. */
__attribute__((weak)) void ctr_top_ui(unsigned *head, const void *buf, unsigned size) { (void)head; (void)buf; (void)size; }
__attribute__((weak)) void ctr_hud_draw_top(int slot, int x, int y, int w, int h, int dw, int dh) {
    (void)slot; (void)x; (void)y; (void)w; (void)h; (void)dw; (void)dh;
}
#define TOP_WORDS 0x2000
static u32 top_mem[OTN + TOP_WORDS];

static void big_portrait(int slot, Character *c, int x, int y, int size) {
    const u8 *e = port_portrait(g_portraitByKind[c->info.b.kind]);
    if (!e) return;
    ctr_hud_image(slot, 0x300u | (u32)g_portraitByKind[c->info.b.kind], (const u16 *)e,
                  port_portrait_face(g_portraitByKind[c->info.b.kind]), 32, 32);
    ctr_hud_draw_top(slot, x, y, 32, 32, size, size);
}

static void stat(int x, int y, const char *label, int value) {
    u8 a[12];
    ui_drawLabel(x, y, 2, (s32)label, 0);
    text_formatNumber(value, a, 4, 1);
    ui_drawDigits(x + 72, y - 1, 2, a, 0);
}

static void hp_mp(int x, int y, Character *c) {
    static const u8 hp[] = "HP", mp[] = "MP";
    u8 a[16];
    ui_drawLabel(x, y, 2, (s32)hp, 0);
    text_formatNumber(c->hp, a, 4, 1);
    a[4] = '/';
    text_formatNumber(c->maxHp, a + 5, -1, 1);
    ui_drawDigits(x + 16, y - 1, 2, a, c->hp <= c->maxHp / 6 ? 4 : 0);
    ui_drawLabel(x, y + 13, 2, (s32)mp, 0);
    text_formatNumber(c->mp, a, 4, 1);
    a[4] = '/';
    text_formatNumber(c->maxMp, a + 5, -1, 1);
    ui_drawDigits(x + 16, y + 12, 2, a, 0);
}

__attribute__((unused)) static void member_sheet(Character *c) {
    static const u8 lv[] = "LV", exp[] = "EXP", stats[] = "STATUS", equip[] = "EQUIPMENT";
    static RECT box = {8, 8, 384, 224}, rs = {150, 92, 234, 76}, re = {16, 92, 128, 132};
    u8 a[12];
    big_portrait(4, c, 18, 18, 64);
    text_drawBlock(96, 20, 2, c->name, 0);
    ui_drawLabel(220, 22, 2, (s32)lv, 0);
    text_formatNumber(c->level, a, 2, 1);
    ui_drawDigits(236, 21, 2, a, 0);
    hp_mp(96, 42, c);
    ui_drawLabel(96, 70, 2, (s32)exp, 0);
    text_formatNumber((s32)c->exp, a, 7, 1);
    ui_drawDigits(124, 69, 2, a, 0);
    ui_drawLabel(rs.x + 6, rs.y - 3, 2, (s32)stats, 0);
    stat(rs.x + 10, rs.y + 10, "SPEED", c->speed);
    stat(rs.x + 10, rs.y + 26, "STRENGTH", c->strength);
    stat(rs.x + 10, rs.y + 42, "MAGIC", c->magic);
    stat(rs.x + 10, rs.y + 58, "SPIRIT", c->spirit);
    stat(rs.x + 122, rs.y + 10, "DEFENSE", c->defense);
    stat(rs.x + 122, rs.y + 26, "EVADE", c->evade);
    stat(rs.x + 122, rs.y + 42, "MAGIC DEF", c->magicDef);
    stat(rs.x + 122, rs.y + 58, "MAGIC EVA", c->magicEva);
    win_drawBox(&rs, 4, 2);
    ui_drawLabel(re.x + 6, re.y - 3, 2, (s32)equip, 0);
    for (int i = 0; i < 5; i++) {
        int id = c->equip[i];
        if (id == 0xFF) continue;
        ui_drawItemIcon(re.x + 6, re.y + 8 + i * 24, 2, id, 1);
        text_drawBlock(re.x + 22, re.y + 8 + i * 24, 2, g_itemNames + g_itemTable[id].name, 0);
    }
    win_drawBox(&re, 4, 2);
    win_drawBox(&box, 5, 2);
}

__attribute__((unused)) static void party_overview(void) {
    static const u8 lv[] = "LV";
    GameData *g = g_gameState->gameData;
    u8 a[12];
    for (int i = 0; i < 4; i++) {
        Character *c = g->party[i];
        if (!ram_ok(c, sizeof *c)) continue;
        RECT r = {(s16)(8 + (i & 1) * 194), (s16)(8 + (i >> 1) * 114), 190, 110};
        big_portrait(4 + i, c, r.x + 10, r.y + 10, 48);
        text_drawBlock(r.x + 66, r.y + 12, 2, c->name, 0);
        ui_drawLabel(r.x + 66, r.y + 32, 2, (s32)lv, 0);
        text_formatNumber(c->level, a, 2, 1);
        ui_drawDigits(r.x + 82, r.y + 31, 2, a, 0);
        hp_mp(r.x + 66, r.y + 52, c);
        win_drawBox(&r, 4, 2);
    }
}

/* What the top screen shows while a menu is up: the main menu keeps the paused scene (the bottom has the party); a
 * sub-screen shows what the bottom can't, about what is highlighted there (decomp-requests R31: pending; the member
 * sheet and the overview below are ready for it). */
static int top_draw(void) {
    if (port_menu_state() != 2 || !g_menuScreen.active) return 0;
    (void)member_sheet;
    (void)party_overview;
    return 0;
}

static void top_hud(void) {
    u32 *ot = top_mem;
    ClearOTagR(ot, OTN);
    u32 *saved_ot = g_ot;
    u8 *saved_prim = g_primPtr;
    u32 *p = top_mem + OTN;
    p[0] = 3u << 24; /* (the top screen's 400x240 from 0, 0) */
    p[1] = 0xE3000000u;
    p[2] = 0xE4000000u | 399u | 239u << 10;
    p[3] = 0xE5000000u;
    addPrim(&ot[OTN - 1], p);
    g_ot = ot;
    g_primPtr = (u8 *)(p + 4);
    int drew = top_draw();
    unsigned used = (unsigned)(g_primPtr - (u8 *)top_mem);
    g_ot = saved_ot;
    g_primPtr = saved_prim;
    if (used > sizeof top_mem) port_fatal("top screen UI: packet buffer overrun");
    if (drew) ctr_top_ui((unsigned *)&ot[OTN - 1], top_mem, sizeof top_mem);
}
