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
        if (adsr.phase == PSX_SPU_ADSR_OFF && !fetch_off) break;
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
        counter += pitch;
        if (counter >= (PSX_SPU_BLOCK_SAMPLES << 12)) {
            /* spu_source_advance's block step; then the fetch the next sample would do first. */
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
        int32_t l = (sample * left) >> 15, r = (sample * right) >> 15;
        dry[s][0] += l;
        dry[s][1] += r;
        if (send) send[s][0] += l, send[s][1] += r;
        if (capture) capture[s][capture_channel] = clamp_sample(sample);
    }
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
