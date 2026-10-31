/* The 3DS plays every movie through the MDEC from the disc (movie_fv.h: transcodes were for the DS's hardware). */
#include "movie_fv.h"
#include "platform.h"
#include "psx_runtime.h"

int psx_cd_disc(void);

int movie_fv_start(void *work, unsigned int size, unsigned int lba) {
    (void)work;
    (void)size;
    (void)lba; /* (the 3DS decodes every movie from the disc: no transcodes) */
    return 0;
}

void movie_fv_frame(unsigned int frame) {
    (void)frame;
}

void movie_fv_stop(void) {
}

int movie_fv_active(void) {
    return 0;
}

int movie_fv_mbg_start(void *work, unsigned int size, unsigned int lba) {
    (void)work;
    (void)size;
    (void)lba;
    return 0;
}

int movie_fv_mbg_frame(unsigned int frame) {
    (void)frame;
    return 0;
}

void movie_fv_mbg_slice(unsigned short *dst, int x, int w, int h, unsigned int stp) {
    (void)dst; (void)x; (void)w; (void)h; (void)stp;
}

void movie_fv_mbg_stop(void) {
}

int movie_fv_mbg_active(void) {
    return 0;
}
