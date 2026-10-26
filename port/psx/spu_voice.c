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

/* The Gaussian coefficients a source phase uses, in sample order: {g[255-p], g[511-p], g[256+p], g[p]}.
 * The same values and summation order as spu_source_interpolate; one row per phase keeps them together. */
static int16_t gauss4[256][4];
/* The same rows as halfword pairs, for the steady loops: {g0 | g1 << 16, g2 | g3 << 16} (16-bit multiplies). */
static uint32_t gauss4w[256][2];
static int gauss4_ready;
static void gauss4_init(void) {
    for (unsigned p = 0; p < 256; ++p) {
        gauss4[p][0] = psx_spu_gauss[255 - p];
        gauss4[p][1] = psx_spu_gauss[511 - p];
        gauss4[p][2] = psx_spu_gauss[256 + p];
        gauss4[p][3] = psx_spu_gauss[p];
        gauss4w[p][0] = (uint16_t)gauss4[p][0] | (uint32_t)(uint16_t)gauss4[p][1] << 16;
        gauss4w[p][1] = (uint16_t)gauss4[p][2] | (uint32_t)(uint16_t)gauss4[p][3] << 16;
    }
    gauss4_ready = 1;
}

/* gauss_sample's value, its envelope product and the two volume products, all 16 x 16 bits: the interpolated
 * sample, the envelope level and both volume levels are halfwords (`volumes`: left | right << 16). */
static inline __attribute__((always_inline)) int32_t steady_sample(const int16_t *samples, uint32_t counter,
                                                                   int32_t level) {
    const int16_t *at = samples + (counter >> 12);
    const uint32_t *c = gauss4w[(counter >> 4) & 255];
    int32_t g = at[0] * (int16_t)c[0] + at[1] * (int16_t)(c[0] >> 16) +
                at[2] * (int16_t)c[1] + at[3] * (int16_t)(c[1] >> 16);
    return ((int16_t)(g >> 15) * (int16_t)level) >> 15;
}

static inline __attribute__((always_inline)) int32_t gauss_sample(const int16_t *samples, uint32_t counter) {
    const int16_t *at = samples + (counter >> 12), *c = gauss4[(counter >> 4) & 255];
    return at[0] * c[0] + at[1] * c[1] + at[2] * c[2] + at[3] * c[3];
}

/* A voice without noise, pitch modulation or a volume sweep, from sample s to the span's end: exactly the
 * single-sample mixer's steps. The source position and the whole ADSR state are kept in locals whose address
 * never escapes, so stores to the mix buffers don't force reloads; the voice struct is written back around the
 * rare out-of-line steps (block crossing and fetch, ADSR phase change, mute). Stops early only when the voice
 * switched off and off voices don't fetch (the caller's per-sample loop then skips it). Returns the next sample. */
