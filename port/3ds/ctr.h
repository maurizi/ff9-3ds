/* 3DS platform internals shared by port/3ds. */
#ifndef CTR_H
#define CTR_H

#include <stdarg.h>

int ctr_is_new3ds(void);
void ctr_shutdown(void);

/* console.c: the bottom-screen log (until the native UI owns that screen) */
void ctr_console_init(void);
void ctr_console_vlog(const char *fmt, va_list ap);
void ctr_console_show(void);

/* video.c */
void ctr_video_init(void);
void ctr_present(long frame);

/* audio.c: NDSP output of the SPU's 44.1 kHz stereo mix */
void ctr_audio_init(void);
void ctr_audio_exit(void);

/* touch.c: PS1 buttons the touch screen holds this frame */
unsigned int ctr_ui_pad_buttons(long frame);

/* render.c: the UI layer's pointing hands (the game's cursor) last drawn, in bottom-screen coordinates */
#define CTR_MAX_HANDS 4
extern int ctr_ui_hands[CTR_MAX_HANDS][2], ctr_ui_nhands;
int ctr_ui_active(void); /* the bottom screen shows a UI layer */

/* async.c: the worker on the other core (plat_async) */
void ctr_async_init(void);
void ctr_async_exit(void);

/* voice.c: a voice pack's recording of a dialogue line, while its window is up */
void ctr_voice_line(unsigned bank, unsigned message);
void ctr_voice_stop(void);
void ctr_voice_exit(void);

/* profile.c */
void ctr_profile_frame(void);

#endif
