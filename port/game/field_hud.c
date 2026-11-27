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
 * The party once the player has had control in the field (before that, as in the opening, it may be a placeholder);
 * MENU only while the game allows the menu; CARDS once the card game has been met (and the menu is allowed).
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
#include "port_textcode.h"
#include "port_buttons.h"
#include "port_text.h"
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
/* (the 3DS's renderer: its UI layer's state; the PC draws every time) */
void psx_gpu_sync(void); /* (gpu.c) */
__attribute__((weak)) unsigned ctr_ui_layer_serial(void) { return ~0u; }
__attribute__((weak)) void ctr_ui_layer_keep(void) {}

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
    return b >= psx_ram + 0x10000 && b <= psx_ram + 0x200000 && size <= (unsigned)(psx_ram + 0x200000 - b); /* (b + size
     * could wrap: a pointer of -1 (field 1756's battle UI pointer, outside a battle) passed, and faulted at 0x12B4) */
}

/* whether the field screen is up (and so its buttons are there to touch) */
int port_hud_shown(void) {
    return shown;
}

static int menu_shown; /* the MENU button is there (the game allows the menu) */
static int log_head, log_count, log_scroll, log_on; /* the dialogue log (port_dialogue_log) */
static int opts_on; /* the options page in the log's place */
static RECT r_optbtn = {226, 38, 84, 20};
static int options_tap(int x, int y);
static RECT *cards_rect; /* where CARDS is this frame (NULL: not shown) */
static int ate_shown;
static int cards_pending; /* frames left to wait for the main menu to open onto the card screen */

/* an Active Time Event can be watched now (the field script's byte b[0xEE]: decomp-requests R30). (By symbol: a PS1
 * address isn't one in port code; an emulator reads unmapped memory as 0, a 3DS faults.) */
/* the PS1's "PRESS [SELECT] TO SEE ATE" prompt is up (the field's init sets it; a field change turns it off) and the
 * Select handler is armed. Gs8[0xEE] alone outlives the field: a field with no ATE menu (611 L. Castle/Guest Room)
 * keeps the town's 1 and no handler */
static int ate_available(void) {
    return g_atePromptMode != ATE_PROMPT_OFF && (g_atePromptMode & ATE_PROMPT_STYLE_MASK) != ATE_PROMPT_BLINK &&
           g_evtGlobalVars.b[0xEE] == 1;
}

/* CARDS: the main menu, then its card screen (once the menu runs: the cursor on Card, the slide-in over, the screen
 * opened as the menu would; decomp-requests R30, ff9-decomp r32) */
static void cards_step(void) {
    if (!cards_pending) return;
    cards_pending--;
    if (g_menuScreen.active != 1 || g_menuScreen.screen != 0) return;
    extern u8 o0a_s_menuMain[]; /* (the field's main menu: its cursor first; row 5 is Card) */
    o0a_s_menuMain[0] = 5;
    g_menuScreen.slideIn = 0;
    menuScreen_open(6);
    cards_pending = 0;
}
static int n_choices, choice_first; /* the question's choices shown this frame */
#define MAX_CHOICES 12
static RECT r_choice[MAX_CHOICES];

/* A choice the script answers itself (the Moogle's menus, ff9-decomp R34: it polls msg_getPadPressed): the tap's
 * Cross goes into g_msgPadPressed after msg_update and before the objects run (evt_updateObjects, wrapped:
 * config/renames.txt). */
static int choice_cross;
static int choice_open(void);
s32 evt_updateObjects_game(void);
s32 evt_updateObjects(void) {
    void port_scenario_object_step(void), port_auction_step(void);
    port_scenario_object_step(); /* (scenario.c: a fixture's placing and talking) */
    port_auction_step(); /* (auction_ui.c: a tapped bid) */
    if (choice_cross) choice_cross = 0, g_msgPadPressed |= BTN_CONFIRM; /* (Cross; Circle in JP) */
    return evt_updateObjects_game();
}

