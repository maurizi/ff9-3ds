/* Voice acting: which dialogue line is on screen, for a voice pack (port/3ds/voice.c plays it).
 * msg_open is wrapped (config/renames.txt): every line the game shows (the event script's message opcodes, Mognet,
 * battle text) opens through it. A line is named by its text bank and its number in it (MsgWindow.msgId): the bank
 * is the field's id in a field (its text block is the field's), 10000 + the battle's id in a battle, 20000 on the
 * world map, 30000 + the mode otherwise. Its window closes by several paths (the player's confirm, the script, a
 * close-all); the port watches the window each frame instead (port_voice_poll) and stops the voice when it shuts or
 * shows another line. Without a pack (and on the PC) nothing happens. Each line also goes to the dialogue log. */
#define M2CTX
#include "ff9.h"

void msg_open_game(s32 msg, s32 id, WinLayout *l, s32 flags);
__attribute__((weak)) void ctr_voice_line(unsigned bank, unsigned message) { (void)bank; (void)message; }
__attribute__((weak)) void ctr_voice_stop(void) {}
void port_log(const char *fmt, ...);

static int voiced = -1, voiced_msg; /* the window whose line is voiced */

static unsigned text_bank(void) {
    GameData *g = g_gameState ? g_gameState->gameData : NULL;
    if (!g) return 39999;
    switch (g_gameState->mode) {
    case 1: return g->fieldId;
    case 2: return 10000u + g->battleId;
    case 3: return 20000u;
    default: return 30000u + g_gameState->mode;
    }
}

void msg_open(s32 msg, s32 id, WinLayout *l, s32 flags) {
    msg_open_game(msg, id, l, flags);
    if (id < 0 || id >= 10 || !g_msgWindows[id].open || g_msgWindows[id].msgId != msg) return; /* (not shown) */
    unsigned bank = text_bank();
#ifdef PORT_VOICE_LOG
    port_log("voice: line %u/%d (window %d)\n", bank, msg, id);
#endif
    voiced = id;
    voiced_msg = msg;
    ctr_voice_line(bank, (unsigned)msg);
    void port_dialogue_log(const u8 *text);
    port_dialogue_log(msg_drawText_msg(msg)); /* (the bottom screen's dialogue log: field_hud.c) */
}

/* each frame: the voiced line's window closed, or shows another line → its voice stops */
void port_voice_poll(void) {
    if (voiced < 0) return;
    MsgWindow *w = &g_msgWindows[voiced];
    if (w->open && w->msgId == voiced_msg) return;
    voiced = -1;
    ctr_voice_stop();
}
