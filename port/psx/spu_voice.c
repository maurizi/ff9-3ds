#include "psx_spu_voice.h"
#include "spu_hot.h"
#include "spu_inline.h"
#include "psx_spu_gauss.h"
#include "profile.h"
#include <string.h>

static SPU_HOT int16_t clamp_sample(int32_t value) {
    if (value > 32767) return 32767;
    if (value < -32768) return -32768;
    return value;
}

SPU_HOT void psx_spu_voice_write(PsxSpuVoice *voice, unsigned index, uint16_t value) {
    if (index >= 8) return;
    voice->registers[index] = value;
    if (index < 2) psx_spu_volume_write(&voice->volume[index], value);
    else if (index == 4 || index == 5)
        psx_spu_adsr_set_registers(&voice->adsr, voice->registers[4], voice->registers[5]);
    else if (index == 6) voice->adsr.level = (int16_t)value;
    else if (index == 7)
        psx_spu_source_set_loop(&voice->source, &voice->registers[7], value,
                                voice->adsr.phase != PSX_SPU_ADSR_OFF);
}

SPU_HOT uint16_t psx_spu_voice_read(const PsxSpuVoice *voice, unsigned index) {
    if (index >= 8) return 0xffff;
    if (index == 6) return (uint16_t)voice->adsr.level;
    return voice->registers[index];
}

SPU_HOT void psx_spu_voice_key_on(PsxSpuVoice *voice) {
    psx_spu_source_key_on(&voice->source, voice->registers[3]);
    psx_spu_adsr_key_on(&voice->adsr);
}

SPU_HOT void psx_spu_voice_key_off(PsxSpuVoice *voice) {
    psx_spu_adsr_key_off(&voice->adsr);
}

SPU_HOT void psx_spu_mix_voices(PsxSpuVoice voices[PSX_SPU_VOICES], const uint8_t *ram,
                         int16_t noise, uint32_t noise_mask, uint32_t modulation_mask,
                         uint32_t reverb_mask, int fetch_off_voices, PsxSpuVoiceMix *mix) {
    memset(mix, 0, sizeof *mix);
    int32_t previous = 0;
    modulation_mask &= ~1u;
    for (unsigned i = 0; i < PSX_SPU_VOICES; ++i) {
        PsxSpuVoice *voice = &voices[i];
        uint32_t bit = 1u << i;
        if (voice->adsr.phase == PSX_SPU_ADSR_OFF && !fetch_off_voices) {
            voice->last_sample = previous = 0;
            continue;
        }
        if (!voice->source.fetched)
            psx_spu_source_fetch(&voice->source, ram, &voice->registers[7]);
        /* Interpolation is read-only. A zero envelope still advances decoder,
         * pitch, loop/end state and ADSR below, but its product is exactly zero. */
        int32_t sample = 0;
        if (voice->adsr.level) {
            sample = (noise_mask & bit) ? noise : psx_spu_source_interpolate(&voice->source);
            sample = (sample * voice->adsr.level) >> 15;
        }
        voice->last_sample = sample;
        /* OFF and a held SUSTAIN return without touching envelope state. */
        if (voice->adsr.phase != PSX_SPU_ADSR_OFF &&
            (voice->adsr.phase != PSX_SPU_ADSR_SUSTAIN || voice->adsr.envelope.increment))
            psx_spu_adsr_tick(&voice->adsr);

        uint16_t pitch = voice->registers[2];
        if (modulation_mask & bit) {
            /* PMON uses signed pitch, then truncates to 16 bits before the
             * source's 0x3fff clamp. Previous voice is AFTER ADSR, before VOLL. */
            int32_t signed_pitch = pitch;
            if (signed_pitch >= 32768) signed_pitch -= 65536;
            pitch = (uint16_t)((signed_pitch * (clamp_sample(previous) + 32768)) >> 15);
        }
        previous = sample;
        unsigned action = psx_spu_source_advance(&voice->source, pitch,
                                                 voice->registers[7], !!(noise_mask & bit));
        if (action & PSX_SPU_SOURCE_END) mix->ended |= bit;
        if (action & PSX_SPU_SOURCE_MUTE) psx_spu_adsr_force_off(&voice->adsr);
        for (unsigned channel = 0; channel < 2; ++channel) {
            int32_t value = (sample * voice->volume[channel].level) >> 15;
            mix->dry[channel] += value;
            if (reverb_mask & bit) mix->reverb_send[channel] += value;
            if (voice->volume[channel].active) psx_spu_volume_tick(&voice->volume[channel]);
        }
        if (i == 1 || i == 3) mix->capture[i >> 1] = clamp_sample(sample);
    }
}