/* the main menu opened as the field's triangle opens it (fld_main.c), directly; 0 if the field allows none now */
static int field_open_menu(void) {
    GameData *g = g_gameState->gameData;
    if (g_gameState->mode != 1 || (g->flags & (GD_PAUSED | GD_MENU_DISABLED | GD_MENU_OPEN))) return 0;
    g->flags |= GD_MENU_OPEN;
    menu_open(MENU_MAIN, 0);
    evt_onMenuOpen();
    return 1;
}

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
__attribute__((weak)) int port_world_tap(int x, int y) { (void)x; (void)y; return 0; }
int port_name_tap(int x, int y, unsigned *seq, int max);
static int title_tap(int x, int y);
static int world_choices; /* the world map's question up as buttons (port_bottom_hud) */
static int boot_live; /* the boot card shown (port_bottom_hud) */
static int gameover_live; /* the game over card shown */
static int bj_live; /* Blackjack's buttons shown (blackjack_ui.c) */
static int auction_live; /* the Treno auction's panel shown (auction_ui.c) */
static RECT r_disc[2];
int port_disc_prompt_live(void);
void port_disc_prompt_set(int c);
static int config_open(void);
static int config_overlay_tap(int x, int y);
int port_title_live(void);
int port_hud_tap(int x, int y, int oy, unsigned *seq, int max) {
    if (g_gameState && g_gameState->mode == 0 && port_title_live()) return title_tap(x, y) ? -1 : 0; /* (title_ui.c) */
    if (g_gameState && g_gameState->mode == 0 && boot_live && max > 0) { /* (START, held 8 frames more: the opening's
                                                                          * movie reads the pad every 4th VBlank) */
        seq[0] = 0x0008 | 8u << 24;
        return 1;
    }
    if (g_gameState && gameover_live && max > 0) { seq[0] = PORT_PAD_CONFIRM; return 1; } /* (Cross; a field's or a battle's) */
    if (g_gameState && g_gameState->mode == 4 && bj_live) { int port_bj_tap(int, int); port_bj_tap(x, y - oy); return -1; }
    if (g_gameState && g_gameState->mode == 1 && auction_live) { int port_auction_tap(int, int); if (port_auction_tap(x, y - oy)) return -1; }
    if (g_gameState && config_open() && config_overlay_tap(x, y)) return -1; /* (the Config screen's 3DS OPTIONS) */
    { int n = port_name_tap(x, y - oy, seq, max); if (n) return n; } /* (name entry: port/game/name_touch.c) */
    { int port_menu_tap(int, int); if (port_menu_tap(x, y - oy)) return -1; } /* (main menu: menu_touch.c) */
    { int port_tm_tap(int, int); if (port_tm_tap(x, y - oy)) return -1; } /* (the card game: tm_ui.c) */
    if (port_menu_state() == 2 && max > 0) { /* (a menu's list: list_touch.c) */
        int port_list_tap(int, int, unsigned *), port_equip_tap(int, int, unsigned *), port_save_tap(int, int, unsigned *);
        int port_ability_tap(int, int, unsigned *);
        int port_shop_tap(int, int, unsigned *);
        int n = port_shop_tap(x, y - oy, seq);
        if (!n) n = port_save_tap(x, y - oy, seq);
        if (!n) n = port_equip_tap(x, y - oy, seq);
        if (!n) n = port_ability_tap(x, y - oy, seq);
        if (!n) { int port_config_tap(int, int, unsigned *); n = port_config_tap(x, y - oy, seq); }
        if (!n) { int port_status_tap(int, int, unsigned *); n = port_status_tap(x, y - oy, seq); }
        if (!n) { int port_card_tap(int, int); n = port_card_tap(x, y - oy); }
        if (!n) { int port_party_tap(int, int, unsigned *); n = port_party_tap(x, y - oy, seq); }
        if (!n) { int port_chocograph_tap(int, int, unsigned *); n = port_chocograph_tap(x, y - oy, seq); }
        if (!n) n = port_list_tap(x, y - oy, seq);
        if (n) return n;
    }
    { /* the world map's full map (the airship's Go to): its question is on the bottom screen 1:1; a tap on one of its
       * choice lines (14 pixels apart from the window's first line) answers it directly, as a field's buttons do */
        extern s32 g_worldNaviMode;
        if (g_gameState && g_gameState->mode == 3 && g_worldNaviMode == 2 && choice_open()) {
            MsgWindow *w = NULL;
            for (int k = 0; k < 10; k++)
                if (g_msgWindows[k].open && g_msgWindows[k].id == g_msgChoiceWindow) w = &g_msgWindows[k];
            if (!w || !w->window) return -1;
            RECT *r = &w->window->cur;
            int yy = y - oy;
            for (int i = 0; i < g_msgChoiceCount; i++) {
                int cy = r->y + 10 + (g_msgChoiceLine + i) * 14;
                if (x < r->x || x >= r->x + r->w || yy < cy - 7 || yy >= cy + 7) continue;
                g_msgChoice = i;
                if (w->waitFrames != MSGW_WAIT_INPUT || (w->flags & MSGW_NO_INPUT)) { /* (the world's script polls Cross) */
                    if (max > 0) { seq[0] = PORT_PAD_CONFIRM; return 1; }
                    return -1;
                }
                w->waitFrames = 1;
                g_gameState->gameData->soundHook(SND_SE_PLAY, SE_CURSOR, 0x800000, SND_PAN_CENTER, SND_VOL_MAX);
            }
            return -1;
        }
    }
    if (g_gameState && g_gameState->mode == 3 && !port_menu_state() && !world_choices) { /* (zoom, names) */
        return port_world_tap(x, y - oy) ? -1 : 0;
    }
    if (!shown) return 0;
    y -= oy;
    if (shown_battle) return port_battle_tap(x, y, seq, max);
    if (port_disc_prompt_live()) { /* (the end of a disc's save prompt: its cursor, then its own Cross) */
        for (int i = 0; i < 2; i++)
            if (inside(&r_disc[i], x, y) && max > 0) { port_disc_prompt_set(i); seq[0] = PORT_PAD_CONFIRM; return 1; }
        return -1;
    }
    if (n_choices) { /* (a choice, directly: the game's choice set and its window answered, as msg_input does on cross) */
        for (int i = 0; i < n_choices; i++) {
            if (!inside(&r_choice[i], x, y)) continue;
            MsgWindow *w = NULL;
            for (int k = 0; k < 10; k++)
                if (g_msgWindows[k].open && g_msgWindows[k].id == g_msgChoiceWindow) w = &g_msgWindows[k];
            if (!w || g_msgChoiceLine < 0) return -1;
            g_msgChoice = choice_first + i;
            if (w->waitFrames != MSGW_WAIT_INPUT || (w->flags & MSGW_NO_INPUT)) { /* (a script polls Cross itself:
                                                                                      evt_updateObjects, below) */
                choice_cross = 1;
                return -1;
            }
            w->waitFrames = 1;
            g_gameState->gameData->soundHook(SND_SE_PLAY, SE_CURSOR, 0x800000, SND_PAN_CENTER, SND_VOL_MAX);
            return -1;
        }
        return -1;
    }
    if (menu_shown && inside(&r_menu, x, y)) { field_open_menu(); return -1; }
    if (menu_shown && cards_rect && inside(cards_rect, x, y) && max > 0) {
        if (field_open_menu()) cards_pending = 90; /* (the menu, then the card screen: cards_step) */
        return -1;
    }
    if (ate_shown && inside(&r_ate, x, y) && max > 0) { seq[0] = 0x0001; return 1; } /* select */
    if (inside(&r_log, x, y)) { log_on = !log_on; log_scroll = 0; opts_on = 0; return -1; }
    if (log_on && !opts_on && inside(&r_optbtn, x, y)) { opts_on = 1; return -1; }
    if (log_on && opts_on) { options_tap(x, y); return -1; }
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
        unsigned n = f ? (unsigned)(f->link - f->sector) : 0;
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
    static const u8 hp[] = TX("HP", "PV", "HP", "HP", "HP", "HP"), mp[] = TX("MP", "PM", "MP", "MP", "MP", "MP");
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

/* the message system's values (g_msgNumbers) as text: an item's name in yellow, a number, a bank's text (msg.c's
 * msg_emitItemName, msg_emitNumber, msg_renderText_text) */
static u8 *msg_value(u8 c, const u8 *arg, u8 *o, u8 *end) {
    const u8 *str = NULL;
    if (c == TEXT_MSG_ITEM_NAME) {
        s32 id = g_msgNumbers[arg[0] & 7];
        if (id < ITEM_ID_KEY_FIRST) str = g_itemNames + g_itemTable[id].name;
        else if (id < ITEM_ID_CARD_FIRST) str = g_keyItemNames + g_keyItemTable[id - ITEM_ID_KEY_FIRST].name;
        else str = (const u8 *)card_getName(id - ITEM_ID_CARD_FIRST);
        if (o + 2 > end) return o;
        *o++ = TEXT_ESC, *o++ = TEXT_COLOR + TEXT_PAL_YELLOW;
        for (int i = 0; i < 32 && str[i] != TEXT_END && o < end - 3; i++) {
            if (port_text_lead(str[i]) && str[i + 1] != TEXT_END) *o++ = str[i++]; /* (a two-byte character) */
            *o++ = str[i];
        }
        *o++ = TEXT_ESC, *o++ = TEXT_COLOR + TEXT_PAL_WHITE;
    } else if (c == TEXT_MSG_NUMBER) {
        s32 v = g_msgNumbers[arg[0] & 7];
        char d[12];
        int n = 0;
        if (v < 0) v = 0; /* (as msg_emitNumber) */
        do d[n++] = (char)(v % 10), v /= 10; while (v && n < 11);
        while (n && o < end) *o++ = (u8)d[--n];
    } else if (c == TEXT_BANK_TEXT && arg[0] < MSG_TEXT_BANKS && arg[1] < MSG_NUMBERS && g_msgTextBanks[arg[0]]) {
        const u8 *tbl = g_msgTextBanks[arg[0]];
        s32 k = g_msgNumbers[arg[1]];
        if (k >= 0 && k < tbl[0]) {
            const u8 *e = tbl + 1 + 2 * k;
            str = tbl + 1 + (e[0] | e[1] << 8);
            for (int i = 0; i < 64 && str[i] != TEXT_END && str[i] != TEXT_NEWLINE && o < end - 1; i++) {
                if (str[i] == TEXT_ESC || port_text_lead(str[i])) *o++ = str[i++];
                *o++ = str[i];
            }
        }
    }
    return o;
}

static const u8 *msg_plain(const u8 *t, u8 *o, int max, u8 stop) {
    u8 *start = o;
    int held = 0; /* after a hold: its line break taken */
    while (*t != TEXT_END && *t != stop && o < start + max - 5) {
        if (*t == TEXT_ESC && (t[1] == TEXT_MSG_ITEM_NAME || t[1] == TEXT_MSG_NUMBER || t[1] == TEXT_BANK_TEXT)) {
            o = msg_value(t[1], t + 2, o, start + max - 5);
            t += 2 + esc_args(t[1]);
            continue;
        }
        if (*t == TEXT_ESC && t[1] == TEXT_HOLD) { /* (the window waits for the script, which shows the rest after it
                                                       * as a window of its own: Prima Vista's "Who's there!?") */
            if (stop == TEXT_END && o > start && o[-1] != TEXT_NEWLINE) *o++ = TEXT_NEWLINE;
            held = 1;
            t += 2;
            continue;
        }
        if (held && (*t == TEXT_NEWLINE || *t == 0x0F)) { t++; continue; } /* (its own line break or space after it) */
        if (*t != TEXT_ESC) held = 0;
        if (*t == TEXT_ESC) {
            u8 c = t[1];
            int n = 2 + esc_args(c);
            /* drawn: names, colours, icons, button icons, argument values; the rest is timing, layout and choices */
            int keep = (c >= 0x10 && c <= 0x3F) || (c >= 0x60 && c <= 0x6F);
            while (n--) { if (keep) *o++ = *t; t++; }
        } else if (port_text_lead(*t)) { /* (a two-byte character: 0xFA-0xFD; JP's kanji too) */
            *o++ = *t++;
            *o++ = *t++;
        } else if (*t == TEXT_SPACE_1PX) { /* (message windows only: text_drawBlock draws it as a glyph, a box over the next letter) */
            t++;
        } else if (*t == 0xFE) { /* (a page break: a new line here) */
            *o++ = TEXT_NEWLINE;
            t++;
        } else *o++ = *t++;
    }
    while (o > start && o[-1] == TEXT_NEWLINE) o--; /* (nothing after a hold) */
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
/* ---- the port's options (settings.c), a page in the log's place: OPTIONS from the log view ---- */
__attribute__((weak)) int port_setting_get(int k) { (void)k; return 0; }
__attribute__((weak)) void port_setting_set(int k, int v) { (void)k; (void)v; }
/* (a setting this machine has no use for: its row is shown dim with the reason, and nothing changes it) */
__attribute__((weak)) int port_setting_available(int k) { (void)k; return 1; }
static const struct { const char *name; int n; const char *choice[3]; } opts[] = {
    {TX("3D DEPTH", "EFECTO 3D", "EFFETTO 3D", "3D TIEFE", "EFFET 3D", "3D@\xC9\xC2\xD6\xBB"), 3, {TX("LOW", "BAJO", "BASSO", "NIEDRIG", "BASSE", "@\xD6\xDC\xB2"), TX("NORMAL", "NORMAL", "NORMALE", "NORMAL", "NORMALE", "@\xCC\xC2\xB3"), TX("HIGH", "ALTO", "ALTO", "HOCH", "HAUTE", "@\xC2\xD6\xB2")}},
    {TX("TOP SCREEN", "PANTALLA SUP.", "SCHERMO SUP.", "OBERER BS", "ECRAN HAUT", "@\xB3\xB4\xB6\xDE\xD2\xDD"), 2, {TX("FIT", "AJUSTAR", "ADATTA", "ANPASSEN", "ADAPTE", "\xCC\xA8\xAF\xC4"), TX("SHARP", "N#ITIDO", "NITIDO", "SCHARF", "NET", "\xBC\xAC\xB0\xCC\xDF")}},
    {TX("VOICES", "VOCES", "VOCI", "STIMMEN", "VOIX", "\xCE\xDE\xB2\xBD"), 2, {TX("OFF", "NO", "NO", "AUS", "NON", "\xB5\xCC"), TX("ON", "S#I", "S$I", "AN", "OUI", "\xB5\xDD")}},
};
static void bar_mark(int x, int y, int w, int h) { /* a small bright bar (as the battle screen marks a target) */
    u32 *p = (u32 *)g_primPtr;
    p[0] = 3u << 24; p[1] = 0x6040E0F8u; p[2] = (u32)x | (u32)y << 16; p[3] = (u32)w | (u32)h << 16;
    addPrim(&g_ot[2], p);
    g_primPtr = (u8 *)(p + 4);
}
static RECT opt_rect(int row, int c) { RECT r = {(s16)(112 + c * 66), (s16)(64 + row * 36), 60, 28}; return r; }
static int opt_row; /* the row the D-pad is on (the panel by buttons: port_pad_filter) */
static void opt_step(int d) { /* to the next row there is something to set on */
    for (int i = 0; i < 3; i++) {
        opt_row = (opt_row + 3 + d) % 3;
        if (port_setting_available(opt_row)) return;
    }
}
static void draw_options(void) {
    static const u8 label[] = TX("OPTIONS", "OPCIONES", "OPZIONI", "OPTIONEN", "OPTIONS", "\xB5\xCC\xDF\xBC\xAE\xDD");
    RECT box = r_logbox;
    ui_drawLabel(box.x + 8, box.y - 3, 2, (s32)label, 0);
    if (!port_setting_available(opt_row)) opt_step(1);
    for (int i = 0; i < 3; i++) {
        ui_drawLabel(box.x + 10, 64 + i * 36 + 11, 2, (s32)opts[i].name, i == opt_row ? 0 : 2);
        if (!port_setting_available(i)) {
            static const char why[] = TX("NEW 3DS ONLY", "SOLO NEW 3DS", "SOLO NEW 3DS", "NUR NEW 3DS", "NEW 3DS SEULEMENT", "New 3DS@\xC9\xD0");
            RECT r = opt_rect(i, 0);
            ui_drawLabel(r.x + 8, r.y + 11, 2, (s32)why, 2);
            continue;
        }
        if (i == opt_row) ui_drawSprite(box.x + 2, 64 + i * 36 + 9, 2, 1, 0); /* (the hand: where the D-pad is) */
        int v = port_setting_get(i);
        for (int c = 0; c < opts[i].n; c++) {
            RECT r = opt_rect(i, c);
            int w = ui_measureLabel((u8 *)opts[i].choice[c]);
            ui_drawLabel(r.x + (r.w - w) / 2, r.y + 11, 2, (s32)opts[i].choice[c], c == v ? 0 : 2);
            if (c == v) bar_mark(r.x + 3, r.y + 6, 3, r.h - 12); /* (the chosen one) */
            win_drawBox(&r, 3, 2);
        }
    }
    win_drawBox(&box, 3, 2);
}
static int options_tap(int x, int y) {
    for (int i = 0; i < 3; i++)
        for (int c = 0; c < opts[i].n && port_setting_available(i); c++) {
            RECT r = opt_rect(i, c);
            if (inside(&r, x, y)) { port_setting_set(i, c); snd_playSe(SE_CURSOR); return 1; }
        }
    return 0;
}

/* ---- the title screen's choices (port/game/title_ui.c) ---- */
int port_title_cursor(void), port_title_has_save(void);
void port_title_choose(int choice);
static int title_opts; /* OPTIONS shown in place of the choices */
static RECT r_tnew = {40, 40, 240, 44}, r_tcont = {40, 92, 240, 44}, r_topt = {40, 156, 240, 32};
static void title_button(RECT *r, const char *label, int on, int dim) {
    int w = ui_measureLabel((u8 *)label);
    ui_drawLabel(r->x + (r->w - w) / 2, r->y + (r->h - 6) / 2, 2, (s32)label, dim ? 2 : 0);
    if (on) bar_mark(r->x + 4, r->y + 6, 3, r->h - 12); /* (where the D-pad's cursor is) */
    win_drawBox(r, 3, 2);
}
/* before the title (the boot screens, the attract images, the opening movie): a card, so the bottom screen is meant */
__attribute__((weak)) int port_audio_missing(void) { return 0; } /* (the 3DS's audio.c) */
static RECT r_boot = {40, 70, 240, 100};
static void draw_boot(void) {
    static const char name[] = "FINAL FANTASY IX", go[] = TX("PRESS START", "PULSA START", "PREMI START", "DR\"UCKE START", "APPUYEZ SUR START", "START\xCE\xDE\xC0\xDD@\xA6 \xB5\xBC\xC3\xB8\xC0\xDE\xBB\xB2"), tap[] = TX("OR TOUCH THE SCREEN", "O TOCA LA PANTALLA", "O TOCCA LO SCHERMO", "ODER BER\"UHRE DEN BILDSCHIRM", "OU TOUCHEZ CET ECRAN", "@\xCF\xC0\xCA \xB6\xDE\xD2\xDD\xC6 @\xC0\xAF\xC1");
    int w = ui_measureLabel((u8 *)name);
    ui_drawLabel(r_boot.x + (r_boot.w - w) / 2, r_boot.y + 22, 2, (s32)name, 0);
    w = ui_measureLabel((u8 *)go);
    ui_drawLabel(r_boot.x + (r_boot.w - w) / 2, r_boot.y + 52, 2, (s32)go, 0);
    w = ui_measureLabel((u8 *)tap);
    ui_drawLabel(r_boot.x + (r_boot.w - w) / 2, r_boot.y + 70, 2, (s32)tap, 2);
    win_drawBox(&r_boot, 3, 2);
    /* (no DSP firmware on the SD card: the game is silent, and only the log said why) */
    if (port_audio_missing()) {
        static RECT r = {40, 176, 240, 44};
        static const char a[] = TX("NO SOUND  DSPFIRM.CDC NOT FOUND", "SIN SONIDO  FALTA DSPFIRM.CDC", "NESSUN SUONO  MANCA DSPFIRM.CDC", "KEIN TON  DSPFIRM.CDC FEHLT", "PAS DE SON  DSPFIRM.CDC ABSENT", "\xBB\xB3\xDD\xC4\xDE@\xC5\xBC  DSPFIRM.CDC\xB6\xDE \xB1\xD8\xCF\xBE\xDD"), b[] = TX("DUMP IT WITH DSP1  SEE THE README", "EXTR#AELO CON DSP1  VER EL README", "ESTRAILO CON DSP1  VEDI IL README", "MIT DSP1 AUSLESEN  SIEHE README", "EXTRAIRE AVEC DSP1  VOIR LE README", "DSP1@\xC3\xDE \xC2\xB8\xAF\xC3\xB8\xC0\xDE\xBB\xB2  README\xA6 \xD0\xC3\xC8");
        w = ui_measureLabel((u8 *)a);
        ui_drawLabel(r.x + (r.w - w) / 2, r.y + 11, 2, (s32)a, 0);
        w = ui_measureLabel((u8 *)b);
        ui_drawLabel(r.x + (r.w - w) / 2, r.y + 26, 2, (s32)b, 2);
        win_drawBox(&r, 3, 2);
    }
}

/* game over (ovl_65 gameover_main: its picture, A goes back to the title) */
static void draw_gameover(void) {
    static RECT box = {40, 70, 240, 100};
    static const char a[] = "GAME OVER", b[] = TX("TOUCH THE SCREEN", "TOCA LA PANTALLA", "TOCCA LO SCHERMO", "BER\"UHRE DEN BILDSCHIRM", "TOUCHEZ CET ECRAN", "@\xB6\xDE\xD2\xDD\xC6 @\xC0\xAF\xC1"), c[] = TX("TO RETURN TO THE TITLE", "PARA VOLVER AL T#ITULO", "PER TORNARE AL TITOLO", "UM ZUM TITEL ZUR\"UCKZUKEHREN", "POUR REVENIR AU TITRE", "\xC0\xB2\xC4\xD9@\xC6 \xD3\xC4\xDE\xD8\xCF\xBD");
    int w = ui_measureLabel((u8 *)a);
    ui_drawLabel(box.x + (box.w - w) / 2, box.y + 22, 2, (s32)a, 0);
    w = ui_measureLabel((u8 *)b);
    ui_drawLabel(box.x + (box.w - w) / 2, box.y + 52, 2, (s32)b, 0);
    w = ui_measureLabel((u8 *)c);
    ui_drawLabel(box.x + (box.w - w) / 2, box.y + 68, 2, (s32)c, 2);
    win_drawBox(&box, 3, 2);
}

#ifdef NUI_DEBUG
/* PC: FF9_LABEL_WIDTHS=FILE measures each line of FILE as the label font draws it (ui_measureLabel) once the title
 * has loaded it (port/pc/platform.c, frame 3600), logs "label width<TAB>line" and exits: the width of a translated label against its box (port_text.h) */
char *getenv(const char *name);
void *fopen(const char *path, const char *mode);
char *fgets(char *s, int n, void *f);
int fclose(void *f);
void exit(int status);
void port_label_widths(void) {
    const char *path = getenv("FF9_LABEL_WIDTHS");
    char line[128];
    void *f = path ? fopen(path, "r") : NULL;
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        for (char *c = line; *c; c++)
            if (*c == '\n' || *c == '\r') *c = 0;
        port_log("label %d\t%s\n", ui_measureLabel((u8 *)line), line);
    }
    fclose(f);
    exit(0);
}
#endif

static void draw_title(void) {
    if (title_opts) {
        draw_options();
        title_button(&r_log, TX("BACK", "ATR#AS", "INDIETRO", "ZUR\"UCK", "RETOUR", "@\xD3\xC4\xDE\xD9"), 0, 0);
        return;
    }
    int cur = port_title_cursor();
    title_button(&r_tnew, TX("NEW GAME", "NUEVA PARTIDA", "NUOVA PARTITA", "NEUES SPIEL", "NOUVELLE PARTIE", "\xC6\xAD\xB0\xB9\xDE\xB0\xD1"), cur == 0, 0);
    title_button(&r_tcont, TX("CONTINUE", "CONTINUAR", "CONTINUA", "FORTSETZEN", "CONTINUER", "\xBA\xDD\xC3\xA8\xC6\xAD\xB0"), cur == 1, !port_title_has_save());
    title_button(&r_topt, TX("OPTIONS", "OPCIONES", "OPZIONI", "OPTIONEN", "OPTIONS", "\xB5\xCC\xDF\xBC\xAE\xDD"), 0, 0);
    ui_drawSprite(r_topt.x + 8, r_topt.y + 9, 2, 6, 0); /* (Square: Y) */
}
static int title_tap(int x, int y) {
    if (title_opts) {
        if (inside(&r_log, x, y)) { title_opts = 0; snd_playSe(SE_CANCEL); }
        else options_tap(x, y);
        return 1;
    }
    if (inside(&r_tnew, x, y)) port_title_choose(0);
    else if (inside(&r_tcont, x, y)) port_title_choose(1);
    else if (inside(&r_topt, x, y)) { title_opts = 1; snd_playSe(SE_CURSOR); }
    return 1;
}

/* ---- the Config screen's way in to the port's options: a button in the strip left of its title box, the options
 * over the screen when tapped (drawn over the menu's own frame on the bottom screen) ---- */
static int cfg_opts;
static RECT r_cfgbtn = {12, 8, 96, 22};
static int config_open(void) {
    return port_menu_state() == 2 && g_menuScreen.active && g_menuScreen.screen == MENU_SCREEN_CONFIG;
}
static void draw_config_overlay(void) {
    if (cfg_opts) {
        static RECT shade = {4, 30, 312, 160};
        win_drawBox(&shade, 4, 2); /* (an opaque panel under the options: the Config rows don't show through) */
        draw_options();
        title_button(&r_log, TX("BACK", "ATR#AS", "INDIETRO", "ZUR\"UCK", "RETOUR", "@\xD3\xC4\xDE\xD9"), 0, 0);
        return;
    }
    title_button(&r_cfgbtn, TX("3DS OPTIONS", "CONFIG. 3DS", "OPZIONI 3DS", "3DS EINST.", "OPTIONS 3DS", "3DS\xB5\xCC\xDF\xBC\xAE\xDD"), 0, 0);
    ui_drawSprite(r_cfgbtn.x + 4, r_cfgbtn.y + 5, 2, 6, 0); /* (Square: the 3DS's Y opens it too) */
}
static int config_overlay_tap(int x, int y) {
    if (cfg_opts) {
        if (inside(&r_log, x, y)) { cfg_opts = 0; snd_playSe(SE_CANCEL); }
        else options_tap(x, y);
        return 1;
    }
    if (inside(&r_cfgbtn, x, y)) { cfg_opts = 1; snd_playSe(SE_CURSOR); return 1; }
    return 0;
}

/* The options panel by buttons (the title's, the Config screen's, the log's): Y opens it on the title and the Config
 * screen; while it is up the D-pad picks a row and changes its setting, B or Y closes it, and the game gets no buttons
 * (a button still held as it closes is kept from the game until released, so B doesn't also back out of the screen). */
static unsigned pad_prev, pad_swallow;
static void set_opt(int row, int dir) {
    int v = port_setting_get(row) + dir;
    if (v < 0 || v >= opts[row].n || !port_setting_available(row)) return;
    port_setting_set(row, v);
    snd_playSe(SE_CURSOR);
}
unsigned int port_pad_filter(unsigned int b) {
    unsigned pressed = b & ~pad_prev;
    pad_prev = b;
    pad_swallow &= b; /* (released: the game may have it again) */
    if (!g_gameState || !g_gameState->gameData) return b;
    int *open = title_opts ? &title_opts : cfg_opts ? &cfg_opts : (log_on && opts_on) ? &opts_on : NULL;
    if (open) {
        if (pressed & 0x0010) opt_step(-1), snd_playSe(SE_CURSOR);
        if (pressed & 0x0040) opt_step(1), snd_playSe(SE_CURSOR);
        if (pressed & 0x0080) set_opt(opt_row, -1);
        if (pressed & 0x0020) set_opt(opt_row, +1);
        if (pressed & (0x2000 | 0x8000)) { *open = 0; snd_playSe(SE_CANCEL); pad_swallow = b; }
        return 0;
    }
    if (pressed & 0x8000) {
        if (g_gameState->mode == 0 && port_title_live()) { title_opts = 1, opt_row = 0, pad_swallow = b; snd_playSe(SE_CURSOR); }
        else if (config_open() && !fade_isBusy()) { cfg_opts = 1, opt_row = 0, pad_swallow = b; snd_playSe(SE_CURSOR); }
    }
    return b & ~pad_swallow;
}

static void draw_log(void) {
    static const u8 label[] = TX("LOG", "REGISTRO", "DIALOGHI", "VERLAUF", "JOURNAL", "\xDB\xB8\xDE"), optl[] = TX("OPTIONS", "OPCIONES", "OPZIONI", "OPTIONEN", "OPTIONS", "\xB5\xCC\xDF\xBC\xAE\xDD");
    RECT box = r_logbox;
    { int w = ui_measureLabel((u8 *)optl); ui_drawLabel(r_optbtn.x + (r_optbtn.w - w) / 2, r_optbtn.y + 7, 1, (s32)optl, 0); win_drawBox(&r_optbtn, 1, 2); }
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

static int choices_only;
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
        else if (port_text_lead(*t) && t[1] != TEXT_END) t++; /* (its second byte can look like anything) */
        else if (*t == TEXT_ESC) t += t[1] == 0x43 || t[1] == 0x44 || t[1] == 0x49 || t[1] == 0x4A || (t[1] >= 0x4B && t[1] <= 0x4E) || (t[1] >= 0x68 && t[1] <= 0x6F) ? 2 : 1;
    }
    int n = g_msgChoiceCount > MAX_CHOICES ? MAX_CHOICES : g_msgChoiceCount;
    /* (between the header and the buttons; the whole screen when the choices are alone there: a cutscene's, or more
     * than 8, which the middle can't hold (the Moogle's tutorial: 11)) */
    int full = choices_only || n > 8, top = full ? 4 : 34, avail = full ? 216 : 152; /* (the frame is 224 lines) */
    int h = n > 4 ? 24 : 32, gap = n > 4 ? 3 : 6;
    if (n * h + (n - 1) * gap > avail) gap = 2, h = (avail - (n - 1) * gap) / n;
    int y0 = top + (avail - (n * h + (n - 1) * gap)) / 2;
    static u8 lines[MAX_CHOICES][96];
    /* (logical rows the game hides, g_msgChoiceEnabled bit k clear, get no button: the Moogle's Mogshop where there
     * is none) */
    u32 shown = g_msgChoiceEnabled ? g_msgChoiceEnabled : ~0u;
    for (int i = 0, k = 0; i < n && *t != TEXT_END && k < 32; k++) {
        u8 raw[160]; /* (the line to its newline, ended at F9 09: the last choice runs into the window's codes) */
        int r_n = 0;
        while (*t != TEXT_END && *t != TEXT_NEWLINE && r_n < 158) {
            if (t[0] == TEXT_ESC && t[1] == 0x09) { while (*t != TEXT_END && *t != TEXT_NEWLINE) t++; break; }
            if (port_text_lead(*t) && t[1] != TEXT_END) raw[r_n++] = *t++; /* (a two-byte character) */
            raw[r_n++] = *t++;
        }
        raw[r_n] = TEXT_END;
        msg_plain(raw, lines[i], sizeof lines[i], TEXT_NEWLINE);
        if (*t == TEXT_NEWLINE) t++;
        if (!(shown >> k & 1)) continue;
        RECT *r = &r_choice[i];
        r->x = 24; r->w = 272; r->h = h; r->y = y0 + i * (h + gap);
        int sel = i == g_msgChoice - 0;
        /* (an item's name ends back in white: in an unselected choice, back in its grey) */
        for (u8 *c = lines[i]; !sel && *c != TEXT_END; c++)
            if (c[0] == TEXT_ESC && c[1] == TEXT_COLOR + TEXT_PAL_WHITE) c[1] = TEXT_COLOR + TEXT_PAL_GREY;
        text_drawBlock(r->x + 12, r->y + (h - 12) / 2 - (h < 24), 2, lines[i], sel ? 0 : 2);
        win_drawBox(r, 3, 2);
        i++;
    }
    n_choices = n;
    choice_first = 0;
    return 1;
}

