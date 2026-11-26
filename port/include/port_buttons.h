/* The PS1 pad bits the 3DS's A and B stand for: confirm and cancel, Cross and Circle, the other way round in the
 * Japanese release (its confirm is Circle). A confirms and B cancels in every release, as on every Nintendo system;
 * the port's touch screens confirm and cancel with the same bits. */
#ifndef PORT_BUTTONS_H
#define PORT_BUTTONS_H
#if VERSION_JP
#define PORT_PAD_CONFIRM 0x2000u /* Circle */
#define PORT_PAD_CANCEL 0x4000u  /* Cross */
#else
#define PORT_PAD_CONFIRM 0x4000u /* Cross */
#define PORT_PAD_CANCEL 0x2000u  /* Circle */
#endif
#endif
