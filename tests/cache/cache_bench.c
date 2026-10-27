/* Does the emulator model the ARM9 data cache? The same count of word loads from a 4 KB array (cache-resident), a
 * 1 MB array read through (each 32-byte line once, 8 loads a line) and the 1 MB array a line a load (a miss each
 * time on hardware). Timer 2/3 ticks (DIV_64: 1.91 us) in `results`; `frames` set when done (tools/ds_profile.py). */
#include <nds.h>

volatile u32 frames;
volatile u32 results[8];
static u32 big[256 * 1024]; /* 1 MB */
static u32 small[1024];     /* 4 KB */

static u32 now(void) { return TIMER_DATA(2) | (u32)TIMER_DATA(3) << 16; }

int main(void) {
    TIMER_CR(2) = 0; TIMER_CR(3) = 0; TIMER_DATA(2) = 0; TIMER_DATA(3) = 0;
    TIMER_CR(3) = TIMER_ENABLE | TIMER_CASCADE;
    TIMER_CR(2) = TIMER_ENABLE | TIMER_DIV_64;
    for (u32 i = 0; i < 256 * 1024; ++i) big[i] = i;
    for (u32 i = 0; i < 1024; ++i) small[i] = i;
    const u32 loads = 256 * 1024;
    volatile u32 sink = 0;
    u32 s = 0, t0 = now();
    for (u32 r = 0; r < loads / 1024; ++r) for (u32 i = 0; i < 1024; ++i) s += small[i];
    results[0] = now() - t0; sink += s;
    s = 0; t0 = now();
    for (u32 i = 0; i < loads; ++i) s += big[i];
    results[1] = now() - t0; sink += s;
    s = 0; t0 = now();
    for (u32 r = 0; r < 8; ++r) for (u32 i = r; i < loads; i += 8) s += big[i];
    results[2] = now() - t0; sink += s;
    results[3] = loads;
    results[7] = 0x43414348; /* "CACH" */
    frames = 100;
    for (;;) swiWaitForVBlank();
}