/* Samples a voice can run before its next event, on the steady path: same source block (no fetch, loop or end),
 * no envelope step, no ADSR phase change, constant ENVX and stereo volumes. 0 if the voice isn't steady. */
static SPU_HOT unsigned steady_samples(const PsxSpuVoice *voice, uint16_t pitch, uint32_t *env_increment) {
    const PsxSpuAdsr *adsr = &voice->adsr;
    if (!voice->source.fetched || voice->volume[0].active || voice->volume[1].active) return 0;
    if (pitch > 0x3fff) pitch = 0x3fff;
    unsigned limit = 0xffffffffu;
    if (pitch) limit = ((PSX_SPU_BLOCK_SAMPLES << 12) - 1 - voice->source.counter) / pitch;
    *env_increment = 0;
    /* An off voice still fetching (SPU IRQs enabled) has no envelope ticks: steady until its block ends. */
    if (adsr->phase == PSX_SPU_ADSR_OFF) return limit;
    if (adsr->phase != PSX_SPU_ADSR_SUSTAIN) {
        int reached = adsr->envelope.decrease ? adsr->level <= adsr->target : adsr->level >= adsr->target;
        if (reached) return 0;
    }
    if (adsr->phase != PSX_SPU_ADSR_SUSTAIN || adsr->envelope.increment) {
        const PsxSpuEnvelope *env = &adsr->envelope;
        uint32_t increment = env->increment;
        /* As spu_envelope_tick: only an exponential increase above 0x6000 changes the counter rate. */
        if (env->exponential && !env->decrease && adsr->level >= 0x6000) {
            if (env->rate >= 44) increment >>= 2;
            else if (env->rate >= 40) increment >>= 1;
        }
        if (increment) {
            unsigned ticks = (0x8000 - (env->counter & 0x7fff) + increment - 1) / increment; /* tick that steps */
            if (env->counter & 0x8000) return 0;
            if (ticks - 1 < limit) limit = ticks - 1;
            *env_increment = increment;
        }
    }
    return limit;
}

/* The Gaussian coefficients a source phase uses, in sample order: {g[255-p], g[511-p], g[256+p], g[p]}.
 * The same values and summation order as spu_source_interpolate; one row per phase keeps them together. */
#if defined(PORT_SPU_ITCM) && defined(__arm__)
__attribute__((section(".sbss")))
#endif
static int16_t gauss4[256][4];
static int gauss4_ready;
static void gauss4_init(void) {
    for (unsigned p = 0; p < 256; ++p) {
        gauss4[p][0] = psx_spu_gauss[255 - p];
        gauss4[p][1] = psx_spu_gauss[511 - p];
        gauss4[p][2] = psx_spu_gauss[256 + p];
        gauss4[p][3] = psx_spu_gauss[p];
    }
    gauss4_ready = 1;
}

static inline __attribute__((always_inline)) int32_t gauss_sample(const int16_t *samples, uint32_t counter) {
    const int16_t *at = samples + (counter >> 12), *c = gauss4[(counter >> 4) & 255];
    return at[0] * c[0] + at[1] * c[1] + at[2] * c[2] + at[3] * c[3];
}

/* The steady run's per-sample work: Gaussian interpolation at the moving source position, ENVX and the two
 * volumes applied, added into dry (and reverb send) pairs. Inlined per case, so the inner loop has no tests. */
static inline __attribute__((always_inline)) int32_t steady_mix(
        const int16_t *samples, uint32_t counter, uint32_t pitch, int32_t level, int32_t left, int32_t right,
        int32_t *dry, int32_t *send, int32_t *keep, unsigned run) {
    int32_t sample = 0;
    for (unsigned k = 0; k < run; ++k) {
        sample = ((int16_t)(gauss_sample(samples, counter) >> 15) * level) >> 15;
        counter += pitch;
        int32_t l = (sample * left) >> 15, r = (sample * right) >> 15;
        dry[0] += l;
        dry[1] += r;
        dry += 2;
        if (send) send[0] += l, send[1] += r, send += 2;
        if (keep) *keep++ = sample;
    }
    return sample;
}

static SPU_HOT __attribute__((noinline)) int32_t steady_dry(const int16_t *samples, uint32_t counter, uint32_t pitch,
        int32_t level, int32_t left, int32_t right, int32_t *dry, unsigned run) {
    return steady_mix(samples, counter, pitch, level, left, right, dry, NULL, NULL, run);
}
static SPU_HOT __attribute__((noinline)) int32_t steady_send(const int16_t *samples, uint32_t counter,
        uint32_t pitch, int32_t level, int32_t left, int32_t right, int32_t *dry, int32_t *send, unsigned run) {
    return steady_mix(samples, counter, pitch, level, left, right, dry, send, NULL, run);
}
static SPU_HOT __attribute__((noinline)) int32_t steady_keep(const int16_t *samples, uint32_t counter,
        uint32_t pitch, int32_t level, int32_t left, int32_t right, int32_t *dry, int32_t *send, int32_t *keep,
        unsigned run) {
    return steady_mix(samples, counter, pitch, level, left, right, dry, send, keep, run);
}

