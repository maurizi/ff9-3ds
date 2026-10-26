/* Decode one FF9 movie extent of a raw (2352-byte sector) disc image to raw RGB24 frames on stdout, for a build-time
 * transcode (docs/movie-validation.md): the port's own frame assembly (movie_stream.c), BS v2 expansion
 * (movie_vlc.c) and MDEC (mdec_decode.c), with the game's tables (216 bytes of run/level pairs, 128 of quantization,
 * 128 of scale: the movie overlay's g_mbgVlcPairs, D_8019DEC8, D_8019DF4C).
 *
 * Usage: movie_export DISC.img LBA SECTORS TABLES.bin > frames.rgb   (stderr: one line per movie with its frames)
 * Frames are 320x224 RGB24 (the movies' size), one per movie frame (15 a second). A frame that fails to decode is
 * written as the last good one, and reported. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "movie_stream.h"
#include "movie_vlc.h"
#include "mdec_decode.h"

static Ff9MovieFrame frame;
static unsigned char raw[2352];
static unsigned char tables[216 + 128 + 128];
static uint16_t rl[0x8002];
static unsigned char image[224][320][3];
static unsigned get16(const unsigned char *p) { return p[0] | ((unsigned)p[1] << 8); }
static unsigned get32(const unsigned char *p) { return get16(p) | (get16(p + 2) << 16); }

/* the frame's macroblocks, top to bottom in each 16-pixel column, columns left to right (the player's slices) */
static int decode_frame(const PsxMdecEnv *env) {
    Ff9MovieVlcInfo info;
    if (ff9_movie_vlc_v2(frame.video, frame.video_bytes, tables, rl, sizeof rl / sizeof *rl, &info)) return -1;
    uint32_t command = rl[0] | (uint32_t)rl[1] << 16;
    int rgb15 = (command >> 27) & 1;
    command &= ~0x08000000u; /* 24-bit output */
    const uint16_t *p = rl + 2;
    size_t left = info.halfwords > 2 ? info.halfwords - 2 : 0;
    unsigned char mb[16 * 16 * 3];
    (void)rgb15;
    for (unsigned x = 0; x < frame.width; x += 16)
        for (unsigned y = 0; y < frame.height; y += 16) {
            size_t used = 0;
            if (psx_mdec_macroblock(env, p, left, command, mb, sizeof mb, &used)) return -2;
            p += used;
            left -= used;
            for (unsigned r = 0; r < 16 && y + r < 224; ++r)
                for (unsigned c = 0; c < 16 && x + c < 320; ++c) memcpy(image[y + r][x + c], mb + (r * 16 + c) * 3, 3);
        }
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 5) { fprintf(stderr, "usage: movie_export DISC.img LBA SECTORS TABLES.bin\n"); return 2; }
    FILE *t = fopen(argv[4], "rb");
    if (!t || fread(tables, 1, sizeof tables, t) != sizeof tables) return 2;
    fclose(t);
    PsxMdecEnv env;
    psx_mdec_env_init(&env, tables + 216, tables + 216 + 128);
    FILE *f = fopen(argv[1], "rb");
    unsigned long start = strtoul(argv[2], NULL, 0), sectors = strtoul(argv[3], NULL, 0);
    if (!f || fseek(f, (long)(start * 2352), SEEK_SET)) return 2;
    unsigned frames = 0, declared = 0, bad = 0;
    for (unsigned long i = 0; i < sectors; i++) {
        if (fread(raw, 1, sizeof raw, f) != sizeof raw) break;
        if (declared && frames == declared) break; /* (the extent's frame-zero tail) */
        int result = ff9_movie_frame_push(&frame, raw + 24, (raw[18] & 0x20) ? 2324 : 2048);
        if (result < 0) { fprintf(stderr, "framing error %d at LBA %lu\n", result, start + i); ff9_movie_frame_reset(&frame); continue; }
        if (result != FF9_MOVIE_READY) continue;
        declared = get32(frame.audio[1] + 0x60);
        if (frame.width != 320 || frame.height != 224 || decode_frame(&env)) bad++;
        fwrite(image, 1, sizeof image, stdout);
        frames++;
    }
    fclose(f);
    fprintf(stderr, "frames %u declared %u bad %u\n", frames, declared, bad);
    return 0;
}