/* what the PS1 shows only through its menu is shown only when the menu can be opened: in the opening and other
 * cutscene stretches (GD_MENU_DISABLED) the party may be a placeholder, and there is no menu to offer */
/* whether the player has had control since the title: before that it is a cutscene or a movie (the opening's
 * "Opening-For FMV" field; its party a placeholder), and the bottom screen stays dark. A dialogue takes control away
 * for a moment, which mustn't hide anything. */
/* (control held for STEADY frames: an event that gives it for a frame, as an encounter's in a cutscene stretch, brought
 * the whole screen up for that frame before the swirl) */
#define STEADY 10
/* (held across fields from then on: a field that opens with a cutscene (Memoria's Recollection, its movie of Alexandria)
 * has the real party though control comes only after. Afresh from the title: a new game's opening has a placeholder) */
/* (the ending's cutscenes (ui_split.c port_ending_field) count only control had in the field itself: there is none, and
 * the minimap and party list showed over them, the party half dead from the last battle (the user's report)) */
static int had_control(void) {
    static int had, run, here, field = -1;
    int port_ending_field(void);
    if (g_gameState->mode == 0) had = 0, run = 0;
    if (g_gameState->mode == 1 && g_gameState->gameData->fieldId != field) field = g_gameState->gameData->fieldId, here = 0;
    if (ram_ok(g_evtState, sizeof *g_evtState) && g_evtState->controlEnabled) { if (++run >= STEADY) had = here = 1; }
    else run = 0;
    return port_ending_field() ? here : had;
}

