#ifndef REVERB_CASES_H
#define REVERB_CASES_H
#include <stdint.h>
#include "reverb_presets.h"

enum { REVERB_CASES = 32, REVERB_FRAMES = 32768 };
static uint32_t reverb_random(uint32_t *state) {
    *state = *state * 1664525u + 1013904223u;
    return *state;
}
static uint16_t reverb_case_base(unsigned scenario) {
    if (scenario < 10) return (0x80000 - reverb_presets[scenario].bytes) / 8;
    static const uint16_t bases[6] = {0, 1, 0x8000, 0xff00, 0xfffe, 0xffff};
    return bases[(scenario - 10) % 6];
}
static void reverb_case_init(unsigned scenario, uint16_t regs[32], uint8_t ram[512*1024]) {
    uint32_t random = 0xae751d23u ^ scenario;
    for (unsigned i = 0; i < 32; ++i)
        regs[i] = scenario < 10 ? reverb_presets[scenario].registers[i] : reverb_random(&random) >> 8;
    if (scenario >= 10 && scenario % 2) regs[2] = 0x8000; /* IIR exceptional coefficient */
    if (scenario >= 10 && scenario % 3) regs[8] = regs[9] = 0x8000;
    if (scenario >= 24) { /* overlapping RAM reads/writes, including previous-word address */
        for (unsigned i = 10; i < 30; ++i) regs[i] = (i & 1) ? 0 : 0xffff;
    }
    for (unsigned i = 0; i < 512*1024; ++i)
        ram[i] = scenario < 10 ? 0 : scenario == 31 ? (i & 1 ? 0x80 : 0) : reverb_random(&random) >> 24;
}
static int reverb_case_base_event(unsigned scenario, unsigned frame, uint16_t *value) {
    if (scenario >= 10 && (frame == 12347 || frame == 24000)) {
        *value = frame == 12347 ? 0xfff0 : reverb_case_base(scenario);
        return 1;
    }
    return 0;
}
static void reverb_case_input(unsigned scenario, unsigned frame, int16_t input[2]) {
    uint32_t random = 0x258acfe1u ^ (scenario * 0x12345u) ^ frame;
    for (unsigned ch = 0; ch < 2; ++ch) {
        uint16_t value = reverb_random(&random) >> 16;
        input[ch] = frame < 4000 ? (int16_t)value : 0;
        if (scenario < 10 && frame < 32) input[ch] = frame == ch ? 32767 : 0;
    }
}
static int reverb_case_enable(unsigned scenario, unsigned frame) {
    return scenario % 4 == 0 ? 0 : !(frame >= 5000 && frame < 7000);
}
static uint32_t reverb_case_hash(uint32_t hash, uint32_t value) {
    return (hash ^ value) * 16777619u;
}
#endif
