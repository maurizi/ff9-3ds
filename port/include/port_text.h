/* The port's own labels in the release's language: TX("NEW GAME", "NUEVA PARTIDA", "NUOVA PARTITA", "NEUES SPIEL", "NOUVELLE PARTIE", "\xC6\xAD\xB0\xB9\xDE\xB0\xD1")
 * is the string of the release the program is built for (NTSC-U, Spanish, Italian, German, French, Japanese; the PAL English
 * release takes the NTSC-U one, as its own menus mostly keep NTSC-U's words). They are drawn by the
 * game's label font (ui_drawLabel): capitals, digits, space and & . _, and in the PAL releases' font also ! and the
 * accents, written before their letter as the games' own label strings write them: '#' acute (P#AG: PÁG), '$' grave
 * (ABILIT$A: ABILITÀ), '"' umlaut (AUSR"USTUNG: AUSRÜSTUNG). No apostrophe, hyphen or colon. The games' own words are
 * used where their label tables have one (GRUPO, SQUADRA, GRUPPE); each fits the space its NTSC-U label has, measured
 * in the release's font (FF9_LABEL_WIDTHS on the PC build: field_hud.c). */
#ifndef PORT_TEXT_H
#define PORT_TEXT_H

#if VERSION_ES
#define TX(us, es, it, de, fr, jp) es
#elif VERSION_IT
#define TX(us, es, it, de, fr, jp) it
#elif VERSION_DE
#define TX(us, es, it, de, fr, jp) de
#elif VERSION_FR
#define TX(us, es, it, de, fr, jp) fr
#elif VERSION_JP
#define TX(us, es, it, de, fr, jp) jp
#else
#define TX(us, es, it, de, fr, jp) us
#endif
/* Japanese: kana labels in the small font (port_label.c: katakana, '@' switching to hiragana and back); the label
 * calls of the files that use TX go there */
#if VERSION_JP
s32 port_ui_measureLabel(u8 *);
void port_ui_drawLabel(s32, s32, s32, s32, s32);
#define ui_measureLabel port_ui_measureLabel
#define ui_drawLabel port_ui_drawLabel
#endif

#endif
