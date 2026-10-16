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
#endif
