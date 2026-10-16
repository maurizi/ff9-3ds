#ifndef ADPCM_CASES_H
#define ADPCM_CASES_H
#include <stdint.h>

/* Synthetic input shared with the independent reference harness, not a decoder. */
static const int16_t adpcm_initial_history[5][2] = {
    {0, 0}, {-1, 1}, {32767, -32768}, {-32768, 32767}, {12345, -23456}
};
static uint32_t adpcm_seed(unsigned header, unsigned scenario) {
    return 0x139ade7bu ^ (header * 0x10203u) ^ (scenario * 0x34567u);
}
static void adpcm_case_block(uint8_t block[16], unsigned header, uint32_t *random) {
    block[0] = header;
    for (unsigned i = 1; i < 16; ++i) {
        *random = *random * 1664525u + 1013904223u;
        block[i] = *random >> 24;
    }
}
static uint32_t adpcm_hash(uint32_t hash, unsigned value) {
    return (hash ^ value) * 16777619u;
}
#endif
