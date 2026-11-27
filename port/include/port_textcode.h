/* The game's text, byte by byte as the port walks it (the dialogue log, the choices, the help, the place names): which
 * bytes lead a two-byte character, whose second byte can be anything (an escape's or a newline's value too). As the
 * game's text_buildBlock / text_measure decide: in the Japanese release a code at or above the kanji font's
 * codeBase >> 8 (when an area has loaded one: GameData.kanjiFont) or the main font's extCode >> 8; elsewhere the
 * message windows' 0xFA-0xFD. Escapes, newlines, page breaks and the end are never leads. */
#ifndef PORT_TEXTCODE_H
#define PORT_TEXTCODE_H
static inline int port_text_lead(unsigned char c) {
    if (c == TEXT_ESC || c == TEXT_NEWLINE || c == TEXT_END || c == 0xFE || c == TEXT_SPACE_1PX) return 0;
#if VERSION_JP
    if (!g_gameState || !g_gameState->gameData) return 0;
    Font *kanji = g_gameState->gameData->kanjiFont, *font = g_gameState->gameData->font;
    if (kanji && c >= (kanji->codeBase >> 8)) return 1;
    return font && c >= (font->extCode >> 8);
#else
    return c >= TEXT_WIDE_FIRST && c <= TEXT_WIDE_LAST;
#endif
}
#endif
