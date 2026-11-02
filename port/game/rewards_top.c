/* The battle rewards on the top screen (docs/ui-design.md): while the PS1's reward windows run on the bottom screen
 * (A advances them as before), the top shows all of it at once: a card per party member (portrait, name, level, an
 * EXP bar towards the next level, the EXP and AP gained, LEVEL UP as it happens, the abilities learned) and a strip
 * with the gil, the items and the card won. Everything is read from the rewards screen's own state as it counts
 * (ovl_65 s_rewards), so the cards fill as the bottom's numbers run. Drawn with the game's window and text code into
 * field_hud.c's top-screen list. */
#define M2CTX
#include "ovl_65.h"

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
