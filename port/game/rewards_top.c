/* The battle rewards on the top screen (docs/ui-design.md): while the PS1's reward windows run on the bottom screen
 * (A advances them as before), the top shows all of it at once: a card per party member (portrait, name, level, an
 * EXP bar towards the next level, the EXP and AP gained, LEVEL UP as it happens, the abilities learned) and a strip
 * with the gil, the items and the card won. Everything is read from the rewards screen's own state as it counts
 * (ovl_65 s_rewards), so the cards fill as the bottom's numbers run. Drawn with the game's window and text code into
 * field_hud.c's top-screen list. */
#define M2CTX
#include "ovl_65.h"
#include "psx_display.h"

void port_big_portrait(int slot, Character *c, int x, int y, int size);

static void rect(int x, int y, int w, int h, u32 bgr) {
    u32 *p = (u32 *)g_primPtr;
    p[0] = 3u << 24;
    p[1] = 0x60000000u | bgr;
    p[2] = (u32)x | (u32)y << 16;
    p[3] = (u32)w | (u32)h << 16;
    addPrim(&g_ot[2], p);
    g_primPtr = (u8 *)(p + 4);
}

static const u8 *ability_name(int id) {
    if (id < ABILITY_SUPPORT_FIRST) return s_rewardsActionNames + s_rewardsActionTable[id].nameOffset;
    return s_rewardsSupportNames + g_supportAbilities[id - ABILITY_SUPPORT_FIRST].name;
}

static void member(int i, int x, int y) {
    static RECT box;
    box = (RECT){(s16)x, (s16)y, 190, 92};
    Character *c = g_gameState->gameData->party[i];
    if (!c) return; /* (an empty slot: nothing) */
    u8 a[16];
    port_big_portrait(4 + i, c, x + 8, y + 8, 40);
    text_drawBlock(x + 56, y + 6, 2, c->name, 0);
    ui_drawLabel(x + 146, y + 9, 2, (s32) "LV", 0);
    text_formatNumber(c->level, a, 2, TEXT_NUM_ASCII);
    ui_drawDigits(x + 162, y + 8, 2, a, 0);
    /* EXP towards the next level */
    int lo = c->level > 1 ? (int)g_levelExp[c->level - 1] : 0, hi = c->level < LEVEL_MAX ? (int)g_levelExp[c->level] : 0;
    int fill = hi > lo ? (int)(((long long)((int)c->exp - lo) * 124) / (hi - lo)) : 124;
    if (fill < 0) fill = 0;
    if (fill > 124) fill = 124;
    rect(x + 56, y + 26, 126, 6, 0x302020);
    if (fill) rect(x + 57, y + 27, fill, 4, 0x30C8F0); /* (gold) */
    if (s_rewards.getsExp[i]) {
        ui_drawLabel(x + 56, y + 38, 2, (s32) "EXP", 0);
        a[0] = '+';
        text_formatNumber((s32)s_rewards.rewards.expEach, a + 1, -1, TEXT_NUM_ASCII);
        ui_drawSmallDigits(x + 76, y + 39, 2, a, 0);
    }
    if (s_rewards.getsAp[i]) {
        ui_drawLabel(x + 128, y + 38, 2, (s32) "AP", 0);
        a[0] = '+';
        text_formatNumber((s32)s_rewards.rewards.ap, a + 1, -1, TEXT_NUM_ASCII);
        ui_drawSmallDigits(x + 144, y + 39, 2, a, 0);
    }
    int ups = s_rewards.levelPopups[i].count;
    if (ups > 0) {
        ui_drawLabel(x + 8, y + 54, 2, (s32) "LEVEL UP", 0);
        if (ups > 1) {
            a[0] = 'x';
            text_formatNumber(ups, a + 1, -1, TEXT_NUM_ASCII);
            ui_drawSmallDigits(x + 62, y + 55, 2, a, 0);
        }
    }
    LearnList *l = &s_rewards.learned[i];
    int n = l->count > 2 ? 2 : l->count;
    for (int k = 0; k < n; k++) {
        if (k == 0) ui_drawLabel(x + 8, y + 68, 2, (s32) "LEARNED", 0);
        text_drawBlock(x + 56, y + 64 + k * 13, 2, (u8 *)ability_name(l->ids[k]), 0);
    }
    if (l->count > 2) {
        a[0] = '+';
        text_formatNumber(l->count - 2, a + 1, -1, TEXT_NUM_ASCII);
        ui_drawSmallDigits(x + 160, y + 80, 2, a, 0);
    }
    win_drawBox(&box, 4, WIN_STYLE_BEVEL);
}

