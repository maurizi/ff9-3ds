#ifndef ENVELOPE_CASES_H
#define ENVELOPE_CASES_H
#include <stdint.h>

static const int16_t envelope_initial_levels[7] = {-32768, -1, 0, 1, 24575, 24576, 32767};
/* Include slow rates for long enough to observe at least one counter rollover.
 * For each rate test both ADSR hold masks, all sweep modes, and seven levels. */
static unsigned envelope_case_ticks(unsigned rate) { return rate >= 104 ? 40000 : 512; }
static uint32_t envelope_hash(uint32_t hash, uint32_t value) {
    return (hash ^ value) * 16777619u;
}
#endif
