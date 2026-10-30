#ifndef PSX_SPU_H
#define PSX_SPU_H
#include <stdint.h>

typedef struct { uint32_t counter, level; } PsxSpuNoise;
void psx_spu_noise_tick(PsxSpuNoise *noise, unsigned clock);

/* Register offsets are bytes relative to 0x1f801c00. All writes are explicit,
 * including repeated values. Voice offsets use the same hardware layout. */
void psx_spu_reset(void);
uint16_t psx_spu_read(unsigned offset);
void psx_spu_write(unsigned offset, uint16_t value);
void psx_spu_voice_register(unsigned voice, unsigned offset, uint16_t value);

/* Enable only when a PCM sink is available: host --audio/--spu-trace or
 * the DS stream setup. SpuInit/reset preserves this setting. */
void psx_spu_set_rendering(int enabled);
/* Root counter clock is master/8: one output sample per 96 ticks. */
void psx_spu_advance(unsigned ticks);

/* Hardware mixing (design §7.0): psx_spu_advance only steps the voices the game observes; each decoded block
 * goes to sink->block in play order and each key-on to sink->key_on (before its first block). After each advance
 * the platform reads psx_spu_voice_output. */
typedef struct {
    void (*key_on)(unsigned voice, void *user);
    void (*block)(unsigned voice, const int16_t *samples, void *user); /* 28 samples */
    /* where to decode the voice's next 28 samples directly (4-byte aligned; *negate: negated), else NULL: they go to
     * block (NULL: always). PSX_SPU_UNHEARD: nowhere, only the block's flags are read (its predictor history then
     * stays as it was: the next block heard starts off it). */
    int16_t *(*target)(unsigned voice, int *negate, void *user);
    void *user;
} PsxSpuSink;
#define PSX_SPU_UNHEARD ((int16_t *)1)
void psx_spu_set_hw(const PsxSpuSink *sink); /* NULL: software mixing */
typedef struct {
    int on;               /* the voice is stepping (envelope not off) */
    unsigned pitch;       /* 4096 = 44.1 kHz */
    int left, right;      /* -32768..32767: envelope x voice volume x main volume (x2^15 scale) */
    int noise;
} PsxSpuVoiceOutput;
void psx_spu_voice_output(unsigned voice, PsxSpuVoiceOutput *out);
unsigned psx_spu_noise_hz(void);
/* Reverb as the game set it, for the hardware path's echo: on when enabled (SPUCNT bit 7), some voice sends to it
 * (EON) and the output volume isn't silent; the output volumes (vLOUT, vROUT, signed) and the work area's size in
 * bytes (from mBASE to the end of SPU RAM, which sets how long the effect lasts). */
typedef struct { int on, left, right; unsigned area; } PsxSpuReverbState;
void psx_spu_reverb_state(PsxSpuReverbState *out);

/* Diagnostics: the complete SPU state except its RAM, so a benchmark can replay a moment of the game
 * (host --spu-snapshot, make spu-snapshot-ds). Plain fixed-width fields: the same bytes on host and DS. */
enum { PSX_SPU_SNAPSHOT_MAGIC = 0x50555331 /* "SPU1" */ };
unsigned psx_spu_snapshot_size(void);
void psx_spu_snapshot(void *out);
int psx_spu_restore(const void *in);
#endif