/* the top screen's view while the rewards screen is up: none. The top keeps the battle as it ended (its last frame
 * before the exit fade: battle_ui.c, render.c); the numbers are on the bottom, where the game counts them. */
int port_rewards_top(void) { return 0; }

/* ---- the rewards run inside the battle (battle_ui.c: the battle's exit held under its victory pose) ----------------
 * ovl_65's code is native and its data is the overlay image the battle preloads to RAM_SUB_OVL as it ends
 * (btl_preloadExit), with the rewards' data file (FILE_BTL_END_DB) at the battle's load buffer: they start once both
 * are in. Their own setup is rewards_init / rewards_open less what would end the battle's picture: game_initGfx (the
 * display and draw buffers), the vsync mode and the fade in from black. They draw into the battle's UI list, which
 * the 3DS shows on its bottom screen. */
int port_rewards_live_state(void);
void port_rewards_live_consumed(void);
s32 o65_rewards_getKind(void);
void o65_rewards_compute(void);
void o65_rewards_clearWinSlot(WinSlot *);
void o65_rewards_initPrimBufs(void);
void o65_rewards_initExp(void);
void o65_rewards_initLearns(void);
void o65_rewards_resetFullWarning(void);
void o65_rewards_loadImages(void);
void o65_rewards_initGil(void);
void o65_rewards_initWindows(void);
u8 o65_rewards_update_game(void);
void o65_rewards_exit_game(void);
void o65_rewards_init_game(void);
s32 o65_battleEnd_loadDb_game(void);
s32 o65_battleEnd_loadPortraits_game(void);
s32 o65_battleEnd_unloadDb_game(void);
s32 preload_isInVram(PreloadEntry *);

int port_rewards_live_eligible(void) { return o65_rewards_getKind() == 1; } /* (a screen, no event script) */

int port_rewards_live_ready(void) {
    if (!preload_isReady(OVL_KEY(OVL_BATTLE_END)) || !preload_isReady(FILE_BTL_END_DB)) return 0;
    PreloadEntry *o = preload_find(OVL_KEY(OVL_BATTLE_END)), *d = preload_find(FILE_BTL_END_DB);
    if (!o || !d || preload_isInVram(o) || preload_isInVram(d)) return 0;
    extern unsigned char psx_ram[];
    return o->dest - (u32)psx_ram == 0x1EDF00; /* (RAM_SUB_OVL, as a native address) */
}

void port_rewards_live_start(void) {
    s32 db = (s32)preload_find(FILE_BTL_END_DB)->dest; /* (as stored: a native address) */
    db_register(db, 0); /* (battleEnd_loadDb's, read where the battle preloaded it) */
    o65_battleEnd_loadPortraits_game();
    db_unregister((void *)db);
    help_setCallback(0);
    s_rewards.screen = 0;
    s_rewards.state = REWARDS_STATE_OPENING;
    s_rewards.frame = 0;
    s_rewards.closeFrames = 0;
    s_rewards.active = 1;
    s_rewards.timer = 0;
    s_rewards.gilSkipped = 0;
    s_rewards.done = 0;
    s_rewards.running = 1;
    s_rewards.closing = 0;
    o65_rewards_compute();
    o65_rewards_clearWinSlot(&s_rewards.fullWin);
    for (int i = 0; i < 4; i++) {
        o65_rewards_clearWinSlot(&s_rewards.levelWins[i]);
        o65_rewards_clearWinSlot(&s_rewards.learnWins[i]);
    }
    text_setBank(g_rewardsText);
    o65_rewards_initPrimBufs();
    o65_rewards_initExp();
    o65_rewards_initLearns();
    o65_rewards_resetFullWarning();
    o65_rewards_loadImages();
    o65_rewards_initGil();
    o65_rewards_initWindows();
}

/* a frame of them (battleEnd_update's, and the window step battleEnd_main does); 0 once they have closed. They draw
 * into an ordering table and packet buffer of their own (mode 8's are GameData.ot, OT_LEN entries: the background
 * goes in at OT_LAST, and the battle UI's are 0x18 entries and 8 KB), in PS1 RAM as every linked packet is, one pair
 * per draw buffer; port_rewards_live_draw puts it on the bottom screen. */
