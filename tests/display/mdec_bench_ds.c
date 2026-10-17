/* DS timing of MDEC reconstruction on real FF9 movie macroblocks (DSi clock).
 * Input: the user's captured run-level data, linked by .incbin from build/ (never committed).
 * Build: tools/docker.sh make mdec-bench-ds, then boot build/tests/mdec_bench.nds in DSi mode. */
#include <nds.h>
#include <stdio.h>
#include "../../port/psx/mdec_decode.c"

extern const uint8_t bench_data[], bench_data_end[];
static uint8_t pixels[768];

static unsigned ticks(void) { return TIMER_DATA(0) | (unsigned)TIMER_DATA(1) << 16; }

static unsigned us_per(unsigned t, unsigned n) { return (unsigned)((uint64_t)t * 1000000 / 33513982 / n); }

int main(void) {
    consoleDemoInit();
    if (isDSiMode()) setCpuClock(true);
    TIMER_CR(0) = 0; TIMER_CR(1) = 0;
    TIMER_DATA(0) = 0; TIMER_DATA(1) = 0;
    TIMER_CR(1) = TIMER_CASCADE | TIMER_ENABLE;
    TIMER_CR(0) = TIMER_DIV_1 | TIMER_ENABLE; /* bus clock */
    PsxMdecEnv env;
    psx_mdec_env_init(&env, bench_data, bench_data + 128);
    unsigned n = 0;
    static int16_t samples[6][64];
    unsigned t0 = ticks();
    for (const uint8_t *p = bench_data + 256; p + 8 <= bench_data_end;) {
        uint32_t cmd, hw;
        memcpy(&cmd, p, 4); memcpy(&hw, p + 4, 4); p += 8;
        size_t used;
        psx_mdec_macroblock(&env, (const uint16_t *)p, hw, cmd, pixels, sizeof pixels, &used);
        p += hw * 2; n++;
    }
    unsigned whole = ticks() - t0;
    t0 = ticks();
    for (const uint8_t *p = bench_data + 256; p + 8 <= bench_data_end;) {
        uint32_t cmd, hw;
        memcpy(&cmd, p, 4); memcpy(&hw, p + 4, 4); p += 8;
        size_t at = 0;
        for (unsigned i = 0; i < 6; i++) block(&env, env.quant[i < 2], (const uint16_t *)p, hw, &at, samples[i]);
        p += hw * 2;
    }
    unsigned transform = ticks() - t0;
    printf("%s, %u macroblocks\n", isDSiMode() ? "DSi 134 MHz" : "DS 67 MHz", n);
    printf("whole:      %u us/mb\n", us_per(whole, n));
    printf("parse+idct: %u us/mb\n", us_per(transform, n));
    printf("colour:     %u us/mb\n", us_per(whole - transform, n));
    while (1) swiWaitForVBlank();
}
