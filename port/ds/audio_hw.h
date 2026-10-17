#ifndef AUDIO_HW_H
#define AUDIO_HW_H
/* DS hardware mixing for the emulated SPU (audio_hw.c, docs/design.md §7.0). */
void port_hw_sound_init(void);
void port_hw_sound_frame(void); /* once per emulated frame, after the SPU advanced */
extern int port_hw_sound_stats[4]; /* channel starts, voices waiting for a channel, voices sounding */
#endif
