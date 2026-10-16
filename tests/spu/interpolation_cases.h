#ifndef INTERPOLATION_CASES_H
#define INTERPOLATION_CASES_H
#include <stdint.h>

static void interpolation_case(unsigned scenario, int16_t values[4]) {
    uint32_t random = 0xc179348bu ^ (scenario * 0x12345u);
    for (unsigned i = 0; i < 4; ++i) {
        random = random * 1664525u + 1013904223u;
        int value = random >> 16;
        if (value >= 32768) value -= 65536;
        if (scenario == 0) value = 0;
        else if (scenario == 1) value = 32767;
        else if (scenario == 2) value = -32768;
        else if (scenario == 3) value = (i & 1) ? 32767 : -32768;
        else if (scenario == 4) value = (i & 1) ? -32768 : 32767;
        else if (scenario < 13) value = i == (scenario - 5) % 4 ? (scenario < 9 ? 32767 : -32768) : 0;
        values[i] = value;
    }
}
static uint32_t interpolation_hash(uint32_t hash, int16_t value) {
    return (hash ^ (uint16_t)value) * 16777619u;
}
#endif
