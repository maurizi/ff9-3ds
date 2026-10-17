/* Replay a captured moment of the game's SPU (host --spu-snapshot) and time it: the real voice, envelope and
 * reverb workload in a few seconds, on the DS or the host. The output hash checks that optimisations keep the
 * PCM identical. The snapshot holds game data: build/ only, never committed.
 * DS: make spu-snapshot-ds SPU_SNAPSHOT=PATH; host: make build/tests/spu_snapshot_host, run with PATH. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "psx_spu.h"
#include "profile.h"
#ifdef ARM9
#include <nds.h>
#else
#include <stdlib.h>
#include <time.h>
#endif

unsigned short psx_spu_regs[0x200];
static unsigned char ram[512 * 1024];
unsigned char *psx_spu_ram = ram;
static uint32_t hash = 2166136261u, frames;
void plat_audio_samples(const short *pcm, unsigned count) {
    frames += count;
    for (unsigned i = 0; i < count * 2; ++i) hash = (hash ^ (uint16_t)pcm[i]) * 16777619u;
}

/* Component timing through the production profile hooks (built with PORT_PROFILE). */
static uint32_t detail[PORT_PROFILE_DETAIL_CATEGORIES];
#ifdef ARM9
unsigned int port_profile_begin(void) { return TIMER_DATA(2) | (unsigned)TIMER_DATA(3) << 16; }
#else
unsigned int port_profile_begin(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (unsigned)(t.tv_sec * 1000000u + t.tv_nsec / 1000); /* microseconds */
}
#endif
void port_profile_end(unsigned int category, unsigned int start) { (void)category; (void)start; }
void port_profile_draw_end(unsigned int kind, unsigned int start) { (void)kind; (void)start; }
void port_profile_detail_end(unsigned int category, unsigned int start) {
    detail[category] += port_profile_begin() - start;
}

#ifdef ARM9
extern const uint8_t snapshot_data[], snapshot_data_end[];
#endif

int main(int argc, char **argv) {
    const uint8_t *data;
    size_t size;
#ifdef ARM9
    consoleDemoInit();
    if (isDSiMode()) setCpuClock(true);
    TIMER_DATA(2) = 0; TIMER_DATA(3) = 0;
    TIMER_CR(3) = TIMER_CASCADE | TIMER_ENABLE;
    TIMER_CR(2) = TIMER_DIV_64 | TIMER_ENABLE;
    data = snapshot_data; size = snapshot_data_end - snapshot_data;
    (void)argc; (void)argv;
    const double tick_ms = 64.0 * 1000.0 / BUS_CLOCK;
#else
    if (argc != 2) { fprintf(stderr, "usage: %s SNAPSHOT\n", argv[0]); return 1; }
    static uint8_t file[1 << 20];
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    size = fread(file, 1, sizeof file, f); fclose(f); data = file;
    const double tick_ms = 0.001;
#endif
    unsigned state = psx_spu_snapshot_size();
    if (size < state + sizeof ram + 4 || !psx_spu_restore(data)) {
        printf("bad snapshot (%u + %u bytes expected)\n", state, (unsigned)sizeof ram);
        return 1;
    }
    memcpy(ram, data + state, sizeof ram);
    psx_spu_set_rendering(1);
    /* Replay the recorded writes and uploads at their output samples. */
    const uint8_t *event = data + state + sizeof ram, *end = data + size;
    enum { FRAMES = 120, SAMPLES = FRAMES * 735 };
    unsigned done = 0, events = 0;
    unsigned start = port_profile_begin();
    for (;;) {
        uint32_t head[4];
        if (event + 4 > end) break;
        memcpy(head, event, 4);
        uint32_t at = head[0] > SAMPLES ? SAMPLES : head[0];
        if (at > done) psx_spu_advance(96 * (at - done)), done = at;
        if (head[0] == 0xffffffffu || head[0] >= SAMPLES || event + 16 > end) break;
        memcpy(head, event, 16);
        event += 16;
        if (head[1] == 1) psx_spu_write(head[2], (uint16_t)head[3]);
        else if (head[1] == 2 && head[2] + head[3] <= sizeof ram && event + head[3] <= end) {
            memcpy(ram + head[2], event, head[3]);
            event += (head[3] + 3) & ~3u;
        }
        events++;
    }
    if (done < SAMPLES) psx_spu_advance(96 * (SAMPLES - done));
    unsigned total = port_profile_begin() - start;
    printf("%u events replayed\n", events);
    printf("SPU snapshot: %u frames\n%08lx\n", (unsigned)frames, (unsigned long)hash);
    printf("total   %.3f ms/frame\n", total * tick_ms / FRAMES);
    printf("voices  %.3f ms/frame\n", detail[PORT_PROFILE_SPU_VOICES] * tick_ms / FRAMES);
    printf("output  %.3f ms/frame\n", detail[PORT_PROFILE_SPU_OUTPUT] * tick_ms / FRAMES);
#ifdef ARM9
    while (1) swiWaitForVBlank();
#endif
    return 0;
}
