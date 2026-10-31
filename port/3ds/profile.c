/* PROFILE=1: frame times and per-category timers (profile.h), from the ARM11's system tick (268 MHz on both
 * models; the New 3DS's CPU runs at 804 MHz but the tick doesn't). Every 128 frames the averages per VBlank go to
 * the bottom screen and the log:  "busy 12.3 | gpu 4.1 gte 1.2 ... ms". */
#include <3ds.h>
#include <stdio.h>
#include <string.h>

#include "ctr.h"
#include "platform.h"
#include "profile.h"
#include "psx_runtime.h"

#ifdef PORT_PROFILE
#define WINDOW 128
#define TICKS_PER_MS (SYSCLOCK_ARM11 / 1000.0)

static const char *const names[PORT_PROFILE_CATEGORIES] = {"gpu", "gte", "vm", "scale", "wait"};
static const char *const detail_names[PORT_PROFILE_DETAIL_CATEGORIES] = {
    "tex", "upload", "pal", "mesh", "ot", "spu", "sink", "vlc", "mdec", "voices", "spuout", "steady", "event",
    "fv", "fvwait", "fvread", "cd"};
static u64 acc[PORT_PROFILE_CATEGORIES], detail[PORT_PROFILE_DETAIL_CATEGORIES], draw_acc;
static u64 window_start;
static unsigned frames;

static inline unsigned now32(void) { return (unsigned)svcGetSystemTick(); }

unsigned int port_profile_begin(void) { return now32(); }
void port_profile_end(unsigned int category, unsigned int start) { acc[category] += now32() - start; }
void port_profile_draw_end(unsigned int kind, unsigned int start) { (void)kind; draw_acc += now32() - start; }
void port_profile_detail_end(unsigned int category, unsigned int start) { detail[category] += now32() - start; }
unsigned int port_profile_wait_begin(void) { return now32(); }
void port_profile_wait_end(unsigned int start) { acc[PORT_PROFILE_WAIT] += now32() - start; }
void port_profile_init(void) { window_start = svcGetSystemTick(); }
void port_profile_frame(unsigned int mode, unsigned int frame) { (void)mode; (void)frame; }

void ctr_profile_frame(void) {
    if (!window_start) window_start = svcGetSystemTick();
    if (++frames < WINDOW) return;
    u64 total = svcGetSystemTick() - window_start;
    char line[256];
    int n = snprintf(line, sizeof line, "%.1f ms/VBl busy %.1f |", total / (double)frames / TICKS_PER_MS,
                     (total - acc[PORT_PROFILE_WAIT]) / (double)frames / TICKS_PER_MS);
    for (int i = 0; i < PORT_PROFILE_CATEGORIES; i++)
        if (i != PORT_PROFILE_WAIT && acc[i] && n < (int)sizeof line - 24)
            n += snprintf(line + n, sizeof line - n, " %s %.1f", names[i], acc[i] / (double)frames / TICKS_PER_MS);
    for (int i = 0; i < PORT_PROFILE_DETAIL_CATEGORIES; i++)
        if (detail[i] / frames > TICKS_PER_MS / 10 && n < (int)sizeof line - 24)
            n += snprintf(line + n, sizeof line - n, " %s %.1f", detail_names[i], detail[i] / (double)frames / TICKS_PER_MS);
    port_log("[prof] %s\n", line);
    memset(acc, 0, sizeof acc);
    memset(detail, 0, sizeof detail);
    draw_acc = 0;
    frames = 0;
    window_start = svcGetSystemTick();
}
#else
void ctr_profile_frame(void) {}
#endif
