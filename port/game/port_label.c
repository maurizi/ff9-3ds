/* The port's labels in the Japanese release: its label font (ui_drawLabel) has only ASCII capitals, digits and '&',
 * so a label with kana (port_text.h's TX, Japanese strings) is drawn as the game draws its own katakana labels, with
 * the small font (text_buildSmall: 7 pixels a character, '@' switching katakana / hiragana, the voicing marks 0xDE /
 * 0xDF over the character before). Other labels, and every release but Japanese, go to the game's ui_drawLabel. */
#define M2CTX
#include "ff9.h"

#if VERSION_JP
static int kana(const u8 *s) {
    for (; *s; s++)
        if (*s >= 0xA0 || *s == '@') return 1;
    return 0;
}

s32 port_ui_measureLabel(u8 *s) {
    if (!kana(s)) return ui_measureLabel(s);
    s32 w = 0;
    for (; *s; s++)
        if (*s != '@' && *s != 0xDE && *s != 0xDF) w += 7;
    return w;
}

void port_ui_drawLabel(s32 x, s32 y, s32 otz, s32 str, s32 mode) {
    u8 *s = (u8 *)str;
    if (!kana(s)) {
        ui_drawLabel(x, y, otz, str, mode);
        return;
    }
    /* (the label modes as the small font's palettes: normal, the emphasis the port draws masked, dim) */
    s32 pal = mode == UI_LABEL_NORMAL ? TEXT_PAL_WHITE : mode == UI_LABEL_MASK ? TEXT_PAL_YELLOW : TEXT_PAL_GREY;
    ui_drawSmallDigits(x, y - 1, otz, s, pal); /* (8 lines high, the label font's 7: a line up) */
}
#endif