#define LIVE_PRIMS 0x8000
static u32 *live_ot[2];
static u8 *live_buf[2];
static u32 *live_env;  /* the packet setting the drawing area to the displayed buffer, first in the table */
static int live_cur = -1;
int port_rewards_live_step(void) {
    void *psx_spare_alloc(unsigned int size);
    if (!live_buf[0])
        for (int i = 0; i < 2; i++) live_ot[i] = psx_spare_alloc(OT_LEN * 4), live_buf[i] = psx_spare_alloc(LIVE_PRIMS);
    int cur = g_gameState->gameData->drawBuf & 1;
    u32 *saved_ot = g_ot;
    u8 *saved_prim = g_primPtr;
    ClearOTagR(live_ot[cur], OT_LEN);
    g_ot = live_ot[cur];
    g_primPtr = live_buf[cur];
    /* (the battle UI reads the pad at each VBlank and the battle steps every few: a press is a press since the last
     * step, as battleEnd_main's own read each frame would see it) */
    static u32 prev;
    u32 held = g_padPortHeld[0];
    g_padPortPressed[0] = held & ~prev;
    prev = held;
    int more = o65_rewards_update_game();
    game_updateWindows();
    u32 *env = (u32 *)g_primPtr; /* (added last at OT_LAST: drawn first) */
    env[0] = 3u << 24;
    env[1] = 0xE3000000u;
    env[2] = 0xE4000000u;
    env[3] = 0xE5000000u;
    addPrim(&live_ot[cur][OT_LAST], env);
    g_primPtr = (u8 *)(env + 4);
    if (g_primPtr > live_buf[cur] + LIVE_PRIMS) port_fatal("rewards: packet buffer overrun (%d bytes)", (int)(g_primPtr - live_buf[cur]));
    g_ot = saved_ot;
    g_primPtr = saved_prim;
    live_env = env;
    live_cur = cur;
    if (!more) {
        o65_rewards_exit_game(); /* (as battleEnd_update: the VRAM row back) */
        live_cur = -1;
    }
    return more;
}

/* field_hud.c, each frame: the rewards' table on the bottom screen, drawn in the displayed buffer's coordinates as the
 * port's own bottom screen is */
void port_rewards_live_draw(void) {
    if (port_rewards_live_state() != 2 || live_cur < 0) return;
    PsxDisplayInfo d;
    psx_gpu_display_info(&d);
    live_env[1] = 0xE3000000u | (u32)d.x | (u32)d.y << 10;
    live_env[2] = 0xE4000000u | (u32)(d.x + d.w - 1) | (u32)(d.y + d.h - 1) << 10;
    live_env[3] = 0xE5000000u | ((u32)d.x & 0x7FF) | ((u32)d.y & 0x7FF) << 11;
    void psx_ui_list_begin_keep(void);
    void psx_ui_list_end(void);
    void port_draw_ot_buffer(unsigned int *head, const void *buffer, unsigned int size);
    psx_ui_list_begin_keep();
    port_draw_ot_buffer(&live_ot[live_cur][OT_LAST], live_buf[live_cur], LIVE_PRIMS);
    psx_ui_list_end();
}

/* mode 8 after the rewards ran in the battle: nothing more of them (the screen goes straight on, as an empty rewards
 * screen would), the mode's own work (the next scene's preload, the return) as before */
void o65_rewards_init(void) {
    if (port_rewards_live_state() != 3) { o65_rewards_init_game(); return; }
    game_initGfx();
    gfx_setUiMode(0);
    help_setCallback(0);
    s_rewards.running = 0;
    s_rewards.closing = 1;
    s_rewards.done = 1;
}
u8 o65_rewards_update(void) { return port_rewards_live_state() == 3 ? 0 : o65_rewards_update_game(); }
void o65_rewards_exit(void) {
    if (port_rewards_live_state() == 3) { port_rewards_live_consumed(); return; }
    o65_rewards_exit_game();
}
s32 o65_battleEnd_loadDb(void) { return port_rewards_live_state() == 3 ? 0 : o65_battleEnd_loadDb_game(); }
s32 o65_battleEnd_loadPortraits(void) { return port_rewards_live_state() == 3 ? 0 : o65_battleEnd_loadPortraits_game(); }
s32 o65_battleEnd_unloadDb(void) { return port_rewards_live_state() == 3 ? 0 : o65_battleEnd_unloadDb_game(); }
