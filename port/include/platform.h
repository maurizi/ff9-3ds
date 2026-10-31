/* What port/psx needs from the platform (port/ds, port/pc). Kept small: everything else is shared. */
#ifndef PLATFORM_H
#define PLATFORM_H

#include <stdarg.h>

/* Block until the next 60 Hz frame boundary (DS: the real VBlank; PC: a 60 Hz pace, or none when headless). */
void plat_wait_frame(void);

/* A VSync call is about to run `frames` virtual frames (plat_wait_frame each): VBlanks that passed while the game was
 * working count toward them, as the PS1's VSync(n) counts them, so a slow frame doesn't wait again (DS display.c). */
void plat_vsync_begin(unsigned int frames);

/* Called once the PS1 memory (VRAM/SPU shadows) is allocated: platform video setup. */
void plat_video_init(void);

/* Interleaved signed PCM16 stereo at the PSX's 44.1 kHz sample clock. */
void plat_audio_samples(const short *samples, unsigned frames);

/* Debug output (DS: bottom-screen console; PC: stderr). */
void plat_vlog(const char *fmt, va_list ap);

/* Stop for good: after a fatal error. */
void plat_halt(void) __attribute__((noreturn));

/* Directory holding the game data (ff9ds/ on the SD card, or a path given on the PC command line), and the
 * raw disc images for the read-the-image-directly CD backend. */
const char *plat_data_dir(void);

/* Run fn(arg) on another CPU core, one job at a time; 0 if there is none (the caller then runs it itself). The job
 * must touch only memory the caller leaves alone until plat_async_wait() returns. (3DS: the second core; PC: none.) */
int plat_async(void (*fn)(void *), void *arg);
void plat_async_wait(void);

#endif