/* Samples before the voice's source block ends (no fetch, loop or end inside), when its volumes are constant. */
static SPU_HOT unsigned block_samples(const PsxSpuVoice *voice, uint16_t pitch) {
    if (!voice->source.fetched || voice->volume[0].active || voice->volume[1].active) return 0;
    if (pitch > 0x3fff) pitch = 0x3fff;
    if (!pitch) return 0xffffffffu;
    return ((PSX_SPU_BLOCK_SAMPLES << 12) - 1 - voice->source.counter) / pitch;
}

/* A run inside one source block whose envelope ticks: the single-sample order (sample at the current ENVX,
 * then the ADSR tick, then the position) with the same inline ADSR step, phase changes included. Stops after
 * a sample that switched the voice off when off voices don't fetch. Returns the samples done. */
static SPU_HOT __attribute__((noinline)) unsigned ticking_run(PsxSpuVoice *voice, unsigned run, uint32_t pitch,
        int32_t *dry, int32_t *send, int32_t *keep, int16_t *capture, int fetch_off, int32_t *last) {
    PsxSpuAdsr *adsr = &voice->adsr;
    const int16_t *samples = voice->source.samples;
    const int32_t left = voice->volume[0].level, right = voice->volume[1].level;
    uint32_t counter = voice->source.counter;
    int32_t sample = 0;
    unsigned k = 0;
    while (k < run) {
        sample = 0;
        if (adsr->level) sample = ((int16_t)(gauss_sample(samples, counter) >> 15) * adsr->level) >> 15;
        if (adsr->phase != PSX_SPU_ADSR_OFF &&
            (adsr->phase != PSX_SPU_ADSR_SUSTAIN || adsr->envelope.increment))
            spu_adsr_tick(adsr);
        counter += pitch;
        int32_t l = (sample * left) >> 15, r = (sample * right) >> 15;
        dry[k * 2] += l;
        dry[k * 2 + 1] += r;
        if (send) send[k * 2] += l, send[k * 2 + 1] += r;
        if (keep) keep[k] = sample;
        if (capture) capture[k * 2] = clamp_sample(sample);
        ++k;
        if (adsr->phase == PSX_SPU_ADSR_OFF && !fetch_off) break;
    }
    voice->source.counter = counter;
    *last = sample;
    return k;
}

/* The same arithmetic as psx_spu_mix_voices, for n consecutive samples at once: each voice runs its whole span
 * before the next, with its state kept local. Pitch modulation reads the previous voice's post-ADSR samples
 * from a buffer, so voice order and values are identical. The caller applies key events before the span and
 * must ensure no voice fetches SPU RAM that capture or reverb writes during it (spu_core.c). */