static SPU_HOT __attribute__((noinline)) unsigned fast_span(PsxSpuVoice *restrict voice, unsigned s, unsigned n,
        const uint8_t *ram, int32_t (*restrict dry)[2], int32_t (*restrict send)[2], int32_t *restrict keep,
        int16_t (*restrict capture)[2], unsigned capture_channel, int fetch_off, uint32_t bit,
        uint32_t *restrict ended, int32_t *restrict last) {
    PsxSpuSource *src = &voice->source;
    const uint32_t pitch = voice->registers[2] > 0x3fff ? 0x3fff : voice->registers[2];
    const int32_t left = voice->volume[0].level, right = voice->volume[1].level;
    const int16_t *samples = src->samples;
    PsxSpuAdsr adsr = voice->adsr;
    uint32_t counter = src->counter;
    int32_t sample = *last;
    if (!src->fetched) {
        psx_spu_source_fetch(src, ram, &voice->registers[7]);
    }
    for (; s < n; ++s) {
        if (adsr.phase == PSX_SPU_ADSR_OFF && !fetch_off) goto done;
        if (!keep && !capture &&
            (adsr.phase == PSX_SPU_ADSR_OFF || (adsr.phase == PSX_SPU_ADSR_SUSTAIN && !adsr.envelope.increment))) {
            /* The envelope holds: run to the block crossing or the span's end with nothing else per sample. */
            const int32_t level = adsr.level;
            if (!level) {
                sample = 0; /* contributes nothing: only the position moves */
                for (;;) {
                    counter += pitch;
                    if (counter >= (PSX_SPU_BLOCK_SAMPLES << 12)) break;
                    if (++s == n) goto done;
                }
            } else {
                /* Pointer-walked, so the loop keeps everything in registers. */
                const uint32_t volumes = (uint16_t)left | (uint32_t)(uint16_t)right << 16;
                int32_t (*d)[2] = dry + s, (*const end)[2] = dry + n;
                if (send) {
                    int32_t (*w)[2] = send + s;
                    for (;;) {
                        sample = steady_sample(samples, counter, level);
                        int32_t l = (sample * (int16_t)volumes) >> 15, r = (sample * (int16_t)(volumes >> 16)) >> 15;
                        (*d)[0] += l; (*d)[1] += r;
                        (*w)[0] += l; (*w)[1] += r;
                        counter += pitch;
                        if (counter >= (PSX_SPU_BLOCK_SAMPLES << 12)) break;
                        ++w;
                        if (++d == end) { s = n; goto done; }
                    }
                } else {
                    for (;;) {
                        sample = steady_sample(samples, counter, level);
                        (*d)[0] += (sample * (int16_t)volumes) >> 15;
                        (*d)[1] += (sample * (int16_t)(volumes >> 16)) >> 15;
                        counter += pitch;
                        if (counter >= (PSX_SPU_BLOCK_SAMPLES << 12)) break;
                        if (++d == end) { s = n; goto done; }
                    }
                }
                s = (unsigned)(d - dry);
            }
        } else {
            sample = 0;
            if (adsr.level) sample = ((int16_t)(gauss_sample(samples, counter) >> 15) * adsr.level) >> 15;
            /* spu_adsr_tick, on the local copy; the phase change itself runs on the voice. */
            if (adsr.phase != PSX_SPU_ADSR_OFF && (adsr.phase != PSX_SPU_ADSR_SUSTAIN || adsr.envelope.increment)) {
                if (adsr.envelope.increment) spu_envelope_tick(&adsr.envelope, &adsr.level);
                if (adsr.phase != PSX_SPU_ADSR_SUSTAIN &&
                    (adsr.envelope.decrease ? adsr.level <= adsr.target : adsr.level >= adsr.target)) {
                    voice->adsr = adsr;
                    psx_spu_adsr_advance_phase(&voice->adsr);
                    adsr = voice->adsr;
                }
            }
            if (keep) keep[s] = sample;
            int32_t l = (sample * left) >> 15, r = (sample * right) >> 15;
            dry[s][0] += l;
            dry[s][1] += r;
            if (send) send[s][0] += l, send[s][1] += r;
            if (capture) capture[s][capture_channel] = clamp_sample(sample);
            counter += pitch;
            if (counter < (PSX_SPU_BLOCK_SAMPLES << 12)) continue;
        }
        /* spu_source_advance's block step for sample s; then the fetch the next sample would do first. Mixing
         * doesn't depend on it, so it may follow the sample's mix. */
        src->counter = counter - pitch;
        unsigned action = spu_source_advance(src, pitch, voice->registers[7], 0);
        if (action & PSX_SPU_SOURCE_END) *ended |= bit;
        if (action & PSX_SPU_SOURCE_MUTE) {
            voice->adsr = adsr;
            psx_spu_adsr_force_off(&voice->adsr);
            adsr = voice->adsr;
        }
        counter = src->counter;
        if (s + 1 < n && (adsr.phase != PSX_SPU_ADSR_OFF || fetch_off))
            psx_spu_source_fetch(src, ram, &voice->registers[7]);
    }
done:
    src->counter = counter;
    voice->adsr = adsr;
    *last = sample;
    return s;
}

/* The same arithmetic as psx_spu_mix_voices, for n consecutive samples at once: each voice runs its whole span
 * before the next, with its state kept local. Pitch modulation reads the previous voice's post-ADSR samples
 * from a buffer, so voice order and values are identical. The caller applies key events before the span and
 * must ensure no voice fetches SPU RAM that capture or reverb writes during it (spu_core.c). */
