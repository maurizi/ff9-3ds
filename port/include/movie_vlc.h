#ifndef MOVIE_VLC_H
#define MOVIE_VLC_H
#include <stddef.h>
#include <stdint.h>

typedef struct { unsigned halfwords, blocks, consumed_bits; } Ff9MovieVlcInfo;
enum { FF9_VLC_OK=0, FF9_VLC_HEADER=-1, FF9_VLC_TRUNCATED=-2,
       FF9_VLC_CAPACITY=-3, FF9_VLC_CODE=-4 };
/* Expand FF9's BS v2 stream to an MDEC command followed by run-level codes.
 * ac_pairs is the user's 108 {run,positive_level} pairs (D_8019DCFC), not a
 * newly allocated lookup table. Capacity counts uint16_t elements, including
 * the command. No allocations or global state; partial output on error.
 * Remaining declared MDEC words are padded with FE00, as in func_8019AEC0.
 * This stage does not inverse-quantize, perform IDCT or produce pixels. */
int ff9_movie_vlc_v2(const uint8_t *bs, size_t bytes, const uint8_t ac_pairs[216],
                     uint16_t *out, size_t capacity, Ff9MovieVlcInfo *info);
#endif