SPU_HOT void psx_spu_mix_voices_block(PsxSpuVoice voices[PSX_SPU_VOICES], const uint8_t *ram, unsigned n,
                                      const int16_t *noise, uint32_t noise_mask, uint32_t modulation_mask,
                                      uint32_t reverb_mask, int fetch_off_voices, PsxSpuVoiceBlock *out) {
    int32_t previous_buffer[2][PSX_SPU_BLOCK_SPAN];
    int32_t *previous = previous_buffer[0], *current = previous_buffer[1];
    if (!gauss4_ready) gauss4_init();
    memset(out, 0, sizeof *out);
    modulation_mask &= ~1u; /* voice 0 is never modulated, so nothing reads a buffer before voice 0's */
    for (unsigned i = 0; i < PSX_SPU_VOICES; ++i) {
        PsxSpuVoice *voice = &voices[i];
        uint32_t bit = 1u << i;
        const int keep = (modulation_mask >> (i + 1)) & 1; /* only a modulated next voice reads `current` */
        if (voice->adsr.phase == PSX_SPU_ADSR_OFF && !fetch_off_voices) {
            voice->last_sample = 0;
            if (keep) memset(current, 0, n * sizeof *current);
        } else {
            const int noisy = !!(noise_mask & bit), modulated = !!(modulation_mask & bit);
            const int reverb = !!(reverb_mask & bit);
            int32_t sample = 0;
            for (unsigned s = 0; s < n; ++s) {
                uint32_t env_increment;
                unsigned run = 0, block_run = 0;
                if (!noisy && !modulated && (block_run = block_samples(voice, voice->registers[2])) != 0)
                    run = steady_samples(voice, voice->registers[2], &env_increment);
                if (block_run >= 2 && run < block_run && run < n - s) {
                    /* The envelope steps or changes phase inside this block: tick it per sample. */
                    unsigned todo = block_run > n - s ? n - s : block_run;
                    uint32_t pitch = voice->registers[2] > 0x3fff ? 0x3fff : voice->registers[2];
                    unsigned done = ticking_run(voice, todo, pitch, out->dry[s], reverb ? out->reverb_send[s] : NULL,
                                                keep ? current + s : NULL,
                                                (i == 1 || i == 3) ? &out->capture[s][i >> 1] : NULL,
                                                fetch_off_voices, &sample);
                    s += done - 1;
                    continue;
                }
                if (run) {
                    /* Steady run: everything but the source position and the envelope counter is constant. */
                    unsigned profile_steady = port_profile_begin();
                    if (run > n - s) run = n - s;
                    uint32_t counter = voice->source.counter, pitch = voice->registers[2];
                    if (pitch > 0x3fff) pitch = 0x3fff;
                    const int32_t level = voice->adsr.level;
                    const int32_t left = voice->volume[0].level, right = voice->volume[1].level;
                    const int capture = i == 1 || i == 3;
                    if (!level) {
                        /* ENVX 0: every product is zero; only the position moves. */
                        sample = 0;
                        if (keep) memset(current + s, 0, run * sizeof *current);
                        if (capture) for (unsigned k = 0; k < run; ++k) out->capture[s + k][i >> 1] = 0;
                    } else if (!keep && !capture) {
                        sample = reverb ? steady_send(voice->source.samples, counter, pitch, level, left, right,
                                                      out->dry[s], out->reverb_send[s], run)
                                        : steady_dry(voice->source.samples, counter, pitch, level, left, right,
                                                     out->dry[s], run);
                    } else {
                        int32_t scratch[PSX_SPU_BLOCK_SPAN], *samples_out = keep ? current + s : scratch;
                        sample = steady_keep(voice->source.samples, counter, pitch, level, left, right,
                                             out->dry[s], reverb ? out->reverb_send[s] : NULL, samples_out, run);
                        if (capture)
                            for (unsigned k = 0; k < run; ++k)
                                out->capture[s + k][i >> 1] = clamp_sample(samples_out[k]);
                    }
                    counter += pitch * run;
                    s += run;
                    voice->source.counter = counter;
                    voice->adsr.envelope.counter += env_increment * run;
                    port_profile_detail_end(PORT_PROFILE_SPU_STEADY, profile_steady);
                    --s; /* the loop's ++s: continue with the event sample */
                    continue;
                }
                if (voice->adsr.phase == PSX_SPU_ADSR_OFF && !fetch_off_voices) {
                    /* Ended inside the span: skipped from here on, as in the single-sample mixer. */
                    current[s] = sample = 0;
                    continue;
                }
                unsigned profile_event = port_profile_begin();
                if (!voice->source.fetched)
                    psx_spu_source_fetch(&voice->source, ram, &voice->registers[7]);
                sample = 0;
                if (voice->adsr.level) {
                    sample = noisy ? noise[s] : spu_source_interpolate(&voice->source);
                    sample = (sample * voice->adsr.level) >> 15;
                }
                if (voice->adsr.phase != PSX_SPU_ADSR_OFF &&
                    (voice->adsr.phase != PSX_SPU_ADSR_SUSTAIN || voice->adsr.envelope.increment))
                    spu_adsr_tick(&voice->adsr);
                uint16_t pitch = voice->registers[2];
                if (modulated) {
                    int32_t signed_pitch = pitch;
                    if (signed_pitch >= 32768) signed_pitch -= 65536;
                    pitch = (uint16_t)((signed_pitch * (clamp_sample(previous[s]) + 32768)) >> 15);
                }
                current[s] = sample;
                unsigned action = spu_source_advance(&voice->source, pitch, voice->registers[7], noisy);
                if (action & PSX_SPU_SOURCE_END) out->ended |= bit;
                if (action & PSX_SPU_SOURCE_MUTE) psx_spu_adsr_force_off(&voice->adsr);
                for (unsigned channel = 0; channel < 2; ++channel) {
                    int32_t value = (sample * voice->volume[channel].level) >> 15;
                    out->dry[s][channel] += value;
                    if (reverb) out->reverb_send[s][channel] += value;
                    if (voice->volume[channel].active) spu_volume_tick(&voice->volume[channel]);
                }
                if (i == 1 || i == 3) out->capture[s][i >> 1] = clamp_sample(sample);
                port_profile_detail_end(PORT_PROFILE_SPU_EVENT, profile_event);
            }
            voice->last_sample = sample;
        }
        int32_t *swap = previous;
        previous = current;
        current = swap;
    }
}