SPU_HOT void psx_spu_mix_voices_block(PsxSpuVoice voices[PSX_SPU_VOICES], const uint8_t *ram, unsigned n,
                                      const int16_t *noise, uint32_t noise_mask, uint32_t modulation_mask,
                                      uint32_t reverb_mask, int fetch_off_voices, PsxSpuVoiceBlock *out) {
    static int32_t previous_buffer[2][PSX_SPU_BLOCK_SPAN]; /* static: the DS stack is the small DTCM */
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
                if (voice->adsr.phase == PSX_SPU_ADSR_OFF && !fetch_off_voices) {
                    /* Ended inside the span: skipped from here on, as in the single-sample mixer. */
                    current[s] = sample = 0;
                    continue;
                }
                if (!noisy && !modulated && !voice->volume[0].active && !voice->volume[1].active) {
                    unsigned profile_steady = port_profile_begin();
                    unsigned next = fast_span(voice, s, n, ram, out->dry, reverb ? out->reverb_send : NULL,
                                              keep ? current : NULL, (i == 1 || i == 3) ? out->capture : NULL,
                                              i >> 1, fetch_off_voices, bit, &out->ended, &sample);
                    port_profile_detail_end(PORT_PROFILE_SPU_STEADY, profile_steady);
                    s = next - 1; /* the loop's ++s: next is n, or a switched-off voice's next sample */
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

SPU_HW_ONLY_HOT void psx_spu_step_voice(PsxSpuVoice *voice, unsigned index, const uint8_t *ram, unsigned n, int fetch_off,
                                uint32_t *ended, void (*block)(unsigned voice, const int16_t *samples, void *user),
                                void *user) {
    PsxSpuSource *src = &voice->source;
    const uint32_t pitch = voice->registers[2] > 0x3fff ? 0x3fff : voice->registers[2];
    const uint32_t bit = 1u << index;
    if (voice->adsr.phase == PSX_SPU_ADSR_OFF && !fetch_off) return;
    for (unsigned c = 0; c < 2; ++c) /* volume sweeps advance per sample whatever the source does */
        for (unsigned k = 0; k < n && voice->volume[c].active; ++k) psx_spu_volume_tick(&voice->volume[c]);
    for (unsigned s = 0; s < n;) {
        if (voice->adsr.phase == PSX_SPU_ADSR_OFF && !fetch_off) return;
        if (!src->fetched) {
            unsigned t = port_profile_begin();
            psx_spu_source_fetch(src, ram, &voice->registers[7]);
            block(index, src->samples + 3, user);
            port_profile_detail_end(PORT_PROFILE_SPU_STEADY, t);
        }
        PsxSpuAdsr *adsr = &voice->adsr;
        int steady = adsr->phase == PSX_SPU_ADSR_OFF ||
                     (adsr->phase == PSX_SPU_ADSR_SUSTAIN && !adsr->envelope.increment);
        unsigned run;
        if (steady) {
            /* jump to the sample that crosses into the next block, or the end */
            if (!pitch) return;
            uint32_t left = (PSX_SPU_BLOCK_SAMPLES << 12) - src->counter;
            run = (left + pitch - 1) / pitch;
            if (run > n - s) { src->counter += pitch * (n - s); return; }
        } else {
            /* one sample with the envelope step (as fast_span's per-sample path) */
            if (adsr->envelope.increment) spu_envelope_tick(&adsr->envelope, &adsr->level);
            int advanced = 0;
            if (adsr->phase != PSX_SPU_ADSR_SUSTAIN &&
                (adsr->envelope.decrease ? adsr->level <= adsr->target : adsr->level >= adsr->target)) {
                psx_spu_adsr_advance_phase(adsr);
                advanced = 1;
            }
            run = 1;
            if (src->counter + pitch < (PSX_SPU_BLOCK_SAMPLES << 12)) {
                src->counter += pitch;
                ++s;
                /* Until the envelope's next level change, a sample only adds to the envelope's and the source's
                 * counters (the phase test sees the same level; it failed just now): skip to the sample before
                 * that change, or before the source crosses into its next block. */
                PsxSpuEnvelope *env = &adsr->envelope;
                if (!advanced && env->increment && env->counter < 0x8000 && s < n) {
                    uint32_t inc = env->increment;
                    if (env->exponential && !env->decrease && adsr->level >= 0x6000) {
                        if (env->rate >= 44) inc >>= 2;
                        else if (env->rate >= 40) inc >>= 1;
                    }
                    if (inc && inc < 0x8000) {
                        uint32_t change = (0x8000 - env->counter + inc - 1) / inc; /* the tick that changes it */
                        uint32_t cross = pitch ? ((PSX_SPU_BLOCK_SAMPLES << 12) - src->counter + pitch - 1) / pitch
                                               : 0xFFFFFFFFu;
                        uint32_t skip = change - 1;
                        if (skip > cross - 1) skip = cross - 1;
                        if (skip > n - s) skip = n - s;
                        env->counter += inc * skip;
                        src->counter += pitch * skip;
                        s += skip;
                    }
                }
                continue;
            }
        }
        /* the crossing sample: everything before it moved the counter, this one steps the block */
        src->counter += pitch * (run - 1);
        s += run;
        unsigned action = spu_source_advance(src, pitch, voice->registers[7], 0);
        if (action & PSX_SPU_SOURCE_END) *ended |= bit;
        if (action & PSX_SPU_SOURCE_MUTE) psx_spu_adsr_force_off(&voice->adsr);
    }
}
