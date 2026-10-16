/* Explicit little-endian, sample-indexed SPU journal; no game data is embedded.
 * --spu-trace enables mixing even without --audio. See audio-validation.md. */
#include <stdio.h>
#include <stdlib.h>
#include "psx_spu.h"
#include "psx_spu_trace.h"
static FILE *journal;
static int write_failed;
static void write_bytes(const void *data, unsigned count) {
    if (fwrite(data, 1, count, journal) != count) {
        write_failed = 1;
        psx_spu_set_trace(NULL);
        perror("SPU trace write");
        exit(2);
    }
}
static void u32(unsigned value) {
    unsigned char bytes[4] = {value, value >> 8, value >> 16, value >> 24};
    write_bytes(bytes, 4);
}
static void event(unsigned type, unsigned a, unsigned b, const void *data) {
    u32(type);
    switch (type) {
    case PSX_SPU_TRACE_RESET: u32(0); break;
    case PSX_SPU_TRACE_WRITE: u32(8); u32(a); u32(b); break;
    case PSX_SPU_TRACE_RAM:
        u32(8 + b); u32(a); u32(b); write_bytes(data, b); break;
    case PSX_SPU_TRACE_SAMPLE: {
        const int32_t *samples = data;
        u32(32);
        for (unsigned i = 0; i < 8; ++i) u32(samples[i]);
        break;
    }
    }
}
static void close_trace(void) {
    if (!journal) return;
    psx_spu_set_trace(NULL);
    if (write_failed) { fclose(journal); journal = NULL; return; }
    u32(0xffffffffu); u32(0); /* completed, unlike an abruptly truncated run */
    if (fclose(journal)) { perror("SPU trace close"); journal = NULL; exit(2); }
    journal = NULL;
}
int plat_spu_trace_open(const char *path) {
    if (journal) { fprintf(stderr, "SPU trace already open\n"); return 0; }
    journal = fopen(path, "wbx");
    if (!journal) { perror("SPU trace open"); return 0; }
    write_bytes("FF9SPU1\0", 8);
    atexit(close_trace);
    psx_spu_set_trace(event);
    psx_spu_set_rendering(1);
    return 1;
}
