/* The port's own labels in the release's language: TX("NEW GAME", "NUEVA PARTIDA", "NUOVA PARTITA", "NEUES SPIEL")
 * is the string of the release the program is built for (NTSC-U, Spanish, Italian, German). They are drawn by the
 * game's label font (ui_drawLabel): capitals, digits, space and & . _, and in the PAL releases' font also ! and the
 * accents, written before their letter as the games' own label strings write them: '#' acute (P#AG: PÁG), '$' grave
 * (ABILIT$A: ABILITÀ), '"' umlaut (AUSR"USTUNG: AUSRÜSTUNG). No apostrophe, hyphen or colon. The games' own words are
 * used where their label tables have one (GRUPO, SQUADRA, GRUPPE); each fits the space its NTSC-U label has, measured
 * in the release's font (FF9_LABEL_WIDTHS on the PC build: field_hud.c). */
#ifndef PORT_TEXT_H
#define PORT_TEXT_H

#if VERSION_ES
#define TX(us, es, it, de) es
#elif VERSION_IT
#define TX(us, es, it, de) it
#elif VERSION_DE
#define TX(us, es, it, de) de
#else
#define TX(us, es, it, de) us
#endif

#endif
