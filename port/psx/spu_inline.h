/* Per-sample SPU steps as inline functions, shared by the exported single-step functions (spu_source.c,
 * spu_envelope.c) and the block voice renderer (spu_voice.c), so both run the same arithmetic. */
#ifndef PORT_SPU_INLINE_H
#define PORT_SPU_INLINE_H
#include "psx_spu_source.h"
#include "psx_spu_envelope.h"
#include "psx_spu_gauss.h"

static inline int16_t spu_source_interpolate(const PsxSpuSource *source) {
    unsigned phase = (source->counter >> 4) & 255;
    const int16_t *samples = source->samples + (source->counter >> 12);
    /* Accumulate before rounding, as in both reference emulators. The largest
     * absolute coefficient sum is below 32768, so int32 cannot overflow even
     * with four full-scale input samples. See audio-validation.md. */
    int32_t total = samples[0] * psx_spu_gauss[255 - phase];
    total += samples[1] * psx_spu_gauss[511 - phase];
    total += samples[2] * psx_spu_gauss[256 + phase];
    total += samples[3] * psx_spu_gauss[phase];
    return (int16_t)(total >> 15);
}

static inline unsigned spu_source_advance(PsxSpuSource *source, uint16_t pitch,
                                          uint16_t loop_address, int noise_enabled) {
    if (pitch > 0x3fff) pitch = 0x3fff;
    source->counter += pitch;
    if (source->counter < (PSX_SPU_BLOCK_SAMPLES << 12)) return 0;

    source->counter -= PSX_SPU_BLOCK_SAMPLES << 12;
    source->address = (uint16_t)(source->address + 2);
    source->fetched = 0;
    source->first_block = 0;
    if (!(source->flags & 1)) return 0;
    source->address = loop_address & 0xfffeu;
    if (!(source->flags & 2) && !noise_enabled)
        return PSX_SPU_SOURCE_END | PSX_SPU_SOURCE_MUTE;
    return PSX_SPU_SOURCE_END;
}

static inline int spu_envelope_tick(PsxSpuEnvelope *env, int16_t *level) {
    uint32_t increment = env->increment;
    int step = env->step;
    if (env->exponential) {
        if (env->decrease) step = (step * *level) >> 15;
        else if (*level >= 0x6000) {
            if (env->rate < 40) step >>= 2;
            else if (env->rate >= 44) increment >>= 2;
            else { step >>= 1; increment >>= 1; }
        }
    }
    env->counter += increment;
    if (!(env->counter & 0x8000)) return 1;
    env->counter = 0;
    int value = *level + step;
    if (!env->decrease) {
        if (value > 32767) value = 32767;
        if (value < -32768) value = -32768;
        *level = value;
        return value != (step < 0 ? -32768 : 32767);
    }
    if (env->negative) {
        if (value < -32768) value = -32768;
        if (value > 0) value = 0;
        *level = value;
        return value < 0;
    }
    if (value < 0) value = 0;
    *level = value;
    return value > 0;
}

static inline void spu_volume_tick(PsxSpuVolume *volume) {
    if (volume->active)
        volume->active = spu_envelope_tick(&volume->envelope, &volume->level);
}

/* The phase change itself is out of line (rare): spu_envelope.c. */
void psx_spu_adsr_advance_phase(PsxSpuAdsr *adsr);

static inline void spu_adsr_tick(PsxSpuAdsr *adsr) {
    if (adsr->phase == PSX_SPU_ADSR_OFF) return;
    if (adsr->envelope.increment) spu_envelope_tick(&adsr->envelope, &adsr->level);
    if (adsr->phase == PSX_SPU_ADSR_SUSTAIN) return;
    int reached = adsr->envelope.decrease ? adsr->level <= adsr->target : adsr->level >= adsr->target;
    if (reached) psx_spu_adsr_advance_phase(adsr);
}
#endif
