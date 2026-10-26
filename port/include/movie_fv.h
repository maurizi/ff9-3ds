/* Full-screen movies from FastVideoDS transcodes (port/ds/movie_fv.c, docs/movie-validation.md). The game's own
 * player keeps streaming the STR (its audio, camera and end data); while a movie is in its 24-bit mode the frames
 * come from DATA/fmv/dN/LBA.fv instead of the MDEC, onto the top screen directly. Without the decoder blob (fv.bin)
 * or the movie's .fv, movies play through the MDEC as before. The PC build has none of this (port/pc). */
#ifndef MOVIE_FV_H
#define MOVIE_FV_H

/* Start playing movie `lba` (the STR's first sector) from its transcode, the decoder in `work` (the player's VLC
 * buffers, which these frames don't use); 1 if it plays. */
int movie_fv_start(void *work, unsigned int size, unsigned int lba);
/* The player is about to show STR frame `frame` (1-based). */
void movie_fv_frame(unsigned int frame);
void movie_fv_stop(void);
int movie_fv_active(void);

/* A 15-bit MBG (behind a field) from its transcode, decoded by the CPU into the player's MDEC slices: start (1 if it
 * plays), the player starting STR frame `frame` (1-based; 0 if it couldn't be decoded), a slice of columns
 * [x, x + w), h rows, filled row by row (`stp` the MDEC's bit 15), stop. */
int movie_fv_mbg_start(void *work, unsigned int size, unsigned int lba);
int movie_fv_mbg_frame(unsigned int frame);
void movie_fv_mbg_slice(unsigned short *dst, int x, int w, int h, unsigned int stp);
void movie_fv_mbg_stop(void);
int movie_fv_mbg_active(void);
/* gx_hybrid.c: banks B-D to a movie behind the field and back (1 if taken) */
int gxh_movie_banks(int take);

/* display.c: the VBlank interrupt, and the game's waits for VBlanks (pace) */
void movie_fv_vblank(void);
void movie_fv_idle(void);
/* display.c: the movie takes the main engine (banks A-E, 3D, capture) and gives it back; the next present sets
 * the display up again */
void display_movie_take(void);
void display_movie_release(void);

#endif
