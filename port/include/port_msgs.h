/* Field message numbers the port acts on (MsgWindow.msgId), for the release it is built for. A translation can add or
 * drop lines in a field's text, so a line can have another number: these were found by aligning each release's event
 * script with NTSC-U's (the same instructions, only the message operands differ). Unchanged in every release: the
 * sword fight's (field 64, 0x6A-0x80), the jump rope's (103) and Hippaul's (1850). */
#ifndef PORT_MSGS_H
#define PORT_MSGS_H

/* field 657, frog catching: "Frogs Captured: N" */
#if VERSION_ES
#define MSG_FROG_COUNT 0x182
#elif VERSION_DE || VERSION_FR
#define MSG_FROG_COUNT 0x184 /* (German, French) */
#else
#define MSG_FROG_COUNT 0x183 /* (NTSC-U, Italian) */
#endif

/* field 909, the Treno auction: the bid windows (NTSC-U 0xC9-0xCE, the bid entry 0xCB); one later in PAL */
#if VERSION_PAL
#define MSG_AUCTION_SHIFT 1
#else
#define MSG_AUCTION_SHIFT 0
#endif
#define MSG_AUCTION_BID_FIRST (0xC9 + MSG_AUCTION_SHIFT)
#define MSG_AUCTION_BID_LAST (0xCE + MSG_AUCTION_SHIFT)
#define MSG_AUCTION_BID_ENTRY (0xCB + MSG_AUCTION_SHIFT)

#endif
