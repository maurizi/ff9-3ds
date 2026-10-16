#ifndef VOICE_CASES_H
#define VOICE_CASES_H
#include <stdint.h>

/* Synthetic input events shared with the independent reference harness. */
enum { VOICE_CASES = 8, VOICE_CASE_FRAMES = 2048 };
void voice_case_write(unsigned voice, unsigned reg, uint16_t value);
void voice_case_key(unsigned voice, int on);

static void voice_case_ram(uint8_t ram[512 * 1024], unsigned scenario) {
    uint32_t random = 0x1927fea1u ^ scenario;
    for (unsigned voice = 0; voice < 24; ++voice) {
        for (unsigned block = 0; block < 16; ++block) {
            uint8_t *p = ram + 0x1000 + voice * 0x100 + block * 16;
            p[0] = (((voice + block) % 5) << 4) | (4 + (voice + scenario) % 9);
            p[1] = block == 0 ? 4 : block == 15 ? ((voice + scenario) % 3 ? 3 : 1) : 0;
            for (unsigned i = 2; i < 16; ++i) {
                random = random * 1664525u + 1013904223u;
                p[i] = random >> 24;
            }
        }
    }
}

static void voice_case_events(unsigned scenario, unsigned frame) {
    static const uint16_t pitches[] = {0, 1, 0x801, 0x1000, 0x17ff, 0x3fff, 0x4000, 0x7fff, 0x8000, 0xffff};
    for (unsigned voice = 0; voice < 24; ++voice) {
        if (frame == 0) {
            voice_case_write(voice, 0, (voice * 0x587 + scenario * 77) & 0x7fff);
            voice_case_write(voice, 1, (voice * 0x921 + 0x2010) & 0x7fff);
            voice_case_write(voice, 2, pitches[(voice + scenario) % 10]);
            voice_case_write(voice, 3, (0x1000 + voice * 0x100) / 8);
            voice_case_write(voice, 4, (voice % 3 ? 0x0800 : 0x8800) | ((voice % 8) << 4) | (voice & 15));
            voice_case_write(voice, 5, ((voice % 4) << 6) | (voice % 3 ? 0x4000 : 0x8000) | 7);
            voice_case_write(voice, 7, (0x1000 + voice * 0x100) / 8);
            voice_case_key(voice, 1);
        }
        if (frame == 13 && voice % 5 == 0) voice_case_key(voice, 0);
        if (frame == 64 && voice % 7 == 0) voice_case_key(voice, 0);
        if (frame == 256 && voice >= 1 && voice <= 8) voice_case_key(voice, 1);
        if (frame == 400 && voice % 4 == 0) voice_case_write(voice, 6, 0x8000);
        if (frame == 600) voice_case_write(voice, 5, 0x1fc7);
        if (frame == 700) voice_case_write(voice, 7, (0x1080 + voice * 0x100) / 8);
        if (frame == 701) voice_case_write(voice, 7, (0x1080 + voice * 0x100) / 8);
        if (frame == 800 && voice % 3 == 0) voice_case_write(voice, 3, (0x1040 + voice * 0x100) / 8);
        if (frame == 900) {
            voice_case_write(voice, 0, voice % 2 ? 0x9030 : 0x3fff);
            voice_case_write(voice, 1, voice % 2 ? 0xe02c : 0x4000);
        }
        if (frame == 1000 && voice % 2 == 0) voice_case_key(voice, 1);
        if (frame == 1300) voice_case_key(voice, 0);
    }
}

static uint32_t voice_case_noise_mask(unsigned scenario) { return scenario & 1 ? 0xa59a53u : 0; }
static uint32_t voice_case_modulation(unsigned scenario) { return scenario & 2 ? 0xffffffu : 1u; }
static uint32_t voice_case_reverb(unsigned scenario) { return scenario & 4 ? 0xffffffu : 0x123456u; }
static int16_t voice_case_noise(unsigned frame) { return (int16_t)((frame * 0x721u) ^ 0xb421u); }
static uint32_t voice_case_hash(uint32_t hash, uint32_t value) { return (hash ^ value) * 16777619u; }
#endif
