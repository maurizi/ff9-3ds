#ifndef MOVIE_STREAM_H
#define MOVIE_STREAM_H
#include <stddef.h>
#include <stdint.h>

enum { FF9_MOVIE_CHUNKS=8, FF9_MOVIE_CHUNK_BYTES=0x8f4 };
/* One compressed frame, not decoded pixels. Caller owns storage and lifetime.
 * Audio sectors retain camera/flag metadata; video headers retain MBG extras. */
typedef struct {
    uint8_t video[FF9_MOVIE_CHUNKS*FF9_MOVIE_CHUNK_BYTES];
    uint8_t audio[2][2048];
    uint8_t video_header[FF9_MOVIE_CHUNKS][32]; /* physical sectors 2..9 */
    uint32_t number, video_bytes;
    uint16_t width, height;
    uint8_t next_sector;
    int error;
} Ff9MovieFrame;

enum {
    FF9_MOVIE_MORE=0, FF9_MOVIE_READY=1,
    FF9_MOVIE_BAD_HEADER=-1, FF9_MOVIE_BAD_ORDER=-2,
    FF9_MOVIE_BAD_SIZE=-3, FF9_MOVIE_BAD_VIDEO=-4, FF9_MOVIE_BAD_AUDIO=-5
};
enum { FF9_MOVIE_AUDIO_NONE=0, FF9_MOVIE_AUDIO_DATA=1, FF9_MOVIE_AUDIO_END=2 };
typedef struct {
    const uint8_t *adpcm; /* borrowed from the frame's audio sector */
    uint32_t number;
    uint16_t bytes, pitch;
} Ff9MovieAudio;
/* Returns NONE, DATA, END, or BAD_AUDIO; metadata remains in the raw sector. */
int ff9_movie_audio_info(const uint8_t sector[2048], Ff9MovieAudio *info);
void ff9_movie_frame_reset(Ff9MovieFrame *frame);
/* Mode 2 user bytes only: exactly 2048 for audio or 2324 for video.
 * Errors latch until reset. READY remains readable until the next push.
 * A reset allows seeking directly to any complete frame's sector zero. */
int ff9_movie_frame_push(Ff9MovieFrame *frame, const uint8_t *sector, size_t bytes);
#endif
