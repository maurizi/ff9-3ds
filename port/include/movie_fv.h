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

/* display.c: the VBlank interrupt, and the game's waits for VBlanks (pace) */
void movie_fv_vblank(void);
void movie_fv_idle(void);
/* display.c: the movie takes the main engine (banks A-E, 3D, capture) and gives it back; the next present sets
 * the display up again */
void display_movie_take(void);
void display_movie_release(void);

#endif
