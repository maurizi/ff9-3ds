/* 3DS platform internals shared by port/3ds/*.c. */
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

/* ui.c: PS1 buttons the touch screen holds this frame */
unsigned int ctr_ui_pad_buttons(void);

/* profile.c */
void ctr_profile_frame(void);

#endif