/* Whether the player is playing, not watching a cutscene: the map, the party and the menu's buttons are shown then (the
 * header and LOG always). Playing once control has held STEADY VBlanks. Control taken away just after a confirm press
 * made while in control is the player's own doing (a talk, a chest, a save point) and keeps it while its message
 * windows follow one another (TALK_GAP VBlanks without one: the talk has led into a scene, as Baku's in the opening's
 * meeting room into the whole Masked Man act). Taken away by a script that gives it back on every path without
 * leaving the field (port_scene_returns, field_map.c: Puck running into Vivi), it is play too, until control is back;
 * taken away otherwise
 * it is the field's script: a cutscene, and after CUTSCENE_VBLANKS (a script's pause of a moment is not one) all of it
 * goes until control is back. A field entered, or the field again after a battle, without control yet: a cutscene at
 * once. (Latched from the first control on (had_control), the map and party showed through every cutscene: after the
 * Masked Man, Vivi's arrival in Alexandria with the scene before's party (the game sets his only as it gives control),
 * the play on the Prima Vista (the user's reports)) */
#define TALK_VBLANKS 12
#define CUTSCENE_VBLANKS 15
#define TALK_GAP 30
static int party_live(void) {
    static int live, on, off, talk, gap, confirm_at = -1000, field = -1, mode = -1, last_frame = -1;
    unsigned int plat_pad_buttons(int port);
    if (!had_control()) return live = 0;
    const int f = (int)VSync(-1);
    if (f == last_frame) return live; /* (once a VBlank) */
    last_frame = f;
    const int ctl = ram_ok(g_evtState, sizeof *g_evtState) && g_evtState->controlEnabled;
    if (g_gameState->mode != 1) { mode = g_gameState->mode; return live; }
    if (mode != 1 || g_gameState->gameData->fieldId != field) { /* (a field entered, or the field again after a battle) */
        mode = 1, field = g_gameState->gameData->fieldId;
        if (!ctl) live = 0, on = 0, talk = 0;
    }
    if (ctl) {
        off = 0, talk = 0;
        { /* (Cross or Circle pressed now: not held, which a player walking with a button down does all the time) */
            static unsigned held;
            const unsigned now = plat_pad_buttons(0) & 0x6000u;
            if (now & ~held) confirm_at = f;
            held = now;
        }
        if (++on >= STEADY) live = 1;
        return live;
    }
    if (on) { /* (control just taken away: by the player's press, or by a script that gives it back here: field_map.c) */
        int port_scene_returns(void);
        talk = port_scene_returns() ? 2 : f - confirm_at <= TALK_VBLANKS ? 1 : 0, gap = 0; /* (the script's answer first: a
         * press just before Puck ran into Vivi made it a talk, which went at the half second of his fall without a line) */
    }
    on = 0;
    if (talk == 1) { /* (a talk goes on while its windows come one after another: a stretch without one, it has become a scene) */
        int open = 0;
        for (int k = 0; k < 10; k++) open |= g_msgWindows[k].open != 0;
        if (open) gap = 0;
        else if (++gap >= TALK_GAP) talk = 0;
    }
    if (!talk && ++off >= CUTSCENE_VBLANKS) live = 0;
    return live;
}

