#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "psx_spu_voice.h"
#include "voice_cases.h"
#include "voice_reference.h"

static PsxSpuVoice voices[PSX_SPU_VOICES];
static uint8_t ram[PSX_SPU_RAM_BYTES];
static uint32_t ended;

void voice_case_write(unsigned voice, unsigned reg, uint16_t value) {
    psx_spu_voice_write(&voices[voice], reg, value);
}
void voice_case_key(unsigned voice, int on) {
    if (on) { psx_spu_voice_key_on(&voices[voice]); ended &= ~(1u << voice); }
    else psx_spu_voice_key_off(&voices[voice]);
}

int main(void) {
    for (unsigned scenario = 0; scenario < VOICE_CASES; ++scenario) {
        memset(voices, 0, sizeof voices);
        memset(ram, 0, sizeof ram);
        voice_case_ram(ram, scenario);
        ended = 0;
        uint32_t hash = 2166136261u;
        for (unsigned frame = 0; frame < VOICE_CASE_FRAMES; ++frame) {
            voice_case_events(scenario, frame);
            PsxSpuVoiceMix mix;
            psx_spu_mix_voices(voices, ram, voice_case_noise(frame), voice_case_noise_mask(scenario),
                               voice_case_modulation(scenario), voice_case_reverb(scenario),
                               scenario >= 4, &mix);
            ended |= mix.ended;
            for (unsigned channel = 0; channel < 2; ++channel) hash = voice_case_hash(hash, mix.dry[channel]);
            for (unsigned channel = 0; channel < 2; ++channel) hash = voice_case_hash(hash, mix.reverb_send[channel]);
            hash = voice_case_hash(hash, (uint16_t)mix.capture[0]);
            hash = voice_case_hash(hash, (uint16_t)mix.capture[1]);
            hash = voice_case_hash(hash, ended);
            for (unsigned i = 0; i < PSX_SPU_VOICES; ++i) {
                const PsxSpuVoice *voice = &voices[i];
                hash = voice_case_hash(hash, psx_spu_voice_read(voice, 6));
                hash = voice_case_hash(hash, psx_spu_voice_read(voice, 7));
                hash = voice_case_hash(hash, voice->source.address);
                hash = voice_case_hash(hash, voice->source.counter);
                hash = voice_case_hash(hash, voice->adsr.phase);
                hash = voice_case_hash(hash, (uint16_t)voice->volume[0].level);
                hash = voice_case_hash(hash, (uint16_t)voice->volume[1].level);
                hash = voice_case_hash(hash, (uint32_t)voice->last_sample);
            }
        }
        if (hash != voice_reference_hashes[scenario]) {
            printf("24-voice scenario %u: %08lx != %08lx\n", scenario,
                   (unsigned long)hash, (unsigned long)voice_reference_hashes[scenario]);
            return 1;
        }
    }
    /* Invalid voice register indices cannot corrupt adjacent state. */
    PsxSpuVoice saved = voices[23];
    psx_spu_voice_write(&voices[23], 8, 0xffff);
    assert(!memcmp(&saved, &voices[23], sizeof saved));
    assert(psx_spu_voice_read(&voices[23], 8) == 0xffff);
    puts("SPU mix: 24 voices, ADSR, PMON, sweeps, loop/end state PASS");
    return 0;
}
