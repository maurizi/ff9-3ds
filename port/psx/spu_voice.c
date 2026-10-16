#include "psx_spu_voice.h"
#include "spu_hot.h"
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