/* (changed only once the new state has held STEADY frames: at an encounter the menu flags change for a frame or two
 * before the swirl, and the party list went and came back) */
static int menu_allowed(void) {
    static int shown, run, last_frame = -1;
    int now = party_live() && !(g_gameState->gameData->flags & GD_MENU_DISABLED);
    int f = (int)VSync(-1);
    if (f != last_frame) { /* (once a frame: draw asks more than once) */
        last_frame = f;
        if (now == shown) run = 0;
        else if (++run >= STEADY) shown = now, run = 0;
    }
    return shown;
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

/* A field's data current: from its fld_load's end until the game leaves field mode (a battle, the world map) or loads
 * another. Outside it the location name points into event data being replaced and the walkmesh is half-loaded. */
void port_scenario_field_entering(int field);
void port_scenario_field_ready(int field);
static int field_ready, after_battle; /* (after_battle: the field now loading follows a battle) */
static unsigned field_serial, field_frames; /* loads so far; frames since the last */
void port_field_entering(int field) {
    field_ready = 0;
    port_scenario_field_entering(field);
}
void port_field_ready(int field) {
    /* (after a battle, its framebuffers black before its first frame: the battle drew 218 of their 224 lines, and the
     * rows below showed on the top screen, scaled to its 240, as a dim grey bar until the field had drawn over them) */
    if (after_battle)
        for (int i = 0; i < 2; i++) ClearImage(&g_gameState->drawEnv[i].clip, 0, 0, 0);
    after_battle = 0;
    field_ready = 1;
    field_serial++;
    field_frames = 0;
    port_scenario_field_ready(field);
}
int port_field_current(void) { return field_ready; }
/* (port/game/field_map.c: its map is baked once per load) */
unsigned port_field_serial(int *frames) {
    *frames = (int)field_frames;
    return field_serial;
}

/* choices_only: no player control (a cutscene) but a question open: its choices alone */
int port_disc_prompt_live(void), port_disc_prompt_cursor(void);
u8 *port_disc_prompt_text(int i);
void port_disc_prompt_set(int c);
static RECT r_disc[2] = {{24, 75, 272, 32}, {24, 113, 272, 32}};
static void draw_disc_prompt(void) { /* (the end of a disc's "save your game?": disc_prompt.c) */
    int cur = port_disc_prompt_cursor();
    for (int i = 0; i < 2; i++) {
        u8 *t = port_disc_prompt_text(1 + i);
        if (t) text_drawBlock(r_disc[i].x + 12, r_disc[i].y + 10, 2, t, cur == i ? 0 : 2);
        win_drawBox(&r_disc[i], 3, 2);
    }
}
/* The Prima Vista play's sword fight (field 64, decomp R38): its "Press <button>!" balloon over Blank (messages
 * 0x70-0x77; the button: field byte 0x2E, 0-7 Left, Right, Triangle, Down, Cross, Up, Circle, Square) also as one
 * large prompt on the bottom screen,
 * the button as the 3DS names it (display only: the button is still the input) */
static int canary_prompt(void) {
    if (g_gameState->mode != 1 || g_gameState->gameData->fieldId != 64) return -1;
    if (!ram_ok(g_evtState, sizeof *g_evtState)) return -1;
    for (int k = 0; k < 10; k++) /* (a "Press" balloon up: messages 0x70-0x77; the button is field byte 0x2E) */
        if (g_msgWindows[k].open && g_msgWindows[k].msgId >= 0x70 && g_msgWindows[k].msgId <= 0x77)
            return g_evtState->fieldVars[0x2E] & 7;
    return -1;
}
static void draw_canary(int b) {
    static const char *const names[8] = {TX("LEFT", "IZQUIERDA", "SINISTRA", "LINKS", "GAUCHE", "@\xCB\xC0\xDE\xD8"), TX("RIGHT", "DERECHA", "DESTRA", "RECHTS", "DROITE", "@\xD0\xB7\xDE"), "X", TX("DOWN", "ABAJO", "GI$U", "UNTEN", "BAS", "@\xBC\xC0"), "A", TX("UP", "ARRIBA", "SU", "OBEN", "HAUT", "@\xB3\xB4"), "B", "Y"};
    static const u8 icon[8] = {0, 0, 7, 0, 4, 0, 5, 6}; /* (the game's button sprites, drawn as the 3DS's: 0 none) */
    static RECT box = {60, 70, 200, 100};
    int w = ui_measureLabel((u8 *)TX("PRESS", "PULSA", "PREMI", "DR\"UCKE", "APPUYEZ", "@\xB5\xBC\xC3"));
    ui_drawLabel(box.x + (box.w - w) / 2, box.y + 22, 2, (s32) TX("PRESS", "PULSA", "PREMI", "DR\"UCKE", "APPUYEZ", "@\xB5\xBC\xC3"), 2);
    w = ui_measureLabel((u8 *)names[b]);
    if (icon[b]) ui_drawSprite(box.x + box.w / 2 - 8, box.y + 40, 2, icon[b], 0);
    ui_drawLabel(box.x + (box.w - w) / 2, box.y + (icon[b] ? 64 : 52), 2, (s32)names[b], 0);
    win_drawBox(&box, 3, 2);
}
static int choice_open(void) {
    if (port_disc_prompt_live()) return 1;
    if (canary_prompt() >= 0) return 1;
    return g_msgChoiceLine >= 0 && g_msgChoiceCount > 0 && g_msgChoiceWindow >= 0 && g_msgChoiceWindow < 10 &&
           g_msgWindows[g_msgChoiceWindow].open;
}

static void draw(void) {
    static const u8 menu[] = TX("MENU", "MEN#U", "MENU", "MEN\"U", "MENU", "\xD2\xC6\xAD\xB0"), cards[] = TX("CARDS", "CARTAS", "CARTE", "KARTEN", "CARTES", "\xB6\xB0\xC4\xDE"), party[] = TX("PARTY", "GRUPO", "SQUADRA", "GRUPPE", "EQUIPE", "\xCA\xDF\xB0\xC3\xA8"), map[] = TX("MAP", "MAPA", "MAPPA", "KARTE", "CARTE", "\xCF\xAF\xCC\xDF"), gil_suffix[] = "G";
    GameData *g = g_gameState->gameData;
    u8 buf[16];
    if (port_disc_prompt_live()) {
        draw_disc_prompt();
        return;
    }
    { int b = canary_prompt(); if (b >= 0) { draw_canary(b); return; } }
    if (choices_only || (choice_open() && g_msgChoiceCount > 8)) { /* (choices alone: draw_choices) */
        draw_choices();
        return;
    }

    /* the header: where, and since when and with what (while playing: not through a cutscene, as the map and party,
     * the user's choice: the LOG button alone then) */
    if (party_live()) {
    if ((field_ready || g_gameState->mode != 1) && ram_ok(g->locationName, 1) && *g->locationName != 0xFF)
        text_drawBlock(r_head.x + 8, r_head.y + 6, 2, g->locationName, 0);
    /* (placed from the right by their widths: the gil against the box's edge, its icon, the time before it. At fixed
     * places the gil icon was drawn over the seconds) */
    text_formatNumber(g->gil, buf, -1, 1);
    strcat((char *)buf, (const char *)gil_suffix);
    int gx = r_head.x + r_head.w - 8 - text_measureDigits(buf);
    ui_drawSmallDigits(gx, r_head.y + 9, 2, buf, 0);
    ui_drawSprite(gx - 13, r_head.y + 8, 2, 0xB2, 0);
    int tcol = text_formatTime(game_getPlayTime(), buf, 1);
    int tx = gx - 13 - 10 - text_measureDigits(buf);
    if (tx > r_head.x + 191) tx = r_head.x + 191; /* (where it always was, with the gil short) */
    ui_drawSmallDigits(tx, r_head.y + 9, 2, buf, tcol);
    ui_drawSprite(tx - 13, r_head.y + 8, 2, 0xB1, 0);
    win_drawBox(&r_head, 3, 2);
    }

    if (!draw_choices() && log_on) opts_on ? draw_options() : draw_log();
    else if (!n_choices) {
        if (party_live()) { /* (the map and party while playing, not through a cutscene) */
            ui_drawLabel(r_map.x + 8, r_map.y - 3, 2, (s32)map, 0);
            if (field_ready || g_gameState->mode != 1)
                g_primPtr = port_field_map_draw(&g_ot[2], g_primPtr, (u8 *)(hud_mem + OTN + BUF_WORDS) - 64, r_map.x + 3,
                                                r_map.y + 3, r_map.w - 6, r_map.h - 6);
            win_drawBox(&r_map, 3, 2);
        }
        /* (the party while playing: the real party then, not the opening's placeholder or a scene's before control) */
        if (party_live()) {
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
        static const u8 logl[] = TX("LOG", "REGISTRO", "DIALOGHI", "VERLAUF", "JOURNAL", "\xDB\xB8\xDE"), mapl[] = TX("MAP", "MAPA", "MAPPA", "KARTE", "CARTE", "\xCF\xAF\xCC\xDF"), hidel[] = TX("HIDE", "OCULTAR", "NASCONDI", "AUSBLENDEN", "MASQUER", "@\xB6\xB8\xBD");
        button(&r_log, log_on ? (party_live() ? mapl : hidel) : logl, NULL); /* (closing the log: back to the map, or with
                                                                                * no map up (a cutscene) to nothing) */
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
__attribute__((weak)) void ctr_flat_background(int on) { (void)on; } /* (the 3DS renderer's: stereo depth) */
/* the Config screen's overlay as a list of its own over the menu's frame (as the battle's over its UI) */
static void overlay(void (*fn)(void));
static void config_overlay(void) { overlay(draw_config_overlay); }
/* the world map's questions (the moogle's, the chocobo's menu, the airship's Go to and navigation): the choices as
 * large buttons over the bottom screen's map, as a field's (the question stays on top) */
static void draw_world_choices(void) { draw_choices(); }

/* world_navi.c, as it draws the bottom screen's map into the world's own packets: the question's choices too, if one
 * is open (1: drawn) */
int port_world_choices(void) {
    world_choices = 0;
    if (port_menu_state() || !g_gameState->gameData->font || !choice_open()) return 0;
    draw_world_choices();
    world_choices = n_choices > 0;
    return world_choices;
}
/* a list of the port's own over the frame the game drew for the bottom screen (as the battle's over its UI) */
static void overlay(void (*fn)(void)) {
    PsxDisplayInfo d;
    psx_gpu_display_info(&d);
    if (d.rgb24) return;
    ClearOTagR(hud_ot, OTN);
    u32 *saved_ot = g_ot;
    u8 *saved_prim = g_primPtr;
    u32 *p = hud_mem + OTN;
    p[0] = 3u << 24;
    p[1] = 0xE3000000u | (u32)d.x | (u32)d.y << 10;
    p[2] = 0xE4000000u | (u32)(d.x + d.w - 1) | (u32)(d.y + d.h - 1) << 10;
    p[3] = 0xE5000000u | ((u32)d.x & 0x7FF) | ((u32)d.y & 0x7FF) << 11;
    addPrim(&hud_ot[OTN - 1], p);
    g_ot = hud_ot;
    g_primPtr = (u8 *)(p + 4);
    fn();
    g_ot = saved_ot;
    g_primPtr = saved_prim;
    psx_gpu_sync(); /* (the scene's walk, if it is on another core: the drawing state is its until it ends) */
    Gp0State saved_gp0 = gp0_state;
    psx_ui_list_begin_keep();
    port_draw_ot_buffer(&hud_ot[OTN - 1], hud_mem, sizeof hud_mem);
    psx_ui_list_end();
    gp0_state = saved_gp0;
}

/* n words of a and b equal */
static int same_words(const u32 *a, const u32 *b, unsigned n) {
    for (; n >= 4; n -= 4, a += 4, b += 4)
        if ((a[0] ^ b[0]) | (a[1] ^ b[1]) | (a[2] ^ b[2]) | (a[3] ^ b[3])) return 0;
    for (; n; n--)
        if (*a++ != *b++) return 0;
    return 1;
}

/* A sprite's or a tile's packet (one GP0 60-7F command): its rectangle. 0: something else. */
static int sprite_box(const u32 *pk, int box[4]) {
    const u32 len = pk[0] >> 24, cmd = pk[1] >> 24;
    if (cmd < 0x60 || cmd > 0x7F) return 0;
    const int textured = (cmd >> 2) & 1, size = (cmd >> 3) & 3;
    if (len != (u32)(2 + textured + (size == 0))) return 0;
    const int x = (int)(pk[2] << 21) >> 21, y = (int)(pk[2] << 5) >> 21;
    int w = size == 1 ? 1 : size == 2 ? 8 : 16, h = w;
    if (size == 0) w = (int)(pk[3 + textured] & 0x3FF), h = (int)(pk[3 + textured] >> 16 & 0x1FF);
    box[0] = x, box[1] = y, box[2] = x + w, box[3] = y + h;
    return 1;
}
/* What differs between this VBlank's packets (hud_mem) and the ones last drawn into the layer (old, as many words):
 * if only sprites and tiles, in the same places of the list, the rectangle that takes them in, as they were and as
 * they are (within w x h; 1). 0: anything else differs, or nothing, or most of the screen. */
static int hud_changed_rect(const u32 *old, unsigned words, int w, int h, int rect[4]) {
    for (unsigned i = 0; i < OTN + 4; i++)
        if (hud_mem[i] != old[i]) return 0;
    int x0 = 1 << 20, y0 = 1 << 20, x1 = -(1 << 20), y1 = -(1 << 20), n = 0;
    for (unsigned i = OTN + 4; i < words;) {
        const u32 len = hud_mem[i] >> 24;
        if (!len || i + 1 + len > words || hud_mem[i] != old[i]) return 0;
        int differs = 0;
        for (u32 k = 1; k <= len; k++) differs |= hud_mem[i + k] != old[i + k];
        if (differs) {
            int a[4], b[4];
            if (!sprite_box(hud_mem + i, a) || !sprite_box(old + i, b) || ++n > 16) return 0;
            x0 = a[0] < x0 ? a[0] : x0, y0 = a[1] < y0 ? a[1] : y0, x1 = a[2] > x1 ? a[2] : x1, y1 = a[3] > y1 ? a[3] : y1;
            x0 = b[0] < x0 ? b[0] : x0, y0 = b[1] < y0 ? b[1] : y0, x1 = b[2] > x1 ? b[2] : x1, y1 = b[3] > y1 ? b[3] : y1;
        }
        i += 1 + len;
    }
    if (!n) return 0;
    x0 = x0 < 0 ? 0 : x0, y0 = y0 < 0 ? 0 : y0, x1 = x1 > w ? w : x1, y1 = y1 > h ? h : y1;
    if (x0 >= x1 || y0 >= y1 || (x1 - x0) * (y1 - y0) * 5 > w * h * 2) return 0;
    rect[0] = x0, rect[1] = y0, rect[2] = x1, rect[3] = y1;
    return 1;
}
int psx_ui_list_begin_part(int x0, int y0, int x1, int y1);
/* the credit card after the Masked Man ("Conceived & Produced by Hironobu Sakaguchi", fld_showSplashImage as the cargo
 * room, field 52, hands over to Vivi in Alexandria, 100) on the top screen: nothing on the bottom meanwhile, not even the header and LOG (the user's
 * report). Set around the call (config/patches.txt) */
static int splash_on;
void port_splash_running(int on) { splash_on = on; }
void port_bottom_hud(void) {
    { void port_rewards_live_draw(void); port_rewards_live_draw(); } /* (the rewards under a battle: rewards_top.c) */
    shown = shown_battle = 0;
    if (splash_on) return;
    ctr_flat_background(g_gameState && g_gameState->mode == 1);
    if (g_gameState && g_gameState->gameData) cards_step();
    if (g_gameState && g_gameState->gameData && g_gameState->gameData->font) top_hud();
    if (!g_gameState || !g_gameState->gameData) return;
    if (!config_open()) cfg_opts = 0;
    else if (g_gameState->gameData->font) { config_overlay(); return; }

    if (port_menu_state()) { boot_live = 0; return; } /* (not left from the frames between the title and its Continue) */
    int battle = g_gameState->mode == 2, world = g_gameState->mode == 3;
    if (!battle) port_battle_hud_on = 0;
    if (battle || world) field_ready = 0; /* (the field is reloaded after) */
    if (battle || g_gameState->mode == 8) after_battle = 1;
    if (g_gameState->mode == 1 && field_ready) field_frames++;
    int title = g_gameState->mode == 0 && port_title_live();
    int port_opening_takes_start(void);
    int boot = g_gameState->mode == 0 && !title && !port_menu_state() && port_opening_takes_start(); /* (before the
                                                                     * title, while Start does something: boot_live) */
    boot_live = boot;
    /* (game over: a field's (mode 7) or a battle's, which battle_end runs in its own mode) */
    int gameover = g_gameState->mode == 7 || (g_gameState->mode == 8 && g_gameState->gameData &&
                                              g_gameState->gameData->battleResult == BATTLE_END_GAME_OVER);
    int port_bj_live(void);
    int bj = g_gameState->mode == 4 && port_bj_live();
    bj_live = bj;
    gameover_live = gameover;
    if (!title) title_opts = 0;
    if (world && world_choices) shown = 1; /* (the world map's question, drawn with its map: port_world_choices) */
    if (g_gameState->mode != 1 && !battle && !world && !title && !boot && !gameover && !bj) return;
    /* (an encounter begun: dark from the swirl on, until the battle's own screen comes with its HUD. Drawn through it,
     * the party list went and came back as the menu flags changed under it, then all went at the battle (the user's
     * report)) */
    { int port_field_encountering(void); if (g_gameState->mode == 1 && port_field_encountering()) return; }
    if (!g_gameState->gameData->font) return; /* (the common images aren't loaded yet) */
    PsxDisplayInfo d;
    psx_gpu_display_info(&d);
    if (!boot && !gameover && !bj && (d.rgb24 || d.w < 256)) return; /* (a movie) */
    /* (a cutscene or movie field: nothing to show yet, but a question the game asks there (the end of disc 1's "save your
     * game?") still gets its choices as buttons, alone) */
    int ctl = battle || world || title || boot || gameover || bj || had_control();
    if (!ctl && !choice_open()) return;
    int port_auction_live(void);
    int auction = g_gameState->mode == 1 && port_auction_live();
    auction_live = auction;
    { int port_minigame_panel_up(void); if (g_gameState->mode == 1 && !auction && port_minigame_panel_up()) return; } /* (a minigame's panel: ui_split.c) */
    { int port_sword_fight(void); if (port_sword_fight()) return; } /* (the play's sword fight: dark, ui_split.c) */
    choices_only = !ctl;

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
    else if (world) drew = 0; /* (the world map's own: world_navi.c) */
    else if (title) draw_title();
    else if (boot) draw_boot();
    else if (gameover) draw_gameover();
    else if (bj) { void port_bj_draw(void); port_bj_draw(); }
    else if (auction) { void port_auction_draw(void); port_auction_draw(); }
    else draw();
    unsigned used = (unsigned)(g_primPtr - (u8 *)hud_mem);
    g_ot = saved_ot;
    g_primPtr = saved_prim;
    if (used > sizeof hud_mem) port_fatal("field HUD: packet buffer overrun");
    if (battle) { /* (the battle's list of this VBlank was drawn for another screen than this one: again, for this) */
        extern unsigned int psx_vblank_list_serial, psx_vblank_list_flags;
        extern int port_battle_hud_targets, port_battle_help_on;
        void psx_vblank_list_again(void);
        static unsigned int seen;
        const unsigned int now = (port_battle_hud_on != 0) | (port_battle_hud_targets != 0) << 1 | (port_battle_help_on != 0) << 2;
        if (psx_vblank_list_serial != seen && psx_vblank_list_flags != now) {
            psx_gpu_sync();
            Gp0State saved_gp0 = gp0_state;
            psx_vblank_list_again();
            gp0_state = saved_gp0;
        }
        seen = psx_vblank_list_serial;
    }
    if (!drew) return;
    /* the screen's drawing as it was drawn last into the displayed buffer's layer (its packets the same, word for word)
     * and that layer neither cleared nor drawn into since (the battle's own list, drawn before it every VBlank, draws
     * nothing there: the battle screen's in its place): left as it is. Drawn every VBlank, the battle screen's 400-odd
     * primitives were some 30% of what a battle draws, the scene's 3D included, and a field's HUD 390 of its 1300, 7 ms
     * of the 26 its drawing took at Lindblum's castle station (an Old 3DS). Per layer: a field's two framebuffers are
     * shown in turn, each with its own */
    /* (each layer's packets as last drawn into it, compared word for word: no hash to collide, and cheaper than one) */
    static u32 last_mem[2][OTN + BUF_WORDS];
    static unsigned last_used[2], last_serial[2] = {~0u, ~0u};
    {
        unsigned serial = ctr_ui_layer_serial();
        if (serial != ~0u && serial == last_serial[serial & 1] && used == last_used[serial & 1] &&
            same_words(hud_mem, last_mem[serial & 1], used / 4)) {
            ctr_ui_layer_keep();
            shown = !world;
            shown_battle = battle;
            return;
        }
    }

    /* The same list but for a few sprites or tiles (a status icon's frame, a digit, the clock): the rectangle that
     * takes them in is cleared and drawn again alone, the list's other sprites and tiles left out of the walk and
     * everything else cut to it by the drawing area. The whole screen, 400-odd primitives through the layer's path, was
     * drawn again for each such change: 2.5 ms of every VBlank on an Old 3DS while an animated status icon is on a
     * party member's card (the sweep's e0167_a01: the bottom screen's and battle's UI 295 ms of a beat). */
    {
        const unsigned serial = ctr_ui_layer_serial();
        int rect[4];
        if (serial != ~0u && serial == last_serial[serial & 1] && used == last_used[serial & 1] &&
            hud_changed_rect(last_mem[serial & 1], used / 4, d.w ? d.w : 320, d.h ? d.h : 240, rect) &&
            psx_ui_list_begin_part(rect[0], rect[1], rect[2], rect[3])) {
            __builtin_memcpy(last_mem[serial & 1], hud_mem, used); /* (as built: before what follows) */
            static u16 hid_at[1024];
            static u8 hid_len[1024];
            unsigned nhid = 0;
            for (unsigned i = OTN + 4; i < used / 4;) {
                const u32 len = hud_mem[i] >> 24;
                int b[4];
                /* (not the pointing hands, 23 x 13: the renderer notes where each is as it meets it) */
                if (nhid < 1024 && sprite_box(hud_mem + i, b) && !(b[2] - b[0] == 23 && b[3] - b[1] == 13) &&
                    (b[0] >= rect[2] || b[2] <= rect[0] || b[1] >= rect[3] || b[3] <= rect[1]))
                    hid_at[nhid] = (u16)i, hid_len[nhid++] = (u8)len, hud_mem[i] &= 0x00FFFFFFu;
                i += 1 + len;
            }
            const u32 area0 = p[1], area1 = p[2];
            p[1] = 0xE3000000u | (u32)(d.x + rect[0]) | (u32)(d.y + rect[1]) << 10;
            p[2] = 0xE4000000u | (u32)(d.x + rect[2] - 1) | (u32)(d.y + rect[3] - 1) << 10;
            Gp0State saved_part = gp0_state;
            port_draw_ot_buffer(&hud_ot[OTN - 1], hud_mem, sizeof hud_mem);
            psx_ui_list_end();
            gp0_state = saved_part;
            p[1] = area0, p[2] = area1;
            for (unsigned k = 0; k < nhid; k++) hud_mem[hid_at[k]] |= (u32)hid_len[k] << 24;
            const unsigned after = ctr_ui_layer_serial();
            if (after != ~0u) last_used[after & 1] = used, last_serial[after & 1] = after;
            shown = !world;
            shown_battle = battle;
            return;
        }
    }
    psx_gpu_sync();
    Gp0State saved_gp0 = gp0_state; /* (the field's drawing environment is the field's again after) */
    if (battle || world) psx_ui_list_begin_keep();
    else psx_ui_list_begin();
    port_draw_ot_buffer(&hud_ot[OTN - 1], hud_mem, sizeof hud_mem);
    psx_ui_list_end();
    gp0_state = saved_gp0;
    {
        unsigned serial = ctr_ui_layer_serial();
        if (serial != ~0u) {
            __builtin_memcpy(last_mem[serial & 1], hud_mem, used);
            last_used[serial & 1] = used, last_serial[serial & 1] = serial;
        }
    }
    shown = !world;
    shown_battle = battle;
}

/* The PS1 button icons the game draws (g_uiSprites: Cross 4, Circle 5, Square 6, Triangle 7, L1 8, L2 9, R1 10, R2 11),
 * for the renderer to draw the 3DS's buttons in their place (port/3ds/render.c): per 3DS button A, B, X, Y, L, R, ZL,
 * ZR, the sprite's texture page, CLUT, uv and size as {tx, ty, clut_x, clut_y, u, v, w, h}. */
int port_button_sprites(int out[8][8]) {
    #if VERSION_JP
    static const u8 ids[8] = {5, 4, 7, 6, 8, 10, 9, 11}; /* (A = circle, B = cross: the Japanese release confirms with circle) */
#else
    static const u8 ids[8] = {4, 5, 7, 6, 8, 10, 9, 11}; /* (A = cross, B = circle, X = triangle, Y = square, ...) */
#endif
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
#define TOP_WORDS 0x8000 /* (a shop's top view takes 37 KB: South Gate's rest stop, disc 2, overran 0x2000 words) */
static u32 top_mem[OTN + TOP_WORDS];

static void big_portrait(int slot, Character *c, int x, int y, int size) {
    const u8 *e = port_portrait(g_portraitByKind[c->info.b.kind]);
    if (!e) return;
    ctr_hud_image(slot, 0x300u | (u32)g_portraitByKind[c->info.b.kind], (const u16 *)e,
                  port_portrait_face(g_portraitByKind[c->info.b.kind]), 32, 32);
    ctr_hud_draw_top(slot, x, y, 32, 32, size, size);
}

void port_big_portrait(int slot, Character *c, int x, int y, int size) { big_portrait(slot, c, x, y, size); } /* (rewards_top.c) */

static void stat(int x, int y, const char *label, int value) {
    u8 a[12];
    ui_drawLabel(x, y, 2, (s32)label, 0);
    text_formatNumber(value, a, 4, 1);
    ui_drawDigits(x + 72, y - 1, 2, a, 0);
}

static void hp_mp(int x, int y, Character *c) {
    static const u8 hp[] = TX("HP", "PV", "HP", "HP", "HP", "HP"), mp[] = TX("MP", "PM", "MP", "MP", "MP", "MP");
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
    static const u8 lv[] = TX("LV", "NV", "LV", "LV", "NV", "Lv"), exp[] = TX("EXP", "EXP", "EXP", "EP", "EXP", "EXP"), stats[] = "STATUS", equip[] = "EQUIPMENT";
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
    static const u8 lv[] = TX("LV", "NV", "LV", "LV", "NV", "Lv");
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
int port_menu_top_draw(void);
static int top_draw(void) {
    int port_battle_help_top(void);
    if (g_gameState->mode == 2 && !port_menu_state()) return port_battle_help_top(); /* (battle_ui.c) */
    int port_tm_top_draw(void);
    if (g_gameState->mode == GS_MODE_TETRA_MASTER) return port_tm_top_draw(); /* (tm_ui.c) */
    int port_rewards_top(void);
    if (g_gameState->mode == 8) return port_rewards_top(); /* (the battle's rewards: rewards_top.c) */
    if (port_menu_state() != 2 && !(g_menu.open && g_menu.type)) return 0;
    (void)member_sheet;
    (void)party_overview;
    int port_help_top_draw(void);
    int views = port_menu_top_draw(); /* (menu_top.c) */
    return port_help_top_draw() | views; /* (drawn after: in front) */
}

/* The event timer (an escape's countdown): the game draws it into its frame each VBlank (timer_draw, wrapped:
 * config/renames.txt). While a menu is up the frame is the menu, on the bottom screen, and the top keeps the scene as
 * it was, dimmed, its timer stopped in it: then it is drawn on the top instead, over the scene where the scene shows
 * it (the display's offset on the top screen), counting and undimmed, and not on the bottom (game_updateTimer, its
 * entry from outside, wrapped: timer_update's own call to timer_draw is inside their object). */
void game_updateTimer_game(void);
void game_updateTimer(void) {
    if (!(port_menu_state() == 1 || port_menu_state() == 2)) {
        game_updateTimer_game();
        return;
    }
    g_clock.pktPtr = &g_clock.pkt[g_gameState->gameData->drawBuf << 9]; /* (timer_update's, less the drawing) */
    game_tickClock();
}
static int top_timer(u32 *ot) {
    if (!(port_menu_state() == 1 || port_menu_state() == 2) || !timer_isEnabled()) return 0;
    PsxDisplayInfo d;
    psx_gpu_display_info(&d);
    int ox = (400 - (d.w > 400 ? 400 : d.w)) / 2, oy = (240 - (d.h > 240 ? 240 : d.h)) / 2;
    GameData *g = g_gameState->gameData;
    u32 *saved_ot = g->ot[g->drawBuf];
    u8 *saved_pkt = g_clock.pktPtr;
    u32 *p = (u32 *)g_primPtr;
    p[0] = 1u << 24; p[1] = 0xE5000000u; /* (offset back to 0 after it: drawn last, so linked first) */
    addPrim(&ot[1], p);
    p += 2;
    g->ot[g->drawBuf] = ot; /* (timer_draw links at ot[drawBuf] + 1) */
    g_clock.pktPtr = (u8 *)p;
    timer_draw();
    p += 0x200 / 4; /* (its packets: within the 0x200 bytes the game gives it a frame; it keeps its cursor local) */
    g->ot[g->drawBuf] = saved_ot;
    g_clock.pktPtr = saved_pkt;
    /* an opaque box under it: the scene's own timer, stopped and dimmed, doesn't show through its translucent one */
    p[0] = 3u << 24; p[1] = 0x60000000u; p[2] = (u32)(g_timerRect.x + 2) | (u32)(g_timerRect.y + 2) << 16;
    p[3] = (u32)(g_timerRect.w - 4) | (u32)(g_timerRect.h - 4) << 16;
    addPrim(&ot[1], p);
    p += 4;
    p[0] = 1u << 24; p[1] = 0xE5000000u | ((u32)ox & 0x7FF) | ((u32)oy & 0x7FF) << 11; /* (drawn first) */
    addPrim(&ot[1], p);
    p += 2;
    g_primPtr = (u8 *)p;
    return 1;
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
    drew |= top_timer(ot);
    unsigned used = (unsigned)(g_primPtr - (u8 *)top_mem);
    g_ot = saved_ot;
    g_primPtr = saved_prim;
    if (used > sizeof top_mem) port_fatal("top screen UI: packet buffer overrun");
    if (drew) ctr_top_ui((unsigned *)&ot[OTN - 1], top_mem, sizeof top_mem);
}
