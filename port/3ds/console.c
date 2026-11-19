/* The log: every line to svcOutputDebugString (Azahar writes it to its log; tools/azahar.sh collects it), to
 * sdmc:/3ds/ff9/log.txt, and, when the game was sent with `3dslink -s`, over the network to the PC that sent it (its
 * stderr redirected there: tools/3dslink.sh shows it live); the last lines are kept for the bottom screen, which shows
 * them only after a fatal stop (the renderer owns it before). */
#include <3ds.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>

#include "ctr.h"

#define KEEP 28
static char lines[KEEP][64];
static int next_line;
static PrintConsole console;
static int shown;
static FILE *logfile; /* sdmc:/3ds/ff9/log.txt: the same lines, buffered (tools/azahar.sh reads it) */
static u64 log_flushed;  /* (the system tick of its last write to the card) */
static int netlog;    /* stderr goes to the 3dslink host */

void ctr_console_init(void) {
    logfile = fopen("sdmc:/3ds/ff9/log.txt", "w");
    if (logfile) setvbuf(logfile, NULL, _IOFBF, 16 << 10);
    if (__3dslink_host.s_addr) { /* (sent by 3dslink: its host listens with -s) */
        static u32 *soc;
        soc = memalign(0x1000, 0x40000);
        if (soc && R_SUCCEEDED(socInit(soc, 0x40000)) && link3dsStdioForDebug() >= 0) netlog = 1;
    }
}

void ctr_console_vlog(const char *fmt, va_list ap) {
    static char line[512];
    static int len;
    if (shown) {
        va_list copy;
        va_copy(copy, ap);
        vprintf(fmt, copy);
        va_end(copy);
    }
    int n = vsnprintf(line + len, sizeof line - len, fmt, ap);
    if (n > 0) len += n < (int)sizeof line - len ? n : (int)sizeof line - 1 - len;
    if (len && (line[len - 1] == '\n' || len >= (int)sizeof line - 1)) {
        svcOutputDebugString(line, len);
        /* (written to the card at most every 5 s, or at once for a fatal stop or the watchdog: each write blocks the game's
         * thread on the SD card, and a write per line (two or three a heartbeat) held up frames enough that the sound
         * queue ran dry, the music skipping (the user, on a 3DS)) */
        if (logfile) {
            fwrite(line, 1, len, logfile);
            u64 now = svcGetSystemTick();
            if (now - log_flushed > 5ull * SYSCLOCK_ARM11 || !strncmp(line, "FATAL", 5) || !strncmp(line, "\nFATAL", 6) ||
                !strncmp(line, "WATCHDOG", 8)) {
                fflush(logfile);
                log_flushed = now;
            }
        }
        if (netlog) { fwrite(line, 1, len, stderr); fflush(stderr); }
#ifdef PORT_AGENT
        { void ctr_agent_log(const char *, int); ctr_agent_log(line, len); } /* (to the PC with the job's results: agent.c) */
#endif
        snprintf(lines[next_line], sizeof lines[0], "%.*s", len, line);
        next_line = (next_line + 1) % KEEP;
        len = 0;
    }
}

void ctr_console_show(void) {
    if (shown) return;
    consoleInit(GFX_BOTTOM, &console);
    shown = 1;
    for (int i = 0; i < KEEP; i++) {
        const char *l = lines[(next_line + i) % KEEP];
        if (*l) printf("%s%s", l, l[strlen(l) - 1] == '\n' ? "" : "\n");
    }
}

/* a line written at once from any thread (the watchdog's): no shared buffer */
void ctr_console_raw(const char *text, int n) {
    svcOutputDebugString(text, n);
    if (logfile) { fwrite(text, 1, n, logfile); fflush(logfile); }
    if (netlog) { fwrite(text, 1, n, stderr); fflush(stderr); }
#ifdef PORT_AGENT
    { void ctr_agent_log(const char *, int); ctr_agent_log(text, n); }
#endif
}
